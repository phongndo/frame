from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from typing import Any

from extensions.lemma_client import Client
from tests.support.mux_harness import (
    LemmaServer,
    process_exists,
    wait_for_process_exit,
    wait_until,
)


class PaneLifecycleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)

    def test_closing_one_split_kills_only_its_child(self) -> None:
        session = self.server.create_session("pane_close")
        left = session.pane()
        right = left.split_right()

        self.assertNotEqual(left.process, right.process)
        left.expect_alive()
        right.expect_alive()
        right.send("printf '__RIGHT_BEFORE_CLOSE__\\n'\r")
        right.expect_output("__RIGHT_BEFORE_CLOSE__")

        left.close()

        self.assertFalse(process_exists(left.process))
        right.expect_alive()
        right.send("printf '__RIGHT_AFTER_CLOSE__\\n'\r")
        right.expect_output("__RIGHT_AFTER_CLOSE__")

    def test_swap_changes_layout_position_not_child_ownership(self) -> None:
        session = self.server.create_session("pane_swap")
        left = session.pane()
        right = left.split_right()
        right.focus()
        state_before = session.state()
        focused_before = state_before.focused_pane

        def leaf_order() -> list[str]:
            inspected = self.server.require_command(
                "proc",
                "tab",
                "inspect",
                "--session",
                session.name,
                "--tab",
                state_before.active_tab,
            )
            layout = json.dumps(
                json.loads(inspected.output)["results"][0]["result"]["tab_state"][
                    "layout"
                ]
            )
            return sorted(
                (left.id, right.id), key=lambda pane: layout.index(f'"{pane}"')
            )

        self.assertEqual(leaf_order(), [left.id, right.id])
        # The keyboard binding resolves its peer through the Core directional-neighbor rule.
        session.require_client().prefix("H")
        wait_until(
            "directional swap to move the focused Pane left",
            lambda: True if leaf_order() == [right.id, left.id] else None,
            diagnostics=self.server.logs,
        )
        focused_after = self.server.wait_for_state(
            session.name,
            lambda state: state.focused_pane == focused_before,
            "focused Pane identity to survive pane swap",
        )
        self.assertEqual(focused_after.focused_pane, right.id)
        self.assertEqual(focused_after.focused.pid, right.process)

        left.send("printf '__LEFT_AFTER_SWAP__\\n'\r")
        left.expect_output("__LEFT_AFTER_SWAP__")
        right.send("printf '__RIGHT_AFTER_SWAP__\\n'\r")
        right.expect_output("__RIGHT_AFTER_SWAP__")
        left.expect_alive()
        right.expect_alive()


# Records its PTY size at start and on every SIGWINCH, so tests observe each committed resize.
SIZE_RECORDER = """
import fcntl, os, signal, struct, sys, termios
path = sys.argv[1]
def record(*_):
    rows, columns = struct.unpack('HHHH', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\\0' * 8))[:2]
    with open(path + '.tmp', 'w') as output:
        output.write(f'{rows} {columns}')
    os.replace(path + '.tmp', path)
signal.signal(signal.SIGWINCH, record)
record()
while True:
    signal.pause()
"""


class FloatingPaneTest(unittest.TestCase):
    """Floating Panes are Core and API state; Scene composition arrives separately."""

    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)
        self.control = Client(
            str(self.server.socket_path), name="float-control", capabilities=("proc",)
        )
        self.addCleanup(self.control.close)

    def run_command(self, command: str, **fields: Any) -> dict[str, Any]:
        return self.control.proc({"command": command, **fields})["results"][0]

    def result(self, command: str, **fields: Any) -> dict[str, Any]:
        entry = self.run_command(command, **fields)
        self.assertIn(entry["result"]["status"], ("applied", "no_effect"), entry)
        return entry["result"]

    def recorded_size(self, path: Path, size: tuple[int, int]) -> None:
        wait_until(
            f"float PTY size {size}",
            lambda: (
                True
                if path.exists() and tuple(map(int, path.read_text().split())) == size
                else None
            ),
            diagnostics=lambda: (
                path.read_text() if path.exists() else "no size recorded"
            ),
        )

    def panes(self, session: dict[str, str]) -> dict[str, dict[str, Any]]:
        listed = self.result("pane.list", session=session)["panes"]
        return {pane["id"]: pane for pane in listed}

    def test_float_lifecycle_through_the_public_api(self) -> None:
        session = self.server.create_session("floats", command=("sleep", "3600"))
        state = session.state()
        target = {"id": state.id}
        tab = {"id": state.active_tab}
        tiled = state.focused_pane
        size = self.server.root / "float.size"
        recorder = [sys.executable, "-c", SIZE_RECORDER, str(size)]

        created = self.result(
            "pane.float",
            session=target,
            tab=tab,
            placement={"kind": "centered", "columns": 40, "rows": 12},
            argv=recorder,
        )
        floating = created["pane"]
        self.assertEqual(created["tab"], state.active_tab)
        # The outer placement includes the native frame; the PTY is the inner rectangle.
        self.recorded_size(size, (10, 38))
        panes = self.panes(target)
        self.assertEqual(panes[tiled]["layer"], "tiled")
        self.assertNotIn("z", panes[tiled])
        record = panes[floating]
        self.assertEqual(
            (record["layer"], record["z"], record["suspended"], record["focused"]),
            ("float", 0, False, True),
        )
        self.assertEqual(
            record["placement"], {"kind": "centered", "columns": 40, "rows": 12}
        )
        self.assertEqual((record["columns"], record["rows"]), (38, 10))
        inspected = self.result("tab.inspect", session=target, tab=tab)["tab_state"]
        self.assertEqual(inspected["floats"], {"visible": True, "panes": [floating]})
        self.assertEqual(inspected["focused_pane"], floating)
        pane_state = self.result("pane.inspect", session=target, pane={"id": floating})
        self.assertEqual(pane_state["pane_state"]["pane"]["layer"], "float")

        for command, fields in (
            ("pane.split", {"direction": "right"}),
            ("pane.zoom", {"enabled": True}),
            ("pane.resize", {"direction": "left"}),
            ("pane.swap", {"other": {"id": tiled}}),
        ):
            rejected = self.run_command(
                command, session=target, pane={"id": floating}, **fields
            )["result"]
            self.assertEqual(
                (rejected["status"], rejected["error"]["reason"]),
                ("unavailable", "floating_pane"),
                command,
            )
        unfit = self.run_command(
            "pane.place",
            session=target,
            pane={"id": floating},
            placement={
                "kind": "absolute",
                "column": 70,
                "row": 0,
                "columns": 20,
                "rows": 6,
            },
        )["result"]
        self.assertEqual(unfit["error"]["reason"], "float_suspended")
        tiled_place = self.run_command(
            "pane.place",
            session=target,
            pane={"id": tiled},
            placement={"kind": "centered", "columns": 20, "rows": 6},
        )["result"]
        self.assertEqual(tiled_place["error"]["reason"], "tiled_pane")

        # Placement and viewport changes resize the float's PTY.
        self.result(
            "pane.place",
            session=target,
            pane={"id": floating},
            placement={"kind": "relative", "width_percent": 50, "height_percent": 50},
        )
        self.recorded_size(size, (10, 38))
        session.require_client().resize(120, 41)
        self.recorded_size(size, (18, 58))

        # Hiding floats returns focus to the tiled layer; focusing a hidden float is refused.
        self.result("tab.floats", session=target, tab=tab, visible=False)
        inspected = self.result("tab.inspect", session=target, tab=tab)["tab_state"]
        self.assertEqual(inspected["floats"]["visible"], False)
        self.assertEqual(inspected["focused_pane"], tiled)
        hidden_focus = self.run_command(
            "pane.focus", session=target, pane={"id": floating}
        )["result"]
        self.assertEqual(hidden_focus["error"]["reason"], "floats_hidden")
        self.result("tab.floats", session=target, tab=tab, visible=True)

        # A float's own exit removes only that float.
        exiting = self.result(
            "pane.float",
            session=target,
            tab=tab,
            placement={"kind": "centered", "columns": 10, "rows": 5},
            argv=["sh", "-c", "exit 0"],
            focus="preserve",
        )["pane"]
        wait_until(
            "exited float to close",
            lambda: True if exiting not in self.panes(target) else None,
        )
        self.assertEqual(set(self.panes(target)), {tiled, floating})
        self.result("pane.kill", session=target, pane={"id": floating})
        self.assertEqual(set(self.panes(target)), {tiled})

    def test_api_creation_ends_copy_mode_only_when_it_moves_focus(self) -> None:
        for command in ("pane.float", "pane.split", "tab.new"):
            with self.subTest(command=command):
                session = self.server.create_session(
                    "copy_focus_" + command.replace(".", "_"), command=("cat",)
                )
                state = session.state()
                target = {"id": state.id}
                fields: dict[str, Any] = {"session": target, "argv": ["cat"]}
                if command == "pane.float":
                    fields.update(
                        tab={"id": state.active_tab},
                        placement={"kind": "centered", "columns": 40, "rows": 12},
                    )
                elif command == "pane.split":
                    fields.update(pane={"id": state.focused_pane}, direction="right")
                client = session.require_client()
                client.send("COPY_SOURCE\r")
                client.expect_output("COPY_SOURCE")
                client.prefix("[")

                def copying() -> bool:
                    client.drain()
                    return client.screen_text().splitlines()[0].startswith("COPY")

                wait_until(
                    "copy mode before API creation", lambda: True if copying() else None
                )
                self.result(command, **fields, focus="preserve")
                self.assertEqual(session.state().focused_pane, state.focused_pane)
                self.assertTrue(copying())

                created = self.result(command, **fields)["pane"]
                wait_until(
                    "created-pane focus to leave copy mode",
                    lambda: True if not copying() else None,
                    diagnostics=client.diagnostics,
                )
                self.assertEqual(session.state().focused_pane, created)
                client.send("CREATED_PANE_INPUT\r")
                wait_until(
                    "input to the created Pane",
                    lambda: (
                        True
                        if "CREATED_PANE_INPUT"
                        in self.result(
                            "pane.capture", session=target, pane={"id": created}
                        )["capture"]["text"]
                        else None
                    ),
                )

    def test_suspending_a_float_leaves_copy_mode_and_routes_input_to_the_tile(
        self,
    ) -> None:
        session = self.server.create_session("float_copy", command=("cat",))
        state = session.state()
        target = {"id": state.id}
        floating = self.result(
            "pane.float",
            session=target,
            tab={"id": state.active_tab},
            placement={"kind": "centered", "columns": 40, "rows": 12},
            argv=["cat"],
        )["pane"]
        client = session.require_client()

        def copying() -> bool:
            client.drain()
            return client.screen_text().splitlines()[0].startswith("COPY")

        client.prefix("[")
        wait_until("float copy mode", lambda: True if copying() else None)
        client.resize(30, 10)
        wait_until(
            "float suspension",
            lambda: True if self.panes(target)[floating]["suspended"] else None,
        )
        self.assertTrue(self.panes(target)[state.focused_pane]["focused"])
        wait_until(
            "suspension to leave float copy mode",
            lambda: True if not copying() else None,
            diagnostics=client.diagnostics,
        )
        client.send("TILE_AFTER_SUSPEND\r")
        client.expect_output("TILE_AFTER_SUSPEND")

        client.resize(80, 24)
        wait_until(
            "float to fit again",
            lambda: True if not self.panes(target)[floating]["suspended"] else None,
        )
        self.assertFalse(self.panes(target)[floating]["focused"])
        self.assertFalse(copying())

    def test_float_survives_its_creating_extension_disconnect(self) -> None:
        session = self.server.create_session("float_owner", command=("sleep", "3600"))
        state = session.state()
        target = {"id": state.id}
        with Client(
            str(self.server.socket_path),
            name="float-creator",
            capabilities=("proc", "surface"),
            session=state.id,
        ) as creator:
            dock = creator.proc(
                {
                    "command": "surface.create",
                    "placement": {"kind": "dock.bottom", "size": 2},
                }
            )
            self.assertTrue(dock["ok"], dock)
            wait_until(
                "creator dock to reserve rows",
                lambda: (
                    True
                    if self.panes(target)[state.focused_pane]["rows"] == 21
                    else None
                ),
            )
            created = creator.proc(
                {
                    "command": "pane.float",
                    "session": target,
                    "tab": {"id": state.active_tab},
                    "placement": {"kind": "centered", "columns": 20, "rows": 8},
                    "argv": ["sleep", "3600"],
                }
            )["results"][0]["result"]
            self.assertEqual(created["status"], "applied")
            floating = created["pane"]
            process = int(self.panes(target)[floating]["process"]["pid"])

        # Dock cleanup proves the daemon processed the creator's disconnect.
        wait_until(
            "creator disconnect to release its dock",
            lambda: (
                True if self.panes(target)[state.focused_pane]["rows"] == 23 else None
            ),
        )
        # Another connection can still address the same PTY after owner cleanup.
        inspected = self.result("pane.inspect", session=target, pane={"id": floating})
        self.assertEqual(inspected["pane_state"]["pane"]["id"], floating)
        self.assertTrue(process_exists(process))
        self.result("pane.kill", session=target, pane={"id": floating})
        wait_for_process_exit(process)
        self.assertEqual(set(self.panes(target)), {state.focused_pane})

    def test_closing_the_last_tiled_pane_closes_the_tab_and_its_floats(self) -> None:
        session = self.server.create_session("float_cascade", command=("sleep", "3600"))
        state = session.state()
        target = {"id": state.id}
        second = self.result(
            "tab.new", session=target, argv=["sleep", "3600"], focus="preserve"
        )
        floating = self.result(
            "pane.float",
            session=target,
            tab={"id": second["tab"]},
            placement={"kind": "relative", "width_percent": 80, "height_percent": 80},
            argv=["sleep", "3600"],
        )["pane"]
        process = int(self.panes(target)[floating]["process"]["pid"])
        self.assertTrue(process_exists(process))

        self.result("pane.kill", session=target, pane={"id": second["pane"]})
        wait_for_process_exit(process)
        remaining = self.panes(target)
        self.assertEqual(set(remaining), {state.focused_pane})
        self.assertEqual(len(self.result("tab.list", session=target)["tabs"]), 1)


if __name__ == "__main__":
    unittest.main()
