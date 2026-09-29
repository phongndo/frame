from __future__ import annotations

import json
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

from tests.support.mux_harness import (
    FrameServer,
    process_exists,
    wait_for_process_exit,
)

CORPUS = Path("tests/sim/corpus/mux")
RESIZE_DIRECTIONS = {7: "left", 8: "right", 9: "up", 10: "down"}


@dataclass(frozen=True)
class TraceOperation:
    kind: str
    tab: str | None
    peer_tab: str | None
    pane: str | None
    peer_pane: str | None
    argument_0: int
    argument_1: int


class IdAllocator:
    def __init__(self, capacity: int) -> None:
        self.generations = [0] * capacity
        self.live: set[str] = set()

    def allocate(self) -> str:
        for slot in range(len(self.generations)):
            if not any(identifier.startswith(f"{slot}:") for identifier in self.live):
                self.generations[slot] += 1
                identifier = f"{slot}:{self.generations[slot]}"
                self.live.add(identifier)
                return identifier
        raise AssertionError("trace model exhausted its bounded identity store")

    def release(self, identifier: str) -> None:
        if identifier not in self.live:
            raise AssertionError(f"trace released non-live identity {identifier}")
        self.live.remove(identifier)


def parse_id(value: str) -> str | None:
    return None if value == "-" else value


def replayable_trace(path: Path) -> bool:
    return "# real-replay: supported\n" in path.read_text(encoding="utf-8")


def read_trace(path: Path) -> list[TraceOperation]:
    operations: list[TraceOperation] = []
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line or line.startswith("#") or line == "frame-mux-trace-v1":
            continue
        fields = line.split()
        if fields[0] == "check":
            continue
        if len(fields) != 10 or fields[0] != "op":
            raise AssertionError(f"{path}:{line_number}: invalid operation record")
        if fields[8:] != ["applied", "applied"]:
            raise AssertionError(
                f"{path}:{line_number}: real replay does not support injected "
                "Runtime faults"
            )
        operations.append(
            TraceOperation(
                kind=fields[1],
                tab=parse_id(fields[2]),
                peer_tab=parse_id(fields[3]),
                pane=parse_id(fields[4]),
                peer_pane=parse_id(fields[5]),
                argument_0=int(fields[6]),
                argument_1=int(fields[7]),
            )
        )
    if not operations:
        raise AssertionError(f"{path}: real-replay trace is empty")
    return operations


class RealMuxTraceReplayTest(unittest.TestCase):
    def test_rejects_injected_runtime_faults_before_daemon_replay(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "faulted.trace"
            path.write_text(
                "frame-mux-trace-v1\nop idle - - - - 0 0 rejected applied\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(AssertionError, "injected Runtime faults"):
                read_trace(path)

    def test_replayable_simulation_traces_converge_on_the_real_daemon(self) -> None:
        traces = sorted(
            path for path in CORPUS.glob("*.trace") if replayable_trace(path)
        )
        self.assertTrue(traces, "mux corpus has no trace marked for real replay")
        for path in traces:
            with self.subTest(trace=path):
                self.replay(path)

    def replay(self, path: Path) -> None:
        server = FrameServer.from_environment()
        self.addCleanup(server.close)
        session = server.create_session("trace-replay")
        initial = session.state()

        pane_ids = IdAllocator(64)
        tab_ids = IdAllocator(16)
        initial_sim_tab = tab_ids.allocate()
        initial_sim_pane = pane_ids.allocate()
        pane_map = {initial_sim_pane: initial.focused_pane}
        tab_map = {initial_sim_tab: initial.active_tab}
        panes_by_tab = {initial_sim_tab: {initial_sim_pane}}
        tab_order = [initial_sim_tab]
        zoomed: dict[str, bool] = {initial_sim_tab: False}
        observed_processes = {initial.focused.pid}
        previous_revision = initial.revision

        def op(*arguments: str) -> dict[str, object]:
            result = server.command("proc", *arguments)
            try:
                proc = json.loads(result.output)
                document = proc["results"][0]["result"]
            except (KeyError, IndexError, TypeError, json.JSONDecodeError) as error:
                raise AssertionError(
                    f"trace op {arguments!r} returned invalid JSON: {result.output}"
                ) from error
            document["_exit_status"] = result.status
            return document

        def require_sim(value: str | None, label: str) -> str:
            if value is None:
                raise AssertionError(f"{path}: operation omitted required {label}")
            return value

        for index, operation in enumerate(read_trace(path)):
            kind = operation.kind
            if kind == "attachment-resize":
                session.require_client().resize(
                    operation.argument_0, operation.argument_1
                )
                server.wait_for_state(
                    session.name,
                    lambda state, columns=operation.argument_0, rows=operation.argument_1: (
                        state.columns == columns and state.rows == rows
                    ),
                    f"trace attachment resize {operation.argument_0}x{operation.argument_1}",
                )
            elif kind == "split":
                sim_tab = require_sim(operation.tab, "Tab")
                sim_pane = require_sim(operation.pane, "Pane")
                created = op(
                    "pane",
                    "split",
                    "--session",
                    session.name,
                    "--pane",
                    pane_map[sim_pane],
                    "--right" if operation.argument_0 == 0 else "--down",
                    "--focus",
                    "created",
                    "--",
                    "/bin/sh",
                )
                self.assertEqual(created.get("status"), "applied", created)
                new_sim_pane = pane_ids.allocate()
                pane_map[new_sim_pane] = str(created["pane"])
                panes_by_tab[sim_tab].add(new_sim_pane)
            elif kind == "create-tab":
                created = op(
                    "tab",
                    "new",
                    "--session",
                    session.name,
                    "--focus",
                    "created",
                    "--",
                    "/bin/sh",
                )
                self.assertEqual(created.get("status"), "applied", created)
                new_sim_tab = tab_ids.allocate()
                new_sim_pane = pane_ids.allocate()
                tab_map[new_sim_tab] = str(created["tab"])
                pane_map[new_sim_pane] = str(created["pane"])
                panes_by_tab[new_sim_tab] = {new_sim_pane}
                tab_order.append(new_sim_tab)
                zoomed[new_sim_tab] = False
            elif kind in {"focus", "stale-focus"}:
                sim_pane = require_sim(operation.pane, "Pane")
                real_pane = pane_map.get(sim_pane, sim_pane)
                focused = op(
                    "pane", "focus", "--session", session.name, "--pane", real_pane
                )
                expected = "stale" if kind == "stale-focus" else None
                if expected is None:
                    self.assertIn(
                        focused.get("status"), {"applied", "no_effect"}, focused
                    )
                else:
                    self.assertEqual(focused.get("status"), expected, focused)
            elif kind == "zoom":
                sim_tab = require_sim(operation.tab, "Tab")
                sim_pane = require_sim(operation.pane, "Pane")
                desired = not zoomed[sim_tab]
                result = op(
                    "pane",
                    "zoom",
                    "--session",
                    session.name,
                    "--pane",
                    pane_map[sim_pane],
                    "--on" if desired else "--off",
                )
                self.assertIn(result.get("status"), {"applied", "no_effect"}, result)
                zoomed[sim_tab] = desired
            elif kind == "resize":
                sim_pane = require_sim(operation.pane, "Pane")
                result = op(
                    "pane",
                    "resize",
                    "--session",
                    session.name,
                    "--pane",
                    pane_map[sim_pane],
                    RESIZE_DIRECTIONS[operation.argument_0],
                    str(operation.argument_1),
                )
                self.assertIn(
                    result.get("status"),
                    {"applied", "no_effect", "unavailable"},
                    result,
                )
            elif kind == "select-tab":
                sim_tab = require_sim(operation.tab, "Tab")
                selected = op(
                    "tab",
                    "select",
                    "--session",
                    session.name,
                    "--tab",
                    tab_map[sim_tab],
                )
                self.assertIn(
                    selected.get("status"), {"applied", "no_effect"}, selected
                )
            elif kind == "place-tab":
                sim_tab = require_sim(operation.tab, "Tab")
                tab_order.remove(sim_tab)
                if operation.peer_tab is None:
                    tab_order.append(sim_tab)
                else:
                    tab_order.insert(tab_order.index(operation.peer_tab), sim_tab)
                moved = op(
                    "tab",
                    "move",
                    "--session",
                    session.name,
                    "--tab",
                    tab_map[sim_tab],
                    str(tab_order.index(sim_tab) + 1),
                )
                self.assertIn(moved.get("status"), {"applied", "no_effect"}, moved)
            elif kind == "swap":
                sim_pane = require_sim(operation.pane, "Pane")
                peer = require_sim(operation.peer_pane, "peer Pane")
                swapped = op(
                    "pane",
                    "swap",
                    "--session",
                    session.name,
                    "--pane",
                    pane_map[sim_pane],
                    pane_map[peer],
                )
                self.assertIn(swapped.get("status"), {"applied", "no_effect"}, swapped)
            elif kind == "close-pane":
                sim_tab = require_sim(operation.tab, "Tab")
                sim_pane = require_sim(operation.pane, "Pane")
                real_pane = pane_map[sim_pane]
                process = session.state().pane(real_pane).pid
                closed = op(
                    "pane", "kill", "--session", session.name, "--pane", real_pane
                )
                self.assertEqual(closed.get("status"), "applied", closed)
                wait_for_process_exit(
                    process, diagnostics=lambda: server.diagnostics(session.name)
                )
                observed_processes.add(process)
                panes_by_tab[sim_tab].remove(sim_pane)
                pane_ids.release(sim_pane)
                del pane_map[sim_pane]
            elif kind == "close-tab":
                sim_tab = require_sim(operation.tab, "Tab")
                processes = [
                    session.state().pane(pane_map[pane]).pid
                    for pane in panes_by_tab[sim_tab]
                ]
                closed = op(
                    "tab", "kill", "--session", session.name, "--tab", tab_map[sim_tab]
                )
                self.assertEqual(closed.get("status"), "applied", closed)
                for process in processes:
                    wait_for_process_exit(
                        process, diagnostics=lambda: server.diagnostics(session.name)
                    )
                    observed_processes.add(process)
                for sim_pane in tuple(panes_by_tab[sim_tab]):
                    pane_ids.release(sim_pane)
                    del pane_map[sim_pane]
                tab_ids.release(sim_tab)
                tab_order.remove(sim_tab)
                del panes_by_tab[sim_tab]
                del tab_map[sim_tab]
                del zoomed[sim_tab]
            elif kind == "idle":
                pass
            else:
                raise AssertionError(
                    f"{path}:{index}: {kind} is not supported by real-replay traces"
                )

            state = session.state()
            self.assertGreaterEqual(state.revision, previous_revision)
            previous_revision = state.revision
            self.assertEqual(state.tabs, len(tab_map))
            self.assertEqual(state.panes, len(pane_map))
            self.assertEqual(
                {pane.id for pane in state.pane_states}, set(pane_map.values())
            )
            self.assertEqual(
                {pane.tab for pane in state.pane_states}, set(tab_map.values())
            )
            for pane in state.pane_states:
                observed_processes.add(pane.pid)
                self.assertTrue(
                    process_exists(pane.pid), f"trace Pane {pane.id} child exited"
                )

        session.detach()
        session.destroy()
        for process in observed_processes:
            wait_for_process_exit(process, diagnostics=server.diagnostics)
