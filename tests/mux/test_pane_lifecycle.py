from __future__ import annotations

import json
import unittest

from tests.support.mux_harness import LemmaServer, process_exists, wait_until


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


if __name__ == "__main__":
    unittest.main()
