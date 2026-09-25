from __future__ import annotations

import json
import os
import signal
import sys
import unittest

from tests.mux.test_extension_runtime import (
    EVENT,
    HELLO,
    PROC,
    PROC_RESULT,
    ExtensionPeer,
)
from tests.support.mux_harness import (
    Client,
    LemmaServer,
    wait_for_process_exit,
    wait_until,
)


class UserExtensionsMuxTest(unittest.TestCase):
    def server(self, config: str | None = None) -> LemmaServer:
        server = LemmaServer.from_environment(config_text=config)
        self.addCleanup(server.close)
        return server

    def test_invalid_configuration_falls_back_to_the_shipped_user_layer(self) -> None:
        server = self.server('require("lemma").extension.set("invalid", {})')
        session = server.create_session("fallback", command=("cat",))
        client = session.require_client()
        client.expect_output("fallback  |")
        self.assertIn("configuration rejected", server.logs())
        client.prefix("s")
        client.expect_output("Session")
        client.send(b"\x03")
        client.expect_output("fallback  |")

    def test_status_is_replaceable_and_pane_uses_the_released_row(self) -> None:
        server = self.server('require("lemma").extension.set("statusline", false)')
        session = server.create_session("bare", command=("cat",))
        session.require_client().send("__NATIVE_INPUT__\r")
        session.require_client().expect_output("__NATIVE_INPUT__")
        state = json.loads(session.state().raw)
        self.assertEqual(state["panes"][0]["row"], 0)
        self.assertEqual(state["panes"][0]["rows"], 24)
        self.assertNotIn("bare  |", session.require_client().screen_text())

    def test_status_mouse_selects_creates_and_reorders_tabs(self) -> None:
        server = self.server()
        session = server.create_session("mouse", command=("cat",))
        client = session.require_client()
        first = session.state().active_tab
        server.require_command(
            "proc", "tab", "rename", "--session", "mouse", "--tab", first, "alpha"
        )
        client.expect_output("alpha")

        def column(label: str) -> int:
            client.drain()
            return client.screen_text().splitlines()[0].index(label) + 1

        plus = column("+")
        client.send(f"\x1b[<0;{plus};1M\x1b[<0;{plus};1m")
        second = server.wait_for_state(
            "mouse", lambda state: state.tabs == 2, "status plus creates tab"
        ).active_tab
        self.assertNotEqual(first, second)
        server.require_command(
            "proc", "tab", "rename", "--session", "mouse", "--tab", second, "beta"
        )
        client.expect_output("beta")
        alpha = column("alpha")
        client.send(f"\x1b[<0;{alpha};1M\x1b[<0;{alpha};1m")
        server.wait_for_state(
            "mouse", lambda state: state.active_tab == first, "status selects tab"
        )
        beta = column("beta")
        client.send(f"\x1b[<0;{alpha};1M\x1b[<32;{beta};1M\x1b[<0;{beta};1m")

        def order() -> bool | None:
            response = server.require_command(
                "proc", "tab", "list", "--session", "mouse"
            )
            tabs = json.loads(response.output)["results"][0]["result"]["tabs"]
            return True if [tab["id"] for tab in tabs] == [second, first] else None

        wait_until("status drag commits tab order", order)
        client.send("__AFTER_DRAG__\r")
        client.expect_output("__AFTER_DRAG__")

    def test_status_marks_inactive_tabs_needing_attention_until_visited(
        self,
    ) -> None:
        server = self.server()
        session = server.create_session("attn", attach=False, command=("cat",))
        home = session.state().active_tab
        server.require_command(
            "proc", "tab", "rename", "--session", "attn", "--tab", home, "home"
        )
        # Each read gates one phase; the first bell predates the attachment.
        script = (
            r"printf '\007'; read step; "
            r"printf '\007'; read step; "
            r"printf '\033]9;agent done\007'; read step; "
            r"printf '\007'; read step; "
            r"printf '\033]9;4;1;40\033\\'; read step; "
            r"printf '\033]9;4;0\033\\\033]133;A\007\033]133;B\007"
            r"\033]133;C\007\033]133;D;3\007'; read step; "
            r"printf '\007'; sleep 30"
        )
        created = server.require_command(
            "proc",
            "tab",
            "new",
            "--session",
            "attn",
            "--title",
            "work",
            "--focus",
            "preserve",
            "--",
            "/bin/sh",
            "-c",
            script,
        )
        result = json.loads(created.output)["results"][0]["result"]
        work, pane = result["tab"], result["pane"]

        def bells() -> int:
            listed = server.require_command("proc", "pane", "list", "--session", "attn")
            panes = json.loads(listed.output)["results"][0]["result"]["panes"]
            return next(item for item in panes if item["id"] == pane)["signals"][
                "bells"
            ]

        wait_until("pre-attach bell", lambda: True if bells() == 1 else None)
        session.attach()

        def row(description: str, expected: str) -> None:
            attached = session.require_client()

            def observe() -> str | None:
                attached.drain()
                line = attached.screen_text().splitlines()[0]
                return line if expected in line.split("|", 1)[-1] else None

            wait_until(description, observe, diagnostics=lambda: attached.screen_text())

        def advance() -> None:
            server.require_command(
                "proc",
                "pane",
                "input",
                "--session",
                "attn",
                "--pane",
                pane,
                "--key",
                "enter",
            )

        def select(tab: str) -> None:
            server.require_command(
                "proc", "tab", "select", "--session", "attn", "--tab", tab
            )

        # Signals present when the statusline attaches are already seen.
        row("attach baseline", "[ 1:home ]  2:work  +")
        advance()
        row("bell marks the inactive tab", "[ 1:home ]  2:work !  +")
        select(work)
        row("visiting clears the marker", "1:home  [ 2:work ]  +")
        select(home)
        row("a visited bell stays seen", "[ 1:home ]  2:work  +")
        advance()
        row("notification marks the inactive tab", "[ 1:home ]  2:work !  +")
        select(work)
        row("visiting clears the notification", "1:home  [ 2:work ]  +")
        advance()
        wait_until("active bell", lambda: True if bells() == 3 else None)
        session.require_client().drain(0.1)
        self.assertIn(
            "1:home  [ 2:work ]  +",
            session.require_client().screen_text().splitlines()[0],
        )
        select(home)
        row("a bell on the active tab is seen", "[ 1:home ]  2:work  +")
        advance()
        row("progress marks the inactive tab", "[ 1:home ]  2:work 40%  +")
        advance()
        row("a failed command marks the inactive tab", "[ 1:home ]  2:work x  +")
        select(work)
        row("visiting clears the failure", "1:home  [ 2:work ]  +")
        select(home)
        row("a visited failure stays seen", "[ 1:home ]  2:work  +")

        # Attention retained across detach marks what happened while detached.
        session.detach()
        advance()
        wait_until("detached bell", lambda: True if bells() == 4 else None)
        session.attach()
        row("a bell while detached marks the tab", "[ 1:home ]  2:work !  +")

    def test_status_recovers_after_tiny_resize_and_reattach(self) -> None:
        server = self.server()
        session = server.create_session("geometry", command=("cat",))
        client = session.require_client()
        client.expect_output("geometry  |")
        client.resize(10, 1)
        tiny = server.wait_for_state(
            "geometry", lambda state: state.rows == 1, "single row terminal"
        )
        self.assertEqual(json.loads(tiny.raw)["panes"][0]["rows"], 1)
        client.resize(100, 30)
        server.wait_for_state(
            "geometry", lambda state: state.rows == 30, "restored terminal"
        )
        client.expect_output("geometry  |")
        self.assertEqual(json.loads(session.state().raw)["panes"][0]["rows"], 29)
        session.detach()
        reattached = session.attach(columns=60, rows=12)
        reattached.expect_output("geometry  |")
        self.assertEqual(json.loads(session.state().raw)["panes"][0]["rows"], 11)
        reattached.send("__REATTACHED__\r")
        reattached.expect_output("__REATTACHED__")

    def test_global_discovery_and_presentation_do_not_require_screen_observation(
        self,
    ) -> None:
        server = self.server()
        global_peer = ExtensionPeer(str(server.socket_path))
        self.addCleanup(global_peer.close)
        global_peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "global-observer",
                "capabilities": ["observe"],
                "events": {"schema": "lemma.events/v1"},
            },
        )
        welcome = global_peer.receive_matching(2, 1)
        self.assertNotIn("attachment", welcome)
        self.assertEqual(global_peer.receive_matching(EVENT)["sessions"], [])
        session = server.create_session("observed", command=("cat",))
        self.assertEqual(
            global_peer.receive_matching(EVENT)["sessions"][0]["name"], "observed"
        )
        peer = ExtensionPeer(str(server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "presentation-observer",
                "capabilities": ["observe", "proc"],
                "events": {
                    "schema": "lemma.events/v1",
                    "session": {"id": session.state().id},
                    "presentation": True,
                },
            },
        )
        peer.receive_matching(2, 1)
        initial = peer.receive_matching(EVENT)
        self.assertEqual(initial["presentation"]["name"], "observed")
        self.assertNotIn("capture", initial)
        client = session.require_client()
        client.expect_output("observed  |")
        client.prefix(":")
        client.send("tab list")
        for _ in range(32):
            event = peer.receive_matching(EVENT)
            self.assertNotIn("capture", event)
            prompt = event.get("presentation", {}).get("prompt", {})
            if prompt.get("value") == "tab list":
                self.assertEqual(prompt["kind"], "command")
                break
        else:
            self.fail("editor state was not observed")
        client.send(b"\x03__AFTER_EDITOR__\r")
        client.expect_output("__AFTER_EDITOR__")

    def test_session_manager_switches_creates_and_respects_busy_attachments(
        self,
    ) -> None:
        server = self.server()
        first = server.create_session("first", command=("cat",))
        target = server.create_session("second", attach=False, command=("cat",))
        client = first.require_client()
        client.prefix("s")
        client.expect_output("Session")
        client.expect_output("second /")
        client.send("second\r")
        server.wait_for_state(
            target.name, lambda state: state.attached, "session manager switch"
        )
        client.expect_output("second  |")
        client.prefix("s")
        client.expect_output("Session")
        client.send(b"\x0f")
        client.expect_output("New session")
        client.send("third\r")
        server.wait_for_state(
            "third", lambda state: state.attached, "session manager creation"
        )
        client.expect_output("third  |")
        busy = server.create_session("busy", command=("cat",))
        client.prefix("s")
        client.expect_output("Session")
        client.expect_output("busy /")
        client.send("busy\r")
        client.expect_output("Cannot switch:")
        self.assertTrue(busy.state().attached)
        third = server.session_state("third")
        assert third is not None
        self.assertTrue(third.attached)
        client.send(b"\x03")
        client.expect_output("third  |")

    def test_status_survives_command_host_failure_and_has_bounded_restart(self) -> None:
        code = (
            "import os,sys; "
            "f=open(os.path.join(os.environ['HOME'],'status-pids'),'a'); "
            "f.write(str(os.getpid())+'\\n'); f.close(); "
            "os.execv(sys.argv[1],[sys.argv[1],'status'])"
        )
        config = f"""
local lemma = require('lemma')
lemma.extension.set('statusline', {{{json.dumps(sys.executable)}, '-c', {json.dumps(code)}, lemma.bundled_ui}})
lemma.command.register('test.crash', {{description='crash', handler=function() os.exit(0) end}})
"""
        server = self.server(config)
        session = server.create_session("recovery", command=("cat",))
        client = session.require_client()
        client.expect_output("recovery  |")
        client.prefix(":")
        client.send("test.crash\r")
        client.expect_output("Error:")
        client.prefix("r")
        client.send(b"\x15alive\r")
        client.expect_output("alive")
        path = server.root / "home" / "status-pids"

        def pids() -> list[int]:
            return [int(value) for value in path.read_text().splitlines()]

        for count in range(1, 5):
            wait_until(
                "managed status launch", lambda: True if len(pids()) == count else None
            )
            process = pids()[-1]
            os.kill(process, signal.SIGKILL)
            wait_for_process_exit(process)
            if count < 4:
                wait_until(
                    "managed status restart",
                    lambda: True if len(pids()) == count + 1 else None,
                )
                client.expect_output("recovery  |")
        wait_until(
            "dock removed after restart budget",
            lambda: (
                True
                if json.loads(session.state().raw)["panes"][0]["rows"] == 24
                else None
            ),
        )
        client.send("__AFTER_CRASH_LOOP__\r")
        client.expect_output("__AFTER_CRASH_LOOP__")
        self.assertEqual(len(pids()), 4)

    def test_disabling_surface_focus_returns_input_to_pane(self) -> None:
        server = self.server()
        session = server.create_session("focus", command=("cat",))
        peer = ExtensionPeer(str(server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "focusable",
                "capabilities": ["proc", "surface"],
                "events": {
                    "schema": "lemma.events/v1",
                    "session": {"id": session.state().id},
                },
            },
        )
        peer.receive_matching(2, 1)
        placement = {"kind": "float", "column": 0, "row": 2, "columns": 10, "rows": 1}
        peer.send(
            PROC,
            2,
            {
                "schema": "lemma.proc/v1",
                "commands": [{"command": "surface.create", "placement": placement}],
            },
        )
        surface = peer.receive_matching(PROC_RESULT, 2)["results"][0]["result"][
            "surface"
        ]
        peer.send(
            PROC,
            3,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {"command": "surface.focus", "surface": {"id": surface}},
                    {
                        "command": "surface.configure",
                        "surface": {"id": surface},
                        "placement": placement,
                        "focusable": False,
                    },
                ],
            },
        )
        self.assertTrue(peer.receive_matching(PROC_RESULT, 3)["ok"])
        session.require_client().send("__PANE_FOCUS__\r")
        session.require_client().expect_output("__PANE_FOCUS__")


class SessionPickerMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)
        self.source = self.server.create_session("source", command=("cat",))
        self.client = self.source.require_client()
        self.peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(self.peer.close)
        self.peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
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
            {"schema": "lemma.proc/v1", "commands": [{"command": command, **fields}]},
        )
        result = self.peer.receive_matching(PROC_RESULT, self.sequence)
        self.assertTrue(result["ok"], result)
        return result["results"][0]["result"]

    def screen(self, client: Client | None = None) -> str:
        client = client or self.client
        client.drain(0.002)
        return client.screen_text()

    def expect_screen(self, marker: str, *, absent: bool = False) -> str:
        return wait_until(
            f"picker {'without' if absent else 'with'} {marker!r}",
            lambda: screen if (marker in (screen := self.screen())) != absent else None,
            diagnostics=self.client.diagnostics,
        )

    def open(self) -> None:
        self.client.prefix("s")
        self.expect_screen("Session")

    def picker_position(self) -> tuple[int, int, int]:
        lines = self.screen().splitlines()
        title_row = next(i for i, line in enumerate(lines) if "Session" in line)
        prompt = lines[title_row + 1]
        # The prompt and its count track both edges without depending on Unicode borders.
        return title_row, prompt.index(">"), len(prompt.rstrip())

    def test_directories_and_fitted_size_survive_filtering_and_back(self) -> None:
        target = self.server.create_session("project", attach=False, command=("cat",))
        scope = {"id": target.state().id}
        # Full paths are searchable; punctuation keeps the query from matching
        # an accidental subsequence in the shared random temporary directory.
        alpha = self.server.root / "home" / "alpha!"
        beta = self.server.root / "home" / "beta"
        alpha.mkdir()
        beta.mkdir()
        tab = self.proc(
            "tab.new", session=scope, title="editor", cwd=str(alpha), argv=["cat"]
        )
        pane = self.proc(
            "pane.split",
            session=scope,
            pane={"id": tab["pane"]},
            direction="right",
            cwd=str(beta),
            argv=["cat"],
        )
        self.proc("pane.focus", session=scope, pane={"id": pane["pane"]})
        self.open()
        self.expect_screen("~/beta  2 panes")
        bounds = self.picker_position()
        self.assertGreater(bounds[0], 5)
        self.assertLess(bounds[2] - bounds[1], 60)
        # The random parent paths can fuzzy-match words; none contains a question mark.
        self.client.send("not-found?")
        self.expect_screen("No matches")
        self.assertEqual(self.picker_position(), bounds)
        # A directory in an unfocused pane still finds its containing Tab.
        self.client.send(b"\x15alpha!")
        self.expect_screen("1/3")
        self.expect_screen("project / 2:editor")
        self.assertEqual(self.picker_position(), bounds)
        self.client.send(b"\t")
        panes = self.expect_screen("~/alpha!")
        self.assertIn("~/beta", panes)
        pane_bounds = self.picker_position()
        self.client.send("alpha!")
        self.expect_screen("1/2")
        self.assertEqual(self.picker_position(), pane_bounds)
        self.client.send(b"\x1b[Z")
        self.expect_screen("> alpha!")
        self.assertEqual(self.picker_position(), bounds)
        self.assertFalse(target.state().attached)
        self.client.send(b"\talpha!\r")
        self.server.wait_for_state(
            target.name,
            lambda state: state.attached and state.focused_pane == tab["pane"],
            "directory search activates the chosen pane after browsing back",
        )

    def test_root_shows_tabs_and_activates_one_without_search_or_browsing(self) -> None:
        target = self.server.create_session("project", attach=False, command=("cat",))
        initial = target.state()
        tab = self.proc(
            "tab.new",
            session={"id": initial.id},
            title="editor",
            focus="preserve",
            argv=["cat"],
        )
        self.open()
        screen = self.expect_screen("project / 2:editor")
        self.expect_screen("3/3")
        self.assertNotIn("1 pane", screen)
        self.assertNotIn("1 tab", screen)
        self.assertEqual(target.state().active_tab, initial.active_tab)
        # source's shell Tab, project's shell Tab, then project's editor Tab.
        self.client.send(b"\x0e\x0e\r")
        self.server.wait_for_state(
            target.name,
            lambda state: (
                state.attached
                and state.active_tab == tab["tab"]
                and state.focused_pane == tab["pane"]
            ),
            "root Tab selection activates the exact destination",
        )
        self.client.send("__ROOT_TAB__\r")
        self.client.expect_output("__ROOT_TAB__")

    def test_search_disambiguates_identical_tab_names_across_sessions(self) -> None:
        target = self.server.create_session("project", attach=False, command=("cat",))
        for session in (self.source, target):
            self.proc(
                "tab.rename",
                session={"id": session.state().id},
                tab={"id": session.state().active_tab},
                title="editor",
            )
        self.open()
        self.expect_screen("2/2")
        self.client.send("prj edtr")
        self.expect_screen("project / 1:editor")
        self.expect_screen("1/2")
        self.client.send(b"\r")
        self.server.wait_for_state(
            target.name,
            lambda state: state.attached,
            "Session and Tab query selects the matching Session",
        )
        self.assertFalse(self.source.state().attached)

    def test_browse_back_preserves_focus_until_enter(self) -> None:
        initial = self.source.state()
        scope = {"id": initial.id}
        tab = self.proc(
            "tab.new", session=scope, title="editor", focus="preserve", argv=["cat"]
        )
        pane = self.proc(
            "pane.split",
            session=scope,
            pane={"id": tab["pane"]},
            direction="right",
            focus="preserve",
            argv=["cat"],
        )
        self.open()
        self.expect_screen("source / 2:editor")
        self.client.send("editor\t")
        self.expect_screen("Session / source / 2:editor")
        self.client.send(b"\x0e")
        self.expect_screen("2 cat")
        self.assertEqual(self.source.state().active_tab, initial.active_tab)
        self.assertEqual(self.source.state().focused_pane, initial.focused_pane)
        self.client.send(b"\x1b[Z")
        self.expect_screen("> editor")
        self.expect_screen("Session")
        # Back restored the root query and the selected Tab.
        self.client.send(b"\t\x0e\r")
        self.server.wait_for_state(
            self.source.name,
            lambda state: (
                state.active_tab == tab["tab"] and state.focused_pane == pane["pane"]
            ),
            "Enter activates the browsed pane",
        )
        self.client.send("__CHOSEN_PANE__\r")
        self.source.pane().expect_output("__CHOSEN_PANE__")

    def test_multiword_search_paste_and_pane_selection_across_sessions(
        self,
    ) -> None:
        target = self.server.create_session("project", attach=False, command=("cat",))
        scope = {"id": target.state().id}
        tab = self.proc(
            "tab.new", session=scope, title="editor", focus="preserve", argv=["cat"]
        )
        pane = self.proc(
            "pane.split",
            session=scope,
            pane={"id": tab["pane"]},
            direction="right",
            focus="preserve",
            argv=["cat"],
        )
        self.open()
        self.client.send("doesnotexist\r")
        self.expect_screen("No matches")
        self.assertTrue(self.source.state().attached)
        self.assertFalse(target.state().attached)
        self.client.send(b"\x15\x1b[200~prj edtr cat\r\x1b[201~")
        self.expect_screen("project / 2:editor")
        self.assertFalse(target.state().attached)  # Pasted Enter is not activation.
        self.client.send(b"\t")
        self.expect_screen("2 cat")
        self.client.send("2 cat\r")
        self.server.wait_for_state(
            target.name,
            lambda state: (
                state.attached
                and state.active_tab == tab["tab"]
                and state.focused_pane == pane["pane"]
            ),
            "search activates exact pane in another Session",
        )
        self.client.send("__DIRECT_PANE__\r")
        self.client.expect_output("__DIRECT_PANE__")

    def test_resize_preserves_single_list_query_selection_and_input(self) -> None:
        target = self.server.create_session("remote", attach=False, command=("cat",))
        self.client.resize(180, 40)
        self.server.wait_for_state(
            "source", lambda state: state.columns == 180, "wide terminal"
        )
        self.open()
        self.client.send("remote")
        self.expect_screen("> remote")
        wide = self.expect_screen("1/2")
        self.assertNotIn("1 shell", wide)
        self.assertNotIn("Search...", wide)
        self.client.resize(80, 24)
        self.server.wait_for_state(
            "source", lambda state: state.columns == 80, "narrow terminal"
        )
        narrow = self.expect_screen("1/2")
        self.assertNotIn("1 shell", narrow)
        self.expect_screen("> remote")
        self.client.resize(30, 10)
        self.server.wait_for_state(
            "source", lambda state: state.rows == 10, "small terminal"
        )
        self.expect_screen("1 shell", absent=True)
        self.client.resize(10, 1)
        self.server.wait_for_state(
            "source", lambda state: state.rows == 1, "tiny terminal"
        )
        self.expect_screen("Esc clos")
        self.client.resize(180, 40)
        self.server.wait_for_state(
            "source", lambda state: state.rows == 40, "restored terminal"
        )
        self.expect_screen("> remote")
        self.expect_screen("1/2")
        self.client.send(b"\r")
        self.server.wait_for_state(
            target.name, lambda state: state.attached, "resized selection activation"
        )
        self.client.send("__AFTER_RESIZE__\r")
        self.client.expect_output("__AFTER_RESIZE__")

    def test_removed_session_does_not_activate_a_replacement(self) -> None:
        target = self.server.create_session("vanishing", attach=False, command=("cat",))
        original = target.state().id
        self.open()
        self.client.send("vanishing")
        self.expect_screen("vanishing / 1:shell")
        target.destroy()
        replacement = self.server.create_session(
            "vanishing", attach=False, command=("cat",)
        )
        self.assertNotEqual(original, replacement.state().id)
        self.expect_screen("Selection disappeared")
        self.client.send(b"\r\x03")
        self.expect_screen("Session", absent=True)
        self.client.send("__STILL_SOURCE__\r")
        self.client.expect_output("__STILL_SOURCE__")
        self.assertTrue(self.source.state().attached)
        self.assertFalse(replacement.state().attached)

    def test_native_tab_selection_after_growth_closes_picker(self) -> None:
        scope = {"id": self.source.state().id}
        tab = self.proc(
            "tab.new", session=scope, title="native", focus="preserve", argv=["cat"]
        )
        self.open()
        self.client.resize(180, 40)
        self.server.wait_for_state(
            "source", lambda state: state.rows == 40, "grown terminal"
        )
        self.expect_screen("Session")
        self.proc("tab.select", session=scope, tab={"id": tab["tab"]})
        self.expect_screen("Session", absent=True)
        self.client.send("__NATIVE_RECOVERY__\r")
        self.client.expect_output("__NATIVE_RECOVERY__")

    def test_maximum_terminal_keeps_search_and_escape_working(self) -> None:
        self.client.resize(500, 200)
        self.server.wait_for_state(
            "source", lambda state: state.rows == 200, "maximum terminal"
        )
        self.open()
        self.client.send("source 1 cat")
        self.expect_screen("source / 1:shell")
        self.expect_screen("1/1")
        self.client.send(b"\x1b")
        self.expect_screen("Session", absent=True)
        self.client.send("__CLOSED_PICKER__\r")
        self.client.expect_output("__CLOSED_PICKER__")

    def test_busy_pane_search_does_not_change_other_clients_tab(self) -> None:
        busy = self.server.create_session("busy", command=("cat",))
        initial = busy.state()
        scope = {"id": initial.id}
        self.proc(
            "tab.new", session=scope, title="hidden", focus="preserve", argv=["cat"]
        )
        self.open()
        self.client.send("busy hidden cat")
        self.expect_screen("busy / 2:hidden")
        self.client.send(b"\r")
        self.expect_screen("Cannot switch: target_attached")
        self.assertEqual(busy.state().active_tab, initial.active_tab)
        self.assertEqual(busy.state().focused_pane, initial.focused_pane)
        self.assertTrue(self.source.state().attached)

    def test_closed_browsing_tab_returns_to_parent_without_choosing_replacement(
        self,
    ) -> None:
        scope = {"id": self.source.state().id}
        tab = self.proc(
            "tab.new", session=scope, title="temporary", focus="preserve", argv=["cat"]
        )
        self.open()
        self.expect_screen("source / 2:temporary")
        self.client.send("temporary\t")
        # The root row contains the same path; wait for the browsing title before deletion.
        self.expect_screen("Session / source / 2:temporary")
        self.proc("tab.kill", session=scope, tab={"id": tab["tab"]})
        self.expect_screen("Tab closed")
        self.expect_screen("source / 1:shell")
        self.client.send(b"\r")
        self.expect_screen("Tab closed")
        self.assertEqual(self.source.state().tabs, 1)


if __name__ == "__main__":
    unittest.main()
