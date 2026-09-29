from __future__ import annotations

import json
import os
import random
import unittest

from tests.support.mux_harness import (
    FrameServer,
    Pane,
    process_exists,
    wait_for_process_exit,
    wait_until,
)


class MuxCommandModelStressTest(unittest.TestCase):
    def test_deterministic_commands_preserve_semantic_identity_and_process_ownership(
        self,
    ) -> None:
        seed = int(os.environ.get("FRAME_STRESS_SEED", "1369964835"), 0)
        operation_count = int(os.environ.get("FRAME_STRESS_OPERATIONS", "64"), 0)
        randomizer = random.Random(seed)
        operations: list[str] = []
        server = FrameServer.from_environment()
        self.addCleanup(server.close)
        session = server.create_session("state_machine")
        first = session.pane()
        panes: dict[str, Pane] = {first.id: first}
        all_processes = {first.process}
        stale_panes: list[str] = []

        def op(*arguments: str) -> dict[str, object]:
            result = server.command("proc", *arguments)
            try:
                proc = json.loads(result.output)
                document = proc["results"][0]["result"]
            except (KeyError, IndexError, TypeError, json.JSONDecodeError) as error:
                raise AssertionError(
                    f"op {arguments!r} returned invalid JSON: {result.output}"
                ) from error
            document["_exit_status"] = result.status
            return document

        try:
            for index in range(operation_count):
                choices = ["focus", "agent-focus", "zoom", "input", "resize", "detach"]
                if len(panes) < 8:
                    choices.extend(("split-right", "split-down"))
                if len(panes) > 1:
                    choices.extend(("close", "swap"))
                if stale_panes:
                    choices.append("stale-focus")
                operation = randomizer.choice(choices)

                if operation.startswith("split"):
                    source = randomizer.choice(list(panes.values()))
                    created = (
                        source.split_right()
                        if operation == "split-right"
                        else source.split_down()
                    )
                    panes[created.id] = created
                    all_processes.add(created.process)
                    operations.append(
                        f"{index}: {operation} {source.process}->{created.process}"
                    )
                elif operation == "close":
                    closed = randomizer.choice(list(panes.values()))
                    closed.close()
                    panes.pop(closed.id)
                    stale_panes.append(closed.id)
                    operations.append(
                        f"{index}: close {closed.id} pid={closed.process}"
                    )
                elif operation == "focus":
                    target = randomizer.choice(list(panes.values()))
                    target.focus()
                    operations.append(f"{index}: focus {target.id}")
                elif operation == "agent-focus":
                    target = randomizer.choice(list(panes.values()))
                    focused = op(
                        "pane",
                        "focus",
                        "--session",
                        session.name,
                        "--pane",
                        target.id,
                    )
                    if focused.get("status") not in {"applied", "no_effect"}:
                        raise AssertionError(f"agent focus failed: {focused}")
                    server.wait_for_state(
                        session.name,
                        lambda state, pane_id=target.id: state.focused_pane == pane_id,
                        f"agent focus Pane {target.id}",
                    )
                    operations.append(f"{index}: agent-focus {target.id}")
                elif operation == "zoom":
                    target = randomizer.choice(list(panes.values()))
                    before = session.state()
                    enabled = randomizer.choice((True, False))
                    zoomed = op(
                        "pane",
                        "zoom",
                        "--session",
                        session.name,
                        "--pane",
                        target.id,
                        "--on" if enabled else "--off",
                    )
                    if zoomed.get("status") not in {"applied", "no_effect"}:
                        raise AssertionError(f"agent zoom failed: {zoomed}")
                    after = session.state()
                    if (
                        zoomed["status"] == "applied"
                        and after.revision == before.revision
                    ):
                        raise AssertionError(
                            "applied zoom did not advance Session revision"
                        )
                    operations.append(f"{index}: zoom {target.id} enabled={enabled}")
                elif operation == "stale-focus":
                    stale = randomizer.choice(stale_panes)
                    before = session.state()
                    rejected = op(
                        "pane",
                        "focus",
                        "--session",
                        session.name,
                        "--pane",
                        stale,
                    )
                    if rejected.get("status") != "stale":
                        raise AssertionError(
                            f"stale Pane focus was not rejected: {rejected}"
                        )
                    after = session.state()
                    if after.revision != before.revision:
                        raise AssertionError(
                            "rejected stale focus mutated Session revision"
                        )
                    operations.append(f"{index}: stale-focus {stale}")
                elif operation == "swap":
                    target = randomizer.choice(list(panes.values()))
                    target.focus()
                    direction = randomizer.choice("HJKL")
                    session.require_client().prefix(direction)
                    operations.append(f"{index}: swap {target.process} {direction}")
                elif operation == "resize":
                    columns, rows = randomizer.choice(((80, 24), (100, 30), (120, 40)))
                    session.require_client().resize(columns, rows)
                    server.wait_for_state(
                        session.name,
                        lambda state, c=columns, r=rows: (
                            state.columns == c and state.rows == r
                        ),
                        f"state-machine resize {columns}x{rows}",
                    )
                    operations.append(f"{index}: resize {columns}x{rows}")
                elif operation == "detach":
                    session.detach()
                    for pane in panes.values():
                        if not process_exists(pane.process):
                            raise AssertionError(
                                f"pane {pane.id} process {pane.process} died while detached"
                            )
                    session.attach()
                    operations.append(f"{index}: detach/reattach")
                else:
                    operations.append(f"{index}: input")

                state = session.state()
                if state.panes != len(panes):
                    raise AssertionError(
                        f"listing has {state.panes} panes, model has {len(panes)}"
                    )
                if state.focused_pane not in panes:
                    raise AssertionError(
                        f"focused pane {state.focused_pane} is not modeled live"
                    )
                observed = {pane.id: pane.pid for pane in state.pane_states}
                expected = {pane.id: pane.process for pane in panes.values()}
                if observed != expected:
                    raise AssertionError(
                        f"structured Pane/PID projection {observed} differs from model {expected}"
                    )
                for pane in panes.values():
                    if not process_exists(pane.process):
                        raise AssertionError(
                            f"modeled live pane {pane.id} process {pane.process} exited"
                        )
                # Tiny panes can scroll a transient marker out before the daemon presents a frame.
                # Use a durable shell-side acknowledgement while keeping the outer client flowing.
                acknowledgement = server.root / f"a{index}"
                client = session.require_client()
                client.send(f': >"$TMPDIR/a{index}"\r')

                def acknowledged() -> bool | None:
                    client.drain(0.002)
                    return True if acknowledgement.exists() else None

                wait_until(
                    f"state-machine input acknowledgement {index}",
                    acknowledged,
                    diagnostics=lambda: server.diagnostics(session.name),
                )

            session.detach()
            session.destroy()
            for process in all_processes:
                wait_for_process_exit(process, diagnostics=server.diagnostics)
        except BaseException as error:
            detail = (
                f"seed={seed} operation_count={operation_count}\n"
                f"operations:\n"
                + "\n".join(operations)
                + "\n\n"
                + server.diagnostics(session.name)
            )
            raise AssertionError(detail) from error


if __name__ == "__main__":
    unittest.main()
