"""Outer-terminal presentation of the attached Session: window title, attention, and directory."""

from __future__ import annotations

import contextlib
import json
import os
import re
import signal
import socket
import sys
import time
import unittest
from pathlib import Path

from tests.support.mux_harness import (
    Client,
    LemmaServer,
    Session,
    process_exists,
    wait_until,
)

TITLE_SETTER = """
import os, sys
os.write(1, b'\\x1b]2;first\\xc2\\x9d\\xe2\\x98\\x83 title\\x1b\\\\__TITLE_READY__\\r\\n')
for line in sys.stdin:
    name = line.strip().encode()
    os.write(1, b'\\x1b]2;' + name + b'\\x1b\\\\__SET_' + name + b'__\\r\\n')
"""


class OuterTitleMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment(
            config_text='require("lemma").setup({})\\n'
        )
        self.addCleanup(self.server.close)

    @staticmethod
    def latest_title(client: Client) -> bytes | None:
        client.drain(0.01)
        titles = re.findall(rb"\x1b\]2;(.*?)\x1b\\", client.process.output_tail)
        return titles[-1] if titles else None

    def expect_title(self, client: Client, expected: str) -> None:
        wait_until(
            f"outer title {expected!r}",
            lambda: True if self.latest_title(client) == expected.encode() else None,
            diagnostics=lambda: (
                f"latest={self.latest_title(client)!r}\n{self.server.diagnostics()}"
            ),
        )

    def test_title_prefers_tab_name_then_pane_title_then_process_name(self) -> None:
        # The ticking shell has no terminal title, so its label is the process name.
        session = self.server.create_session(
            "title_order",
            command=("/bin/sh", "-c", "while :; do printf .; sleep 0.1; done"),
        )
        client = session.require_client()
        state = session.state()
        left_id = state.focused_pane
        self.expect_title(client, "title_order: sh")

        right_id = self.server.require_command(
            "split",
            "--session",
            session.name,
            "--pane",
            left_id,
            "--right",
            "--",
            sys.executable,
            "-c",
            TITLE_SETTER,
        ).output.strip()
        self.expect_title(client, "title_order: first\u2603 title")
        self.server.require_command(
            "focus", "--session", session.name, "--pane", left_id
        )
        self.expect_title(client, "title_order: sh")
        self.server.require_command(
            "focus", "--session", session.name, "--pane", right_id
        )
        self.expect_title(client, "title_order: first\u2603 title")

        # An explicit Tab name wins over the focused Pane's title and follows Tab selection.
        self.server.require_command(
            "proc",
            "tab",
            "rename",
            "--session",
            session.name,
            "--tab",
            state.active_tab,
            "named",
        )
        self.expect_title(client, "title_order: named")
        client.prefix("c")
        self.server.wait_for_state(
            session.name, lambda current: current.tabs == 2, "second tab to open"
        )
        wait_until(
            "title to follow the new tab",
            lambda: (
                True
                if self.latest_title(client) not in (None, b"title_order: named")
                else None
            ),
        )
        client.prefix("p")
        self.expect_title(client, "title_order: named")

    def test_session_switch_presents_the_target_session_title(self) -> None:
        source = self.server.create_session(
            "title_source", command=(sys.executable, "-c", TITLE_SETTER)
        )
        self.server.create_session(
            "title_target", attach=False, command=(sys.executable, "-c", TITLE_SETTER)
        )
        client = source.require_client()
        self.expect_title(client, "title_source: first\u2603 title")
        client.prefix(":")
        client.send("switch title_target\r")
        self.server.wait_for_state(
            "title_target", lambda state: state.attached, "attachment to switch"
        )
        self.expect_title(client, "title_target: first\u2603 title")

    def test_focused_pane_title_is_sanitized_presented_on_change_and_restored(
        self,
    ) -> None:
        session = self.server.create_session(
            "outer_title", command=(sys.executable, "-c", TITLE_SETTER)
        )
        client = session.require_client()
        client.expect_raw(b"\x1b[22;2t")
        client.expect_output("__TITLE_READY__")
        # The C1 control is dropped; the session name identifies the attachment.
        first = "outer_title: first☃ title".encode()
        client.expect_raw(b"\x1b]2;" + first + b"\x1b\\")
        self.assertNotIn(b"\xc2\x9d", client.process.output_tail)

        # A full redraw without a title change does not repeat the title.
        presented = client.process.output_tail.count(b"\x1b]2;")
        client.resize(100, 30)
        self.server.wait_for_state(
            session.name, lambda state: state.columns == 100, "resize to apply"
        )
        client.drain(0.2)
        self.assertEqual(client.process.output_tail.count(b"\x1b]2;"), presented)

        client.send("second\r")
        client.expect_output("__SET_second__")
        client.expect_raw(b"\x1b]2;outer_title: second\x1b\\")

        # Titles are bounded to 256 bytes, truncated at a code point boundary.
        prefix = b"outer_title: "
        client.send("é" * 200 + "\r")
        visible = (256 - len(prefix)) // 2
        client.expect_raw(b"\x1b]2;" + prefix + "é".encode() * visible + b"\x1b\\")

        # Disabling the option restores the saved title and saves it again for detach.
        config = Path(self.server.environment["XDG_CONFIG_HOME"]) / "lemma/init.lua"
        config.write_text('require("lemma").setup({ ui = { outer_title = false } })\n')
        self.server.require_command("config", "reload")
        client.expect_raw(b"\x1b[23;2t\x1b[22;2t")
        client.send("third\r")
        client.expect_output("__SET_third__")
        client.drain(0.2)
        self.assertNotIn(b"outer_title: third", client.process.output_tail)
        session.detach()


# Each input line holds space-separated HEX or HEX*COUNT segments, so a long sequence fits within
# the tty's canonical line limit.
EMITTER = """
import os, sys
os.write(1, b'__EMIT_READY__\\r\\n')
for index, line in enumerate(sys.stdin):
    data = b''
    for segment in line.split():
        text, _, count = segment.partition('*')
        data += bytes.fromhex(text) * int(count or 1)
    os.write(1, data + b'__EMITTED_%d__\\r\\n' % index)
"""
BELL_FLOOD = """
import os, sys, time
sys.stdin.readline()
for _ in range(20):
    os.write(1, b'\\x07')
    time.sleep(0.01)
time.sleep(60)
"""
PROGRESS_REMOVED = b"\x1b]9;4;0\x1b\\"


# Two Panes notify every second from a shared start. The first three notifications spend the burst;
# afterwards each Pane always has one waiting, and B's latest notification always precedes A's at a
# refill. B also changes progress every 20 ms, so its other signals are always newer than A's.
PERIODIC_NOTIFIER = """
import os, sys, time
start, name = float(sys.argv[1]), sys.argv[2]
offsets = [0.0, 0.05] + [0.6 + k for k in range(16)] if name == 'a' else [
    0.1 + k for k in range(16)
]
step = 0
for index, offset in enumerate(offsets):
    while name == 'b' and time.time() < start + offset:
        step += 1
        os.write(1, b'\\x1b]9;4;1;%d\\x1b\\\\' % (step % 100))
        time.sleep(0.02)
    time.sleep(max(0.0, start + offset - time.time()))
    os.write(1, b'\\x1b]9;%s %d\\x07' % (name.encode(), index))
time.sleep(60)
"""
PROGRESS_STEPS = """
import os, sys, time
sys.stdin.readline()
for percent in range(1, 41):
    os.write(1, b'\\x1b]9;4;1;%d\\x1b\\\\' % percent)
    time.sleep(0.01)
os.write(1, b'__STEPS_DONE__\\r\\n')
sys.stdin.readline()
"""


class OuterAttentionMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment(
            config_text='require("lemma").setup({ ui = { outer_progress = true } })\n'
        )
        self.addCleanup(self.server.close)
        self.emitted: dict[str, int] = {}

    def emit(
        self, session: Session, pane: str, data: bytes, *, hex_text: str = ""
    ) -> str:
        """Makes the Pane's emitter write data; returns the marker that follows it."""
        index = self.emitted.get(pane, 0)
        self.emitted[pane] = index + 1
        hex_text = hex_text or data.hex()
        text = ("--text", hex_text) if hex_text else ()
        self.server.require_command(
            "send", "--session", session.name, "--pane", pane, *text, "--key", "enter"
        )
        return f"__EMITTED_{index}__"

    def emit_visible(
        self, session: Session, pane: str, data: bytes, *, hex_text: str = ""
    ) -> None:
        # The marker follows the sequence in the same write, so the frame that shows it has
        # already processed the attention.
        marker = self.emit(session, pane, data, hex_text=hex_text)
        session.require_client().expect_output(marker)

    def start(self, name: str) -> Session:
        session = self.server.create_session(
            name, command=(sys.executable, "-c", EMITTER)
        )
        session.require_client().expect_output("__EMIT_READY__")
        return session

    def new_tab(self, session: Session, title: str, *command: str) -> str:
        document = json.loads(
            self.server.require_command(
                "proc",
                "tab",
                "new",
                "--session",
                session.name,
                "--title",
                title,
                "--focus",
                "preserve",
                "--",
                *(command or (sys.executable, "-c", EMITTER)),
            ).output
        )
        return document["results"][0]["result"]["pane"]

    def reload(self, ui: str) -> None:
        config = Path(self.server.environment["XDG_CONFIG_HOME"]) / "lemma/init.lua"
        config.write_text(f'require("lemma").setup({{ ui = {{ {ui} }} }})\n')
        self.server.require_command("config", "reload")

    @staticmethod
    def expect_notification(
        client: Client, title: bytes, body: bytes, *, timeout: float = 5.0
    ) -> None:
        # The title starts with the Session and Tab labels; the process-name Tab label varies.
        pattern = re.compile(
            re.escape(b"\x1b]777;notify;" + title)
            + rb"[^;\x1b]*;"
            + re.escape(body + b"\x1b\\")
        )
        wait_until(
            f"outer notification {body!r}",
            lambda: (
                True
                if client.drain(0.01) >= 0
                and pattern.search(client.process.output_tail) is not None
                else None
            ),
            timeout=timeout,
            diagnostics=client.diagnostics,
        )

    @staticmethod
    def latest_after(client: Client, later: bytes, earlier: bytes) -> bool | None:
        client.drain(0.01)
        tail = client.process.output_tail
        return True if tail.rfind(later) > tail.rfind(earlier) else None

    def test_notifications_from_any_pane_are_labelled_sanitized_and_coalesced(
        self,
    ) -> None:
        session = self.start("notify")
        client = session.require_client()
        background = self.new_tab(session, "jobs;x")
        time.sleep(0.2)

        # A background Tab's notification reaches the outer terminal immediately, titled with its
        # Session and Tab; the separator in the Tab name cannot split the OSC 777 fields.
        # It also rings the bell, which terminals without OSC 777 still present.
        self.emit(session, background, b"\x1b]777;notify;Build;done\x1b\\")
        client.expect_raw(b"\x1b]777;notify;notify: jobs x - Build;done\x1b\\\x07")
        self.emit(session, background, b"\x1b]9;plain body\x07")
        client.expect_raw(b"\x1b]777;notify;notify: jobs x;plain body\x1b\\")

        # Notifications before forwarding coalesce into the Pane's latest. The body is bounded,
        # and control characters never reach the outer terminal.
        body = b"\xc2\x9d" + b"x" * 2000
        burst = b"".join(b"\x1b]9;first %d\x07" % index for index in range(3))
        self.emit(session, background, burst + b"\x1b]9;" + body + b"\x07")
        client.expect_raw(b"notify: jobs x;??" + b"x" * 1022 + b"\x1b\\")
        client.drain(0.2)
        self.assertNotIn(b"first 0", client.process.output_tail)
        self.assertNotIn(b"\xc2\x9d", client.process.output_tail)

        # Without notification forwarding, a notification only rings the outer bell.
        self.reload("outer_notifications = false")
        client.drain(0.2)
        bells = client.process.output_tail.count(b"\x07")
        self.emit(session, background, b"\x1b]9;quiet\x07")
        wait_until(
            "a notification to ring the bell",
            lambda: (
                True
                if client.drain(0.01) >= 0
                and client.process.output_tail.count(b"\x07") > bells
                else None
            ),
            diagnostics=client.diagnostics,
        )
        self.assertNotIn(b"quiet", client.process.output_tail)

        # Without bell forwarding, neither BEL nor a notification rings.
        self.reload("outer_bell = false")
        client.drain(0.2)
        bells = client.process.output_tail.count(b"\x07")
        self.emit(session, background, b"\x07\x1b]9;silent\x07")
        client.expect_raw(b"\x1b]777;notify;notify: jobs x;silent\x1b\\")
        client.drain(0.3)
        self.assertEqual(client.process.output_tail.count(b"\x07"), bells)

    def test_periodic_notifiers_share_the_notification_budget(self) -> None:
        session = self.start("share")
        client = session.require_client()
        start = time.time() + 1.0
        for name in ("a", "b"):
            self.new_tab(
                session, name, sys.executable, "-c", PERIODIC_NOTIFIER, str(start), name
            )

        # B's progress redraws its status marker continually, so forwarded notifications are
        # recorded as they appear rather than read from the bounded output tail at the end.
        latest = {b"a": -1, b"b": -1}

        def forwarded() -> bool | None:
            client.drain(0.01)
            for name, index in re.findall(
                rb"\x1b\]777;notify;share: ([ab]);[ab] (\d+)\x1b\\",
                client.process.output_tail,
            ):
                latest[name] = max(latest[name], int(index))
            return True if latest[b"a"] >= 2 and latest[b"b"] >= 1 else None

        # After the burst (a 0, a 1, b 0), the two refills at five and ten seconds go to each Pane
        # once: neither B's newer progress nor A's newer notifications move the other back in line.
        wait_until(
            "both periodic notifiers to be forwarded after the burst",
            forwarded,
            timeout=13.0,
            diagnostics=lambda: f"forwarded={latest}\n{client.diagnostics()}",
        )

    def test_notifications_and_bells_are_rate_limited(self) -> None:
        session = self.start("limits")
        client = session.require_client()
        pane = session.state().focused_pane
        for index in range(3):
            self.emit_visible(session, pane, b"\x1b]9;note %d\x07" % index)
            self.expect_notification(client, b"limits: ", b"note %d" % index)
        # The burst is spent: the fourth waits for a refill instead of being dropped.
        self.emit_visible(session, pane, b"\x1b]9;note 3\x07")
        self.assertNotIn(b";note 3\x1b\\", client.process.output_tail)
        self.expect_notification(client, b"limits: ", b"note 3", timeout=8.0)

        # Twenty separate bells from a background Tab within 200 ms forward a burst of four,
        # then one coalesced bell after the next refill.
        bells = self.new_tab(session, "bells", sys.executable, "-c", BELL_FLOOD)
        time.sleep(0.2)
        client.drain(0.1)
        before = client.process.output_tail.count(b"\x07")
        self.emit(session, bells, b"")
        client.drain(0.8)
        rung = client.process.output_tail.count(b"\x07") - before
        self.assertGreaterEqual(rung, 2, client.diagnostics())
        self.assertLessEqual(rung, 5, client.diagnostics())

    def test_attention_is_not_replayed_across_session_switch(self) -> None:
        early = b"\x1b]9;before attach\x07\x07"
        script = f"import os\nos.write(1, {early!r})\n" + EMITTER
        target = self.server.create_session(
            "target", attach=False, command=(sys.executable, "-c", script)
        )
        time.sleep(0.5)
        source = self.start("source")
        client = source.require_client()
        client.drain(0.1)
        bells = client.process.output_tail.count(b"\x07")
        client.prefix(":")
        client.send("switch target\r")
        self.server.wait_for_state(
            "target", lambda state: state.attached, "attachment to switch"
        )
        client.expect_output("__EMIT_READY__")
        client.drain(0.3)
        self.assertNotIn(b"before attach", client.process.output_tail)
        self.assertEqual(client.process.output_tail.count(b"\x07"), bells)
        target.client = client
        self.emit_visible(target, target.state().focused_pane, b"\x1b]9;switched\x07")
        self.expect_notification(client, b"target: ", b"switched")

    def test_progress_follows_the_focused_pane_and_is_removed(self) -> None:
        session = self.start("progress")
        client = session.require_client()
        left = session.state().focused_pane
        right = self.server.require_command(
            "split",
            "--session",
            session.name,
            "--pane",
            left,
            "--right",
            "--",
            sys.executable,
            "-c",
            EMITTER,
        ).output.strip()
        client.expect_output("__EMIT_READY__")
        # Progress from an unfocused Pane is not forwarded until that Pane is focused.
        self.emit_visible(session, left, b"\x1b]9;4;1;40\x1b\\")
        self.assertNotIn(b"\x1b]9;4;1;40", client.process.output_tail)
        self.server.require_command("focus", "--session", session.name, "--pane", left)
        client.expect_raw(b"\x1b]9;4;1;40\x1b\\")
        self.emit(session, left, b"\x1b]9;4;2\x1b\\")
        client.expect_raw(b"\x1b]9;4;2\x1b\\")

        # Focusing a Pane without progress removes the indicator; returning restores it.
        self.server.require_command("focus", "--session", session.name, "--pane", right)
        client.expect_raw(PROGRESS_REMOVED)
        self.server.require_command("focus", "--session", session.name, "--pane", left)
        wait_until(
            "progress to follow focus back",
            lambda: self.latest_after(client, b"\x1b]9;4;2\x1b\\", PROGRESS_REMOVED),
            diagnostics=client.diagnostics,
        )

        # Disabling it by reload removes the indicator; enabling presents it again.
        self.reload("outer_progress = false")
        wait_until(
            "progress removal after reload",
            lambda: self.latest_after(client, PROGRESS_REMOVED, b"\x1b]9;4;2\x1b\\"),
            diagnostics=client.diagnostics,
        )
        self.reload("outer_progress = true")
        wait_until(
            "progress to return after reload",
            lambda: self.latest_after(client, b"\x1b]9;4;2\x1b\\", PROGRESS_REMOVED),
            diagnostics=client.diagnostics,
        )

        # A requested detach removes the indicator before the client restores the terminal.
        session.detach()
        tail = client.process.output_tail
        self.assertGreater(
            tail.rfind(PROGRESS_REMOVED), tail.rfind(b"\x1b]9;4;2\x1b\\"), tail[-512:]
        )

    def test_progress_is_coalesced_at_a_bounded_rate(self) -> None:
        session = self.start("steps")
        client = session.require_client()
        pane = self.new_tab(session, "steps", sys.executable, "-c", PROGRESS_STEPS)
        client.prefix("n")
        self.server.wait_for_state(
            session.name,
            lambda state: state.focused_pane == pane,
            "stepping Tab to be focused",
        )
        client.drain(0.2)
        before = client.process.output_tail.count(b"\x1b]9;4;1;")
        self.emit(session, pane, b"")
        client.expect_output("__STEPS_DONE__")
        # Forty changes within about 400 ms present a few intermediate values and the latest.
        client.expect_raw(b"\x1b]9;4;1;40\x1b\\")
        client.drain(0.3)
        presented = client.process.output_tail.count(b"\x1b]9;4;1;") - before
        self.assertLessEqual(presented, 5, client.diagnostics())

    def test_ended_process_and_session_remove_progress(self) -> None:
        # A held Pane whose process exited no longer reports progress.
        script = (
            "import os, sys\n"
            "os.write(1, b'\\x1b]9;4;1;30\\x1b\\\\__HELD_READY__\\r\\n')\n"
            "sys.stdin.readline()\n"
        )
        held = self.server.create_session(
            "held", hold=True, command=(sys.executable, "-c", script)
        )
        client = held.require_client()
        client.expect_raw(b"\x1b]9;4;1;30\x1b\\")
        self.emit(held, held.state().focused_pane, b"")
        wait_until(
            "progress removal after the held process exits",
            lambda: self.latest_after(client, PROGRESS_REMOVED, b"\x1b]9;4;1;30\x1b\\"),
            diagnostics=client.diagnostics,
        )
        held.destroy()
        with contextlib.suppress(RuntimeError):
            client.wait_for_exit()  # The client exits unsuccessfully when its Session ends.

        # A Session that ends while attached removes presented progress before its client exits.
        session = self.start("ending")
        client = session.require_client()
        pane = session.state().focused_pane
        self.emit(session, pane, b"\x1b]9;4;3\x1b\\")
        client.expect_raw(b"\x1b]9;4;3\x1b\\")
        self.server.require_command(
            "send", "--session", session.name, "--pane", pane, "--key", "ctrl+d"
        )
        with contextlib.suppress(RuntimeError):
            client.wait_for_exit()  # The client exits unsuccessfully when its Session ends.
        tail = client.process.output_tail
        self.assertGreater(
            tail.rfind(PROGRESS_REMOVED), tail.rfind(b"\x1b]9;4;3\x1b\\"), tail[-512:]
        )

    def test_pane_hyperlinks_reach_the_outer_terminal_scoped_per_pane(self) -> None:
        session = self.start("links")
        client = session.require_client()
        left = session.state().focused_pane
        right = self.server.require_command(
            "split",
            "--session",
            session.name,
            "--pane",
            left,
            "--right",
            "--",
            sys.executable,
            "-c",
            EMITTER,
        ).output.strip()

        def link(uri: bytes, text: bytes) -> bytes:
            return b"\x1b]8;;" + uri + b"\x1b\\" + text + b"\x1b]8;;\x1b\\"

        def expect_link(uri: bytes, text: bytes) -> bytes:
            # The link opens with a Pane-scoped ID, may restyle, and closes after its text.
            pattern = re.compile(
                rb"\x1b\]8;id=(lemma-\d+);"
                + re.escape(uri)
                + rb"\x1b\\(?:\x1b\[[0-9;:]*m)*"
                + re.escape(text)
                + rb"\x1b\]8;;\x1b\\"
            )

            def observe() -> bytes | None:
                client.drain(0.01)
                match = pattern.search(client.process.output_tail)
                return match.group(1) if match else None

            return wait_until(
                f"outer link {text!r}", observe, diagnostics=client.diagnostics
            )

        # The same URI in two Panes gets distinct IDs, so the outer terminal never joins them.
        self.emit(session, left, link(b"https://example.com/a?b=c;d", b"left-docs"))
        left_id = expect_link(b"https://example.com/a?b=c;d", b"left-docs")
        self.emit(session, right, link(b"https://example.com/a?b=c;d", b"right-docs"))
        right_id = expect_link(b"https://example.com/a?b=c;d", b"right-docs")
        self.assertNotEqual(left_id, right_id)

        # URIs that are not printable ASCII with a scheme are shown without their link.
        self.emit_visible(
            session,
            left,
            link(b"https://example.com/\xe2\x98\x83", b"snowman")
            + link(b"no-scheme", b"relative"),
        )
        client.expect_output("snowman")
        client.drain(0.2)
        self.assertNotIn(
            b"https://example.com/\xe2\x98\x83", client.process.output_tail
        )
        self.assertNotIn(b";no-scheme", client.process.output_tail)

        # Disabling forwarding repaints without links and forwards none afterwards.
        self.reload("outer_hyperlinks = false")
        self.emit_visible(session, left, link(b"https://example.com/off", b"unlinked"))
        client.drain(0.2)
        self.assertNotIn(b"https://example.com/off", client.process.output_tail)

    def test_client_cleanup_closes_hyperlinks_on_detach_and_signal(self) -> None:
        for interrupted in (False, True):
            with self.subTest(interrupted=interrupted):
                self.emitted.clear()
                session = self.start(f"link_cleanup_{interrupted}")
                client = session.require_client()
                self.emit_visible(
                    session,
                    session.state().focused_pane,
                    b"\x1b]8;;https://example.com/\x1b\\linked\x1b]8;;\x1b\\",
                )
                client.expect_raw(b";https://example.com/\x1b\\")
                if interrupted:
                    os.kill(client.pid, signal.SIGTERM)
                    with self.assertRaisesRegex(RuntimeError, "exited unsuccessfully"):
                        client.wait_for_exit()
                else:
                    session.detach()
                # A transport interruption can occur after an OSC 8 opener. CAN cancels only
                # partial escape sequences, so restoration must close an already active link too.
                cleanup = client.process.final_output.split(b"\x18")[-1]
                self.assertIn(b"\x1b]8;;\x1b\\", cleanup)
                self.assertLess(
                    cleanup.index(b"\x1b]8;;\x1b\\"), cleanup.index(b"\x1b[?1049l")
                )
                self.assertTrue(client.process.terminal_state_restored)

    def test_focused_pane_directory_is_forwarded_and_inherited(self) -> None:
        session = self.start("cwd")
        client = session.require_client()
        pane = session.state().focused_pane

        def launch_cwd(pane_id: str) -> str:
            document = json.loads(
                self.server.require_command(
                    "proc",
                    "pane",
                    "inspect",
                    "--session",
                    session.name,
                    "--pane",
                    pane_id,
                ).output
            )
            state = document["results"][0]["result"]["pane_state"]
            return state["process"]["launch"]["cwd"]

        def split(*arguments: str) -> str:
            return self.server.require_command(
                "split", "--session", session.name, "--pane", pane, *arguments
            ).output.strip()

        def close(pane_id: str) -> None:
            self.server.require_command(
                "proc", "pane", "kill", "--session", session.name, "--pane", pane_id
            )

        # Without a report, a new Pane uses the launch default.
        created = split("--down")
        fallback = launch_cwd(created)
        close(created)

        project = self.server.root / "project dir"
        project.mkdir()
        host = socket.gethostname().encode()
        uri = b"file://" + host + str(project).replace(" ", "%20").encode()
        self.emit_visible(session, pane, b"\x1b]7;" + uri + b"\x1b\\")
        client.expect_raw(b"\x1b]7;" + uri + b"\x1b\\")

        # Splits and Tabs without an explicit directory start in the reported directory.
        created = split("--right")
        self.assertEqual(launch_cwd(created), str(project))
        close(created)
        self.assertEqual(
            launch_cwd(self.new_tab(session, "t", "/bin/sh")), str(project)
        )
        created = split("--down", "--cwd", "/")
        self.assertEqual(launch_cwd(created), "/")
        close(created)

        # Remote and stale reports are forwarded as reported but not inherited.
        for report in (
            b"file://elsewhere.invalid/tmp",
            b"file://" + host + str(project / "missing").encode(),
        ):
            self.emit_visible(session, pane, b"\x1b]7;" + report + b"\x1b\\")
            client.expect_raw(b"\x1b]7;" + report + b"\x1b\\")
            created = split("--down")
            self.assertEqual(launch_cwd(created), fallback)
            close(created)

        # A directory the daemon's user cannot enter is not inherited, so the Pane still starts.
        if os.geteuid() != 0:
            locked = self.server.root / "locked"
            locked.mkdir(mode=0o600)
            self.addCleanup(locked.chmod, 0o700)
            report = b"file://" + host + str(locked).encode()
            self.emit_visible(session, pane, b"\x1b]7;" + report + b"\x1b\\")
            created = split("--down")
            self.assertEqual(launch_cwd(created), fallback)
            time.sleep(0.3)
            self.assertTrue(
                process_exists(session.state().pane(created).pid),
                self.server.diagnostics(session.name),
            )
            close(created)

        # A report too long to forward whole is not truncated into another path.
        long_report = " ".join((b"\x1b]7;file:///".hex(), "61*2100", b"\x1b\\".hex()))
        self.emit_visible(session, pane, b"", hex_text=long_report)
        client.drain(0.2)
        self.assertNotIn(b"a" * 2100, client.process.output_tail)

        # Disabling forwarding leaves the outer directory alone.
        self.reload("outer_cwd = false")
        self.emit_visible(session, pane, b"\x1b]7;file:///srv\x1b\\")
        client.drain(0.2)
        self.assertNotIn(b"\x1b]7;file:///srv", client.process.output_tail)
