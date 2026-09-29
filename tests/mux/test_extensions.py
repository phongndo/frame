from __future__ import annotations

import json
import os
import signal
import sys
import unittest
from pathlib import Path

from tests.mux.test_extension_runtime import HELLO, PROC, PROC_RESULT, ExtensionPeer
from tests.support.mux_harness import (
    FrameServer,
    SessionState,
    wait_for_process_exit,
    wait_until,
)

CONFIG = r"""
local frame = require("frame")
local function register(name, handler, timeout)
  frame.command.register("test." .. name, {
    description = name, handler = handler, timeout_ms = timeout or 5000,
  })
end
local function rename(ctx, title)
  return ctx:proc({commands={{command="tab.rename", session={id=ctx.session},
                             tab={id=ctx.tab}, title=title}}})
end
register("rename", function(ctx, args)
  assert(#args == 1)
  local result = rename(ctx, args[1])
  assert(result.schema == "frame.proc-result/v1" and result.ok)
  assert(result.results[1].result.status == "applied")
  local inspected = ctx:proc({commands={{command="pane.inspect", session={id=ctx.session},
                                       pane={id=ctx.pane}}}})
  assert(inspected.ok)
end)
register("sequence", function(ctx)
  local result = ctx:proc({commands={
    {id="created", command="tab.new", session={id=ctx.session}, title="created"},
    {command="tab.rename", session={id=ctx.session}, tab={result="created"}, title="referenced"},
  }})
  assert(result.ok and #result.results == 2)
  assert(rename(ctx, "original").ok)
end)
register("invalid", function(ctx)
  local result = ctx:proc({commands={
    {command="tab.new", session={id=ctx.session}, title="must-not-execute"},
    {command="not.a.native.command"},
  }})
  assert(not result.ok)
  assert(rename(ctx, "rejected").ok)
end)
register("wait", function(ctx)
  local result = ctx:proc({commands={
    {command="tab.rename", session={id=ctx.session}, tab={id=ctx.tab}, title="waiting"},
    {command="pane.wait", session={id=ctx.session}, pane={id=ctx.pane},
     contains="__EXTENSION_RELEASE__", timeout_ms=4000},
    {command="tab.new", session={id=ctx.session}, title="after-wait"},
  }})
  assert(result.ok)
end)
register("captured", function(ctx)
  local result = ctx:proc({commands={
    {command="tab.rename", session={id=ctx.session}, tab={id=ctx.tab}, title="captured-wait"},
    {command="pane.wait", session={id=ctx.session}, pane={id=ctx.pane},
     contains="__EXTENSION_RELEASE__", timeout_ms=4000},
  }})
  assert(result.ok)
  assert(rename(ctx, "captured-finished").ok)
end)
register("stale", function(ctx, args)
  ctx:proc({commands={
    {command="tab.rename", session={id=ctx.session}, tab={id=ctx.tab}, title="stale-wait"},
    {command="pane.wait", session={id=ctx.session}, pane={id=args[1]},
     contains="__EXTENSION_RELEASE__", timeout_ms=4000},
  }})
  local result = ctx:proc({commands={{command="pane.input", session={id=ctx.session},
    pane={id=ctx.pane}, events={{kind="text", text="MUST_NOT_REACH_REPLACEMENT"}}}}})
  assert(not result.ok and result.results[1].result.status == "stale")
  assert(rename(ctx, "stale-rejected").ok)
end)
register("invalid-yield", function() local cycle = {}; cycle.self = cycle; coroutine.yield(cycle) end)
register("crash", function() os.exit(0) end)
register("loop", function() while true do end end)
register("block", function()
  local entered = assert(io.open(os.getenv("HOME") .. "/extension-entered", "w"))
  entered:write("entered")
  entered:close()
  io.open(os.getenv("HOME") .. "/extension-fifo", "r")
end, 1500)
"""


class ExtensionMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = FrameServer.from_environment(config_text=CONFIG)
        self.addCleanup(self.server.close)
        self.session = self.server.create_session("extensions")
        self.client = self.session.require_client()

    def invoke(self, command: str) -> None:
        self.client.prefix(":")
        self.client.send(command + "\r")

    def titles(self) -> list[str]:
        result = self.server.require_command(
            "proc", "tab", "list", "--session", self.session.name
        )
        return [
            tab["title"]
            for tab in json.loads(result.output)["results"][0]["result"]["tabs"]
        ]

    def title(self, title: str) -> SessionState:
        return self.server.wait_for_state(
            self.session.name,
            lambda _state: title in self.titles(),
            f"extension command to publish title {title!r}",
        )

    def release(self, pane: str) -> None:
        self.server.require_command(
            "proc",
            "pane",
            "input",
            "--session",
            self.session.name,
            "--pane",
            pane,
            "--paste",
            "printf '__EXTENSION_RELEASE__\\n'",
            "--key",
            "enter",
        )

    def test_completion_literal_arguments_and_coroutine_results(self) -> None:
        self.client.prefix(":")
        self.client.send("test.ren\t")
        self.client.expect_output("test.rename")
        self.client.send("'two words'\r")
        self.title("two words")
        # A second invocation runs after the first coroutine receives its Proc results.
        self.invoke("test.rename '$HOME'")
        self.title("$HOME")
        self.assertNotIn("Error:", self.client.screen_text())

    def test_ordered_backward_references_and_a_second_proc(self) -> None:
        self.invoke("test.sequence")
        state = self.title("original")
        self.assertEqual(state.tabs, 2)
        self.assertIn("referenced", self.titles())

    def test_invalid_proc_does_not_execute_its_valid_prefix(self) -> None:
        self.invoke("test.invalid")
        self.assertEqual(self.title("rejected").tabs, 1)

    def test_waiting_callback_does_not_block_other_callbacks_or_terminal_input(
        self,
    ) -> None:
        pane = self.session.state().focused_pane
        self.invoke("test.wait")
        self.title("waiting")
        self.invoke("test.rename 'still responsive'")
        self.title("still responsive")
        self.client.send("printf '__NATIVE_INPUT_PROGRESS__\\n'\r")
        self.client.expect_output("__NATIVE_INPUT_PROGRESS__")
        self.release(pane)
        self.assertEqual(self.title("after-wait").tabs, 2)

    def test_detach_cancels_remaining_proc_commands_before_reattachment(self) -> None:
        pane = self.session.state().focused_pane
        self.invoke("test.wait")
        self.title("waiting")
        self.session.detach()
        self.release(pane)
        self.client = self.session.attach()
        self.invoke("test.rename 'reattached'")
        self.assertEqual(self.title("reattached").tabs, 1)

    def test_invocation_keeps_original_targets_when_focus_changes(self) -> None:
        pane = self.session.state().focused_pane
        self.invoke("test.captured")
        self.title("captured-wait")
        self.server.require_command(
            "proc", "tab", "new", "--session", self.session.name, "--title", "other"
        )
        self.release(pane)
        state = self.title("captured-finished")
        self.assertIn("other", self.titles())
        self.assertEqual(state.tabs, 2)

    def test_stale_pane_generation_cannot_address_a_replacement(self) -> None:
        original = self.session.state().focused_pane
        self.server.require_command(
            "proc",
            "pane",
            "split",
            "--session",
            self.session.name,
            "--pane",
            original,
            "--right",
            "--focus",
            "preserve",
        )
        gate = next(
            pane.id for pane in self.session.state().pane_states if pane.id != original
        )
        self.invoke(f"test.stale {gate}")
        self.title("stale-wait")
        self.server.require_command(
            "proc", "pane", "kill", "--session", self.session.name, "--pane", original
        )
        self.server.require_command(
            "proc",
            "pane",
            "split",
            "--session",
            self.session.name,
            "--pane",
            gate,
            "--right",
            "--focus",
            "preserve",
        )
        replacement = next(
            pane.id for pane in self.session.state().pane_states if pane.id != gate
        )
        self.assertEqual(original.split(":")[0], replacement.split(":")[0])
        self.assertNotEqual(original, replacement)
        self.release(gate)
        self.title("stale-rejected")

    def test_session_switch_cancels_source_invocation(self) -> None:
        # Force the switch redraw to need ongoing PTY reads on Linux too; Darwin can
        # exhaust its output queue at the default geometry. Polling the destination
        # must keep draining the client that was originally attached to the source.
        self.client.resize(300, 150)
        self.server.wait_for_state(
            self.session.name,
            lambda state: (state.columns, state.rows) == (300, 150),
            "large viewport before switching sessions",
        )
        source = self.session
        pane = source.state().focused_pane
        target = self.server.create_session("destination", attach=False)
        self.invoke("test.wait")
        self.title("waiting")
        self.invoke("switch destination")
        self.server.wait_for_state(
            source.name, lambda state: not state.attached, "source to detach"
        )
        self.release(pane)
        self.session = target
        self.invoke("test.rename destination-ready")
        self.title("destination-ready")
        self.assertEqual(source.state().tabs, 1)

    def test_invalid_yield_is_bounded_and_other_commands_recover(self) -> None:
        self.invoke("test.invalid-yield")
        self.client.expect_output("invalid extension yield")
        self.invoke("test.rename recovered")
        self.title("recovered")

    def test_host_crash_preserves_panes_and_removes_command_discovery(self) -> None:
        self.invoke("test.crash")
        self.client.expect_output("Extension host unavailable")
        self.invoke("test.rename unused")
        self.client.expect_output("Error: Unknown command")
        self.client.send("printf '__AFTER_HOST_CRASH__\\n'\r")
        self.client.expect_output("__AFTER_HOST_CRASH__")
        self.assertEqual(self.session.state().panes, 1)

    def test_instruction_budget_failure_does_not_disable_other_commands(self) -> None:
        self.invoke("test.loop")
        self.client.expect_output("instruction budget exceeded")
        self.invoke("test.rename recovered")
        self.title("recovered")

    def test_detach_does_not_remove_a_blocked_hosts_watchdog(self) -> None:
        os.mkfifo(Path(self.server.environment["HOME"]) / "extension-fifo")
        self.invoke("test.block")
        marker = Path(self.server.environment["HOME"]) / "extension-entered"
        wait_until(
            "blocking callback to start", lambda: True if marker.exists() else None
        )
        self.session.detach()
        self.client = self.session.attach()
        self.invoke("test.rename must-not-run")
        self.client.expect_output("Extension command timed out")
        self.assertNotIn("must-not-run", self.titles())

    def test_blocking_native_callback_isolated_and_terminated_by_deadline(self) -> None:
        os.mkfifo(Path(self.server.environment["HOME"]) / "extension-fifo")
        self.invoke("test.block")
        marker = Path(self.server.environment["HOME"]) / "extension-entered"
        wait_until(
            "blocking callback to start", lambda: True if marker.exists() else None
        )
        self.client.send("printf '__WHILE_HOST_BLOCKED__\\n'\r")
        self.client.expect_output("__WHILE_HOST_BLOCKED__")
        self.client.expect_output("Extension command timed out")
        self.invoke("test.rename unused")
        self.client.expect_output("Error: Unknown command")


class PickerMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        picker = Path(__file__).resolve().parents[2] / "extensions/picker.py"
        wrapper = (
            "import json,os,runpy,sys; from pathlib import Path; "
            f"sys.path.insert(0, {str(picker.parent)!r}); "
            "ctx=json.loads(os.environ['FRAME_COMMAND_CONTEXT']); "
            "ctx['pid']=os.getpid(); "
            "Path(os.environ['HOME'], 'picker-context.json').write_text(json.dumps(ctx)); "
            f"runpy.run_path({str(picker)!r}, run_name='__main__')"
        )
        config = f"""
local frame = require("frame")
frame.command.register("nav.pick", {{description="Pick a target", timeout_ms=15000,
  argv={{{json.dumps(sys.executable)}, "-c", {json.dumps(wrapper)}}}}})
frame.command.register("test.exit", {{description="Fail without killing the host",
  argv={{{json.dumps(sys.executable)}, "-c", "raise SystemExit(7)"}}}})
frame.command.register("test.bound", {{description="A keybound Lua callback",
  handler=function(ctx)
    assert(ctx.connection and ctx.endpoint)
    assert(ctx:proc({{commands={{{{command="tab.rename", session={{id=ctx.session}},
      tab={{id=ctx.tab}}, title="keybound"}}}}}}).ok)
  end}})
frame.keymap.set("prefix", "p", "nav.pick")
frame.keymap.set("prefix", "f", "test.exit")
frame.keymap.set("prefix", "b", "test.bound")
frame.command.register("test.flood", {{description="Bound child diagnostics",
  argv={{{json.dumps(sys.executable)}, "-c", "import os; os.write(1, b'x'*8192)"}}}})
frame.keymap.set("prefix", "v", "test.flood")
frame.command.register("test.hostcrash", {{description="Exit the shared host",
  handler=function() os.exit(0) end}})
frame.keymap.set("prefix", "x", "test.hostcrash")
frame.command.register("test.hosttimeout", {{description="Block the shared host", timeout_ms=150,
  handler=function() os.execute("sleep 10") end}})
frame.keymap.set("prefix", "z", "test.hosttimeout")
"""
        self.config = config
        self.server = FrameServer.from_environment(config_text=config)
        self.addCleanup(self.server.close)
        self.session = self.server.create_session("source", command=("cat",))
        self.client = self.session.require_client()
        self.peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(self.peer.close)
        self.peer.send(
            HELLO,
            1,
            {
                "schema": "frame.extension/v1",
                "name": "picker-test",
                "capabilities": ["proc"],
            },
        )
        self.peer.receive_matching(2, 1)
        self.sequence = 1

    def proc(self, command: str, **fields: object) -> dict:
        self.sequence += 1
        self.peer.send(
            PROC,
            self.sequence,
            {"schema": "frame.proc/v1", "commands": [{"command": command, **fields}]},
        )
        result = self.peer.receive_matching(PROC_RESULT, self.sequence)
        self.assertTrue(result["results"], result)
        return result["results"][0]["result"]

    def open_picker(self) -> dict:
        self.client.prefix("p")
        self.client.expect_output("Sessions - Frame picker")
        return json.loads(
            (Path(self.server.environment["HOME"]) / "picker-context.json").read_text()
        )

    def test_host_crash_revokes_its_running_picker(self) -> None:
        context = self.open_picker()
        self.client.prefix("x")
        wait_for_process_exit(context["pid"])
        self.client.send("HOST_CRASH_RECOVERED\n")
        self.session.pane().expect_output("HOST_CRASH_RECOVERED")

    def test_host_watchdog_revokes_its_running_picker(self) -> None:
        context = self.open_picker()
        self.client.prefix("z")
        wait_for_process_exit(context["pid"])
        self.client.send("HOST_TIMEOUT_RECOVERED\n")
        self.session.pane().expect_output("HOST_TIMEOUT_RECOVERED")

    def test_external_output_is_bounded_without_disabling_the_host(self) -> None:
        self.client.prefix("v")
        self.client.expect_output("external command output limit exceeded")
        self.client.prefix("b")
        self.client.expect_output("keybound")

    def test_visible_picker_does_not_block_lua_or_terminal_progress(self) -> None:
        context = self.open_picker()
        result = self.proc(
            "pane.send",
            session={"id": context["session"]},
            pane={"id": context["pane"]},
            text="UNDER_PICKER\n",
        )
        self.assertEqual(result["status"], "applied", result)
        observed = self.proc(
            "pane.wait",
            session={"id": context["session"]},
            pane={"id": context["pane"]},
            contains="UNDER_PICKER",
            timeout_ms=2000,
        )
        self.assertEqual(observed["status"], "applied", observed)
        self.client.prefix("b")
        self.client.expect_output("keybound")
        self.client.send("q")
        wait_for_process_exit(context["pid"])

    def test_keybinding_works_without_native_status_line(self) -> None:
        server = FrameServer.from_environment(
            config_text=self.config + "\nframe.setup({ui={status_line=false}})"
        )
        self.addCleanup(server.close)
        session = server.create_session("no-status", command=("cat",))
        client = session.require_client()
        panes = server.require_command(
            "proc", "pane", "list", "--session", session.name
        )
        self.assertEqual(
            json.loads(panes.output)["results"][0]["result"]["panes"][0]["rows"], 24
        )
        client.prefix("p")
        client.expect_output("Sessions - Frame picker")
        context = json.loads(
            (Path(server.environment["HOME"]) / "picker-context.json").read_text()
        )
        client.send("q")
        wait_for_process_exit(context["pid"])
        client.send("NO_STATUS_INPUT\n")
        session.pane().expect_output("NO_STATUS_INPUT")

    def test_keybinding_captures_before_later_keys_change_focus(self) -> None:
        self.client.send(b"\x02b\x02c")
        self.server.wait_for_state(
            self.session.name, lambda state: state.tabs == 2, "later key creates Tab"
        )

        def renamed() -> bool | None:
            tabs = self.proc("tab.list", session={"id": self.session.state().id})[
                "tabs"
            ]
            return (
                True
                if tabs[0]["title"] == "keybound" and tabs[1]["title"] != "keybound"
                else None
            )

        wait_until("original captured Tab renamed", renamed)

    def test_external_arguments_are_literal_and_picker_is_discoverable(self) -> None:
        self.client.prefix(":")
        self.client.send("nav.pi\t")
        self.client.expect_output("nav.pick")
        self.client.send("'$HOME' 'two words'\r")
        self.client.expect_output("Sessions - Frame picker")
        context = json.loads(
            (Path(self.server.environment["HOME"]) / "picker-context.json").read_text()
        )
        self.assertEqual(context["args"], ["$HOME", "two words"])
        self.client.send("q")
        wait_for_process_exit(context["pid"])

    def test_keybound_lua_and_external_crash_recovery(self) -> None:
        self.client.prefix("b")
        self.client.expect_output("keybound")
        self.client.prefix("f")
        self.client.expect_output("external command exited with status 7")
        context = self.open_picker()
        self.assertEqual(context["session"], self.session.state().id)
        self.client.send("q")
        wait_for_process_exit(context["pid"])
        self.client.send("AFTER_PICKER\n")
        self.session.pane().expect_output("AFTER_PICKER")

    def test_session_switch_is_connection_scoped_and_rejects_stale_ids(self) -> None:
        target = self.server.create_session("target", attach=False, command=("cat",))
        busy = self.server.create_session("busy", command=("cat",))
        context = self.open_picker()
        same = self.proc(
            "attachment.switch",
            connection=context["connection"],
            session={"id": context["session"]},
        )
        self.assertEqual(same["status"], "no_effect", same)
        self.assertEqual(same["connection"], context["connection"])
        conflict = self.proc(
            "attachment.switch",
            connection=context["connection"],
            session={"id": busy.state().id},
        )
        self.assertEqual(conflict["status"], "conflict", conflict)
        self.assertEqual(conflict["error"]["reason"], "target_attached")
        self.client.send("j\r")
        self.server.wait_for_state(
            target.name, lambda state: state.attached, "picker session switch"
        )
        self.assertFalse(self.session.state().attached)
        self.assertTrue(busy.state().attached)
        wait_for_process_exit(context["pid"])
        stale = self.proc(
            "attachment.switch",
            connection=context["connection"],
            session={"id": context["session"]},
        )
        self.assertEqual(stale["status"], "stale", stale)
        self.client.send("TARGET_INPUT\n")
        self.client.expect_output("TARGET_INPUT")

    def test_connection_identity_survives_session_slot_reuse(self) -> None:
        keep = self.server.create_session("keep", attach=False, command=("cat",))
        original = self.open_picker()
        self.session.destroy()
        wait_for_process_exit(original["pid"])
        self.session = self.server.create_session("replacement", command=("cat",))
        self.client = self.session.require_client()
        fresh = self.open_picker()
        self.assertEqual(
            original["session"].split(":")[0], fresh["session"].split(":")[0]
        )
        self.assertNotEqual(original["connection"], fresh["connection"])
        result = self.proc(
            "attachment.switch",
            connection=original["connection"],
            session={"id": keep.state().id},
        )
        self.assertEqual(result["status"], "stale", result)
        self.assertTrue(self.session.state().attached)
        self.client.send("q")
        wait_for_process_exit(fresh["pid"])

    def test_picker_selects_an_inactive_tab_and_its_pane(self) -> None:
        session = {"id": self.session.state().id}
        tab = self.proc(
            "tab.new",
            session=session,
            title="chosen-tab",
            focus="preserve",
            argv=["cat"],
        )
        pane = self.proc(
            "pane.split",
            session=session,
            pane={"id": tab["pane"]},
            direction="right",
            focus="preserve",
            argv=["cat"],
        )
        context = self.open_picker()
        self.client.send("l")
        self.client.expect_output("Tabs - Frame picker")
        self.client.send("jl")
        self.client.expect_output("Panes - Frame picker")
        self.client.send("j\r")
        self.server.wait_for_state(
            self.session.name,
            lambda state: (
                state.active_tab == tab["tab"] and state.focused_pane == pane["pane"]
            ),
            "picker tab and pane selection",
        )
        wait_for_process_exit(context["pid"])
        self.client.send("SELECTED_PANE\n")
        self.session.pane().expect_output("SELECTED_PANE")

    def test_stale_target_does_not_switch_or_capture_input_after_close(self) -> None:
        target = self.server.create_session("vanishing", attach=False, command=("cat",))
        context = self.open_picker()
        target.destroy()
        self.client.send("j\r")
        self.client.expect_output("Not selected: stale")
        self.assertTrue(self.session.state().attached)
        self.client.send("q")
        wait_for_process_exit(context["pid"])
        self.client.send("STILL_SOURCE\n")
        self.session.pane().expect_output("STILL_SOURCE")

    def test_detach_and_helper_crash_revoke_ui_and_allow_reinvocation(self) -> None:
        context = self.open_picker()
        self.session.detach()
        wait_for_process_exit(context["pid"])
        self.client = self.session.attach()
        fresh = self.open_picker()
        self.assertNotEqual(fresh["connection"], context["connection"])
        os.kill(fresh["pid"], signal.SIGKILL)
        wait_for_process_exit(fresh["pid"])
        self.client.expect_output("external command terminated")
        self.client.send("CRASH_RECOVERED\n")
        self.session.pane().expect_output("CRASH_RECOVERED")
        last = self.open_picker()
        self.client.send("q")
        wait_for_process_exit(last["pid"])


if __name__ == "__main__":
    unittest.main()
