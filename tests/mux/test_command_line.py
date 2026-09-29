from __future__ import annotations

import time
import unittest

from tests.support.mux_harness import FrameServer, wait_until


class CommandLineTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = FrameServer.from_environment()
        self.addCleanup(self.server.close)

    def test_prompt_opens_clear_and_history_remains_available(self) -> None:
        session = self.server.create_session("command_source")
        client = session.require_client()

        client.prefix(":")
        client.expect_output(": ")
        client.send("p")
        client.expect_output(":p")
        client.send(b"\x15" + b"pane split --right\r")
        self.server.wait_for_state(
            session.name,
            lambda state: state.panes == 2,
            "command line split to create a pane",
        )

        isolated = self.server.create_session("command_isolated")
        isolated_client = isolated.require_client()
        isolated_client.prefix(":")
        isolated_client.send(b"\x1b[A")
        isolated_client.drain(0.01)
        self.assertNotIn("pane split --right", isolated_client.screen_text())
        isolated_client.send(b"\x03")

        # Reopening starts clear; Up recalls the most recently submitted command.
        client.prefix(":")
        client.expect_output(": ")
        self.assertNotIn("pane split --right", client.screen_text())
        client.send(b"\x1b[A")
        client.expect_output("pane split --right")
        client.send(b"\x15" + b"unknown-command\r")
        client.expect_output("Error: Unknown command")
        self.assertFalse(client.screen_text().splitlines()[0].startswith(":"))
        self.assertNotIn("frame.command-result", client.screen_text())

        # Entering command mode replaces the message with a clean prompt.
        client.prefix(":")
        client.expect_output(": ")
        client.send(b"tab new --title tests\r")
        self.server.wait_for_state(
            session.name,
            lambda state: state.tabs == 2,
            "command line tab creation",
        )

        client.prefix(":")
        client.expect_output(": ")
        self.assertNotIn("tab new --title tests", client.screen_text())
        client.send(b"\x1b[A")
        client.expect_output("tab new --title tests")
        client.send(b"\x1b[A")
        client.expect_output("unknown-command")
        client.send(b"\x1b[B")
        client.expect_output("tab new --title tests")
        client.send(b"\x03")

    def test_errors_expire_dismiss_on_input_and_remain_in_the_message_log(self) -> None:
        session = self.server.create_session("command_messages")
        client = session.require_client()

        def normal_status() -> bool | None:
            client.drain(0.01)
            first_line = client.screen_text().splitlines()[0]
            return True if "command_messages  |" in first_line else None

        client.prefix(":")
        client.send("unknown-one\r")
        client.expect_output("Error: Unknown command")
        wait_until("command error to expire", normal_status, timeout=3.0)

        client.prefix(":")
        client.send("unknown-two\r")
        client.expect_output("Error: Unknown command")
        client.send("a")
        wait_until("keyboard input to dismiss command error", normal_status)

        client.prefix(":")
        client.send("unknown-three\r")
        client.expect_output("Error: Unknown command")
        client.send(b"\x1b[<0;1;2M\x1b[<0;1;2m")
        wait_until("mouse input to dismiss command error", normal_status)

        client.prefix("~")
        client.expect_output("LOG")
        client.expect_output("Error: Unknown command")
        client.send("q")
        wait_until("message viewer to restore the pane", normal_status)

    def test_repeated_error_restarts_the_display_timeout(self) -> None:
        session = self.server.create_session("command_timeout")
        client = session.require_client()

        client.prefix(":")
        client.send("unknown-one\r")
        client.expect_output("Error: Unknown command")
        time.sleep(1.0)
        client.prefix(":")
        client.send("unknown-two\r")
        client.expect_output("Error: Unknown command")
        time.sleep(0.8)
        client.drain(0.01)
        self.assertIn("Error: Unknown command", client.screen_text().splitlines()[0])

        wait_until(
            "restarted command error timeout",
            lambda: (
                client.drain(0.01) >= 0
                and (
                    True
                    if "command_timeout  |" in client.screen_text().splitlines()[0]
                    else None
                )
            ),
            timeout=2.0,
        )

    def test_configured_history_file_seeds_a_new_attachment_after_restart(self) -> None:
        persistent = FrameServer.from_environment(
            config_text=(
                'local frame = require("frame")\n'
                'frame.setup({ history = { file = os.getenv("HOME") .. "/commands" } })'
            )
        )
        self.addCleanup(persistent.close)
        session = persistent.create_session("history_before")
        client = session.require_client()
        client.prefix(":")
        client.send("tab new --title persisted\r")
        persistent.wait_for_state(
            session.name,
            lambda state: state.tabs == 2,
            "command history source to execute",
        )

        persistent.restart()
        restored = persistent.create_session("history_after")
        restored_client = restored.require_client()
        restored_client.prefix(":")
        restored_client.send(b"\x1b[A")
        restored_client.expect_output("tab new --title persisted")
        restored_client.send(b"\x03")

    def test_session_tab_completion_moves_the_live_attachment(self) -> None:
        source = self.server.create_session("switch_source")
        target = self.server.create_session("switch_target", attach=False)
        client = source.require_client()

        client.prefix(":")
        client.send("switch switch_ta\t\r")
        self.server.wait_for_state(
            source.name,
            lambda state: not state.attached,
            "source attachment to move away",
        )
        self.server.wait_for_state(
            target.name,
            lambda state: state.attached,
            "target session to receive the same attachment",
        )

        self.assertTrue(client.running)
        client.prefix(":")
        client.expect_output(": ")
        self.assertNotIn("switch switch_target", client.screen_text())
        client.send(b"\x1b[A")
        client.expect_output("switch switch_target")
        client.send(b"\x03")
        client.expect_output("switch_target  |")
        client.send("printf '__SWITCHED_ATTACHMENT__\\n'\r")
        client.expect_output("__SWITCHED_ATTACHMENT__")


if __name__ == "__main__":
    unittest.main()
