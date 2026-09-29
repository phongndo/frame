from __future__ import annotations

import shlex
import unittest

from tests.support.mux_harness import FrameServer, process_exists, wait_for_process_exit


class SessionLifecycleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = FrameServer.from_environment()
        self.addCleanup(self.server.close)

    def test_output_while_detached_is_current_on_reattach(self) -> None:
        session = self.server.create_session("detach_output")
        pane = session.pane()
        gate = self.server.root / "detached-output.gate"
        command = (
            "printf '__DETACH_READY__\\n'; "
            f"while [ ! -e {shlex.quote(str(gate))} ]; do sleep 0.01; done; "
            "printf '\\033[1;32m__OUTPUT_WHILE_DETACHED__\\033[0m\\n'\r"
        )
        pane.send(command)
        pane.expect_output("__DETACH_READY__")

        session.detach()
        self.assertTrue(process_exists(pane.process))
        detached = session.state()
        self.assertFalse(detached.attached)
        self.assertEqual(detached.focused_pane, pane.id)
        self.assertEqual(detached.focused.pid, pane.process)

        gate.touch()
        client = session.attach()
        client.expect_output("__OUTPUT_WHILE_DETACHED__")
        attached = session.state()
        self.assertEqual(attached.focused_pane, pane.id)
        self.assertEqual(attached.focused.pid, pane.process)
        pane.expect_alive()

    def test_destroyed_detached_session_reclaims_its_child(self) -> None:
        session = self.server.create_session("session_destroy")
        pane = session.pane()
        session.detach()
        pane.expect_alive()

        session.destroy()

        wait_for_process_exit(pane.process, diagnostics=self.server.diagnostics)
        self.assertIsNone(self.server.session_state(session.name))


if __name__ == "__main__":
    unittest.main()
