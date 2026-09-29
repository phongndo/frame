from __future__ import annotations

import json
import os
import signal
import socket
import sys
import unittest
from pathlib import Path

from tests.support.mux_harness import FrameServer, wait_until


class ConfigurationMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = FrameServer.from_environment(
            config_text="""
local frame = require("frame")
frame.setup({
  input = { preset = "none", prefix = false },
  terminal = { scrollback_lines = 1234 },
  ui = { status_line = false },
  launch = {
    default_cwd = "/tmp",
    default_program = {
      "/bin/sh", "-c", [=[printf 'CONFIGURED_PROGRAM:%s\\n' "$PWD"; exec /bin/sh]=]
    },
  },
})
frame.context.set("copy", { label = " COPY ", unbound = "consume" })
frame.keymap.set("normal", "M-c", "enter_copy_mode")
frame.keymap.set("copy", "x", "copy_leave")
frame.keymap.set("normal", "M-s", "split_left_right")
frame.keymap.set("normal", "M-f", "enter_copy_search_forward")
frame.keymap.del("normal", "C-b")
"""
        )
        self.addCleanup(self.server.close)

    def test_compiled_lua_keymap_is_active_for_new_sessions(self) -> None:
        session = self.server.create_session("configured_input")
        client = session.require_client()
        self.assertEqual(session.state().panes, 1)
        client.expect_output("CONFIGURED_PROGRAM:/tmp")
        self.assertNotIn("configured_input", client.screen_text())

        # This copy mode and its leave key both come from the blank user policy.
        client.send(b"\x1bcx")
        client.send(b"\x1bs")

        state = self.server.wait_for_state(
            session.name,
            lambda current: current.panes == 2,
            "configured split key to create a second pane",
        )
        self.assertEqual(state.panes, 2)

    def test_copy_search_does_not_capture_input_without_a_status_line(self) -> None:
        session = self.server.create_session("configured_hidden_search")
        client = session.require_client()
        pane = session.pane()
        client.expect_output("CONFIGURED_PROGRAM:/tmp")

        client.send(b"\x1bfprintf '__VISIBLE_AFTER_SEARCH__\\n'\r")

        pane.expect_output("__VISIBLE_AFTER_SEARCH__")
        pane.expect_alive()


class DocumentedConfigurationMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.examples = Path(__file__).resolve().parents[2] / "examples"
        self.server = FrameServer.from_environment(
            config_text=(self.examples / "configuration.lua").read_text()
            + "\n"
            + (self.examples / "command.lua").read_text()
        )
        self.addCleanup(self.server.close)

    def test_all_lua_examples_pass_native_configuration_validation(self) -> None:
        examples = sorted(self.examples.glob("*.lua"))
        self.assertTrue(examples)
        for example in examples:
            with self.subTest(example=example.name):
                result = self.server.command("config", "check", str(example))
                self.assertEqual(result.status, 0, result.output)

    def test_documented_keymap_splits_a_real_pane(self) -> None:
        session = self.server.create_session("example-config")
        session.require_client().send(b"\x1bd")
        self.server.wait_for_state(
            session.name,
            lambda state: state.panes == 2,
            "documented Meta-d binding to split a pane",
        )

    def test_documented_command_opens_a_named_shell_tab(self) -> None:
        session = self.server.create_session("example-command")
        client = session.require_client()
        client.prefix(":")
        client.send("work.sh\t")
        client.expect_output("work.shell")
        client.send("'example shell'\r")
        self.server.wait_for_state(
            session.name,
            lambda state: state.tabs == 2,
            "documented command to open a shell tab",
        )
        result = self.server.require_command(
            "proc", "tab", "list", "--session", session.name
        )
        tabs = json.loads(result.output)["results"][0]["result"]["tabs"]
        self.assertIn("example shell", [tab["title"] for tab in tabs])


class ConfigurationReloadMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.initial = (
            'local frame = require("frame")\n'
            'frame.keymap.set("normal", "M-d", "split_left_right")\n'
        )
        self.server = FrameServer.from_environment(config_text=self.initial)
        self.addCleanup(self.server.close)
        self.path = Path(self.server.environment["XDG_CONFIG_HOME"]) / "frame/init.lua"
        self.session = self.server.create_session("reload", command=("cat",))
        self.client = self.session.require_client()

    def reload(self) -> dict:
        result = self.server.command("config", "reload")
        document = json.loads(result.output)
        self.assertEqual(result.status == 0, document["ok"], document)
        return document

    def test_success_replaces_policy_and_preserves_process_identity(self) -> None:
        pane = self.session.pane()
        before = self.session.state()
        self.path.write_text(
            'local frame = require("frame")\n'
            'frame.keymap.set("normal", "M-n", "split_left_right")\n'
        )
        self.assertTrue(self.reload()["ok"])
        self.assertEqual(self.session.state().panes, before.panes)
        pane.expect_alive()
        self.client.send(b"\x1bn")
        self.server.wait_for_state(
            self.session.name, lambda s: s.panes == 2, "new split binding"
        )

    def test_invalid_and_startup_only_changes_leave_old_policy_intact(self) -> None:
        for text, reason in [
            ("error('bad reload')", "invalid_configuration"),
            ('require("frame").setup({ui={status_line=false}})', "restart_required"),
        ]:
            with self.subTest(reason=reason):
                self.path.write_text(text)
                result = self.reload()
                self.assertFalse(result["ok"], result)
                self.assertEqual(
                    result["results"][0]["result"]["error"]["reason"], reason
                )
        self.client.send(b"\x1bd")
        self.server.wait_for_state(
            self.session.name, lambda s: s.panes == 2, "old split binding"
        )

    @unittest.skipIf(os.geteuid() == 0, "root bypasses file read permissions")
    def test_unreadable_configuration_preserves_active_policy(self) -> None:
        self.path.chmod(0)
        self.addCleanup(self.path.chmod, 0o600)
        self.assertNotEqual(self.server.command("config", "check").status, 0)
        result = self.reload()
        self.assertFalse(result["ok"], result)
        self.assertEqual(
            result["results"][0]["result"]["error"]["reason"], "invalid_configuration"
        )
        self.client.send(b"\x1bd")
        self.server.wait_for_state(
            self.session.name, lambda s: s.panes == 2, "policy after unreadable reload"
        )

    def test_existing_nonregular_or_broken_path_preserves_active_policy(self) -> None:
        self.path.unlink()
        self.path.mkdir()
        self.assertNotEqual(self.server.command("config", "check").status, 0)
        self.assertFalse(self.reload()["ok"])
        self.path.rmdir()
        self.path.symlink_to("missing.lua")
        self.assertFalse(self.reload()["ok"])
        self.client.send(b"\x1bd")
        self.server.wait_for_state(
            self.session.name,
            lambda s: s.panes == 2,
            "policy after invalid path reload",
        )

    def test_missing_discovered_configuration_publishes_builtins(self) -> None:
        self.path.unlink()
        result = self.reload()
        self.assertTrue(result["ok"], result)
        self.client.send(b"\x1bdMISSING-CONFIG\r")
        self.client.expect_output("MISSING-CONFIG")
        self.assertEqual(self.session.state().panes, 1)
        self.client.prefix("%")
        self.server.wait_for_state(
            self.session.name,
            lambda s: s.panes == 2,
            "builtin split after file removal",
        )

    def test_reload_is_native_and_replaces_commands(self) -> None:
        self.path.write_text(
            'local l=require("frame")\n'
            'l.command.register("test.new", {description="new", handler=function(ctx) '
            'assert(ctx:proc({commands={{command="tab.rename", session={id=ctx.session}, '
            'tab={id=ctx.tab}, title="RELOADED"}}}).ok) end})\n'
        )
        self.client.prefix(":")
        self.client.send("reload\r")
        self.client.expect_output("Configuration reloaded")
        self.client.prefix(":")
        self.client.send("test.new\r")
        self.client.expect_output("RELOADED")
        self.assertEqual(self.session.state().panes, 1)

    def test_slow_configuration_does_not_stop_terminal_progress(self) -> None:
        gate = self.server.root / "release-reload"
        marker = self.server.root / "loading"
        self.path.write_text(
            self.initial
            + f'local f=assert(io.open({json.dumps(str(marker))}, "w")); f:close()\n'
            f'while not io.open({json.dumps(str(gate))}, "r") do os.execute("sleep 0.01") end\n'
        )
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(peer.close)
        peer.settimeout(4)
        peer.connect(str(self.server.socket_path))
        peer.sendall(
            b'{"schema":"frame.proc/v1","commands":[{"command":"config.reload"}]}\n'
        )
        wait_until("configuration host started", lambda: marker.exists() or None)
        self.client.send("ALIVE-DURING-RELOAD\r")
        self.client.expect_output("ALIVE-DURING-RELOAD", timeout=1)
        gate.touch()
        with peer.makefile("rb") as stream:
            result = json.loads(stream.readline())
        self.assertTrue(result["ok"], result)

    def test_disconnected_control_owner_cannot_publish_its_candidate(self) -> None:
        gate = self.server.root / "cancel-gate"
        marker = self.server.root / "cancel-loading"
        self.path.write_text(
            'local frame=require("frame")\n'
            'frame.keymap.set("normal", "M-n", "split_left_right")\n'
            f'local f=assert(io.open({json.dumps(str(marker))}, "w")); f:close()\n'
            f'while not io.open({json.dumps(str(gate))}, "r") do os.execute("sleep 0.01") end\n'
        )
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
            peer.connect(str(self.server.socket_path))
            peer.sendall(
                b'{"schema":"frame.proc/v1","commands":[{"command":"config.reload"}]}\n'
            )
            wait_until(
                "cancellable configuration host", lambda: marker.exists() or None
            )
        # A subsequent observable daemon round trip lets owner cancellation run before release.
        self.server.require_command("proc", "session", "list")
        gate.touch()
        self.client.send(b"\x1bd")
        self.server.wait_for_state(
            self.session.name, lambda s: s.panes == 2, "unchanged old policy"
        )

    @unittest.skipUnless(
        sys.platform == "linux", "uses Linux host wait-state introspection"
    )
    def test_disconnected_reload_owner_is_revoked_with_unrelated_pending_procs(
        self,
    ) -> None:
        # Occupy scheduler slots so Proc lifetime can outlast connection lifetime.
        for _ in range(8):
            pending = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.addCleanup(pending.close)
            pending.connect(str(self.server.socket_path))
            request = {
                "schema": "frame.proc/v1",
                "commands": [
                    {
                        "command": "pane.wait",
                        "session": {"name": self.session.name},
                        "pane": {"id": "0:1"},
                        "contains": "__NEVER__",
                        "timeout_ms": 10000,
                    }
                ],
            }
            pending.sendall(json.dumps(request).encode() + b"\n")
        gate = self.server.root / "queued-cancel-gate"
        marker = self.server.root / "queued-cancel-loading"
        self.path.write_text(
            'require("frame").keymap.set("normal", "M-n", "split_left_right")\n'
            'local s=assert(io.open("/proc/self/stat")); local pid=s:read("*n"); s:close()\n'
            f'local f=assert(io.open({json.dumps(str(marker))}, "w")); f:write(pid); f:close()\n'
            f'while not io.open({json.dumps(str(gate))}, "r") do os.execute("sleep 0.01") end\n'
        )
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(peer.close)
        peer.connect(str(self.server.socket_path))
        peer.sendall(
            b'{"schema":"frame.proc/v1","commands":[{"command":"config.reload"}]}\n'
        )
        wait_until(
            "queued cancellable host",
            lambda: (marker.exists() and marker.read_text()) or None,
        )
        host = int(marker.read_text())
        daemon = self.server.process.pid
        os.kill(daemon, signal.SIGSTOP)
        try:
            wait_until(
                "daemon stopped",
                lambda: (
                    (os.waitpid(daemon, os.WUNTRACED | os.WNOHANG)[0] == daemon) or None
                ),
            )
            peer.close()
            gate.touch()
            # The host has sent its registration once it waits for invocation input.
            channel = Path(f"/proc/{host}/wchan")
            wait_until(
                "host registration ready",
                lambda: ("poll" in channel.read_text()) or None,
                diagnostics=channel.read_text,
            )
        finally:
            os.kill(daemon, signal.SIGCONT)
        self.client.send("AFTER-CANCEL\r")
        self.client.expect_output("AFTER-CANCEL")
        self.client.send(b"\x1bd")
        self.server.wait_for_state(
            self.session.name, lambda s: s.panes == 2, "old policy after cancellation"
        )

    @unittest.skipUnless(sys.platform == "linux", "uses Linux descriptor introspection")
    def test_candidate_inherits_no_daemon_or_pane_descriptors(self) -> None:
        status_file = self.server.root / "host-status"
        self.path.write_text(
            self.initial
            + 'local f=assert(io.open("/proc/self/status")); local s=f:read("*a"); f:close()\n'
            f'local out=assert(io.open({json.dumps(str(status_file))}, "w")); out:write(s); out:close()\n'
        )
        self.assertTrue(self.reload()["ok"])
        pid = next(
            line.split()[1]
            for line in status_file.read_text().splitlines()
            if line.startswith("Pid:")
        )
        self.assertEqual(
            {p.name for p in Path(f"/proc/{pid}/fd").iterdir()}, {"0", "1", "2", "3"}
        )

    def test_reload_recovers_a_crashed_command_host(self) -> None:
        self.path.write_text(
            self.initial + 'require("frame").command.register("test.crash", '
            '{description="crash", handler=function() os.exit(0) end})\n'
        )
        self.assertTrue(self.reload()["ok"])
        self.client.prefix(":")
        self.client.send("test.crash\r")
        self.client.expect_output("Extension host unavailable")
        self.path.write_text(self.initial)
        self.assertTrue(self.reload()["ok"])
        self.client.send(b"\x1bd")
        self.server.wait_for_state(
            self.session.name, lambda s: s.panes == 2, "recovered policy"
        )

    def test_timeout_preserves_terminal_and_allows_another_reload(self) -> None:
        self.path.write_text("while true do end")
        self.assertFalse(self.reload()["ok"])
        self.client.send("AFTER-TIMEOUT\r")
        self.client.expect_output("AFTER-TIMEOUT")
        self.path.write_text(self.initial)
        result = self.reload()
        self.assertTrue(result["ok"], result)


if __name__ == "__main__":
    unittest.main()
