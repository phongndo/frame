from __future__ import annotations

import base64
import fcntl
import json
import os
import re
import select
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
import zlib
from pathlib import Path

from tests.support.mux_harness import Client, LemmaServer, Session, wait_until


class TerminalBoundaryMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)

    def test_late_fragmented_host_theme_replies_never_become_keys_or_modify_paste(
        self,
    ) -> None:
        captured = self.server.root / "host-reply-input.bin"
        script = f"""
import os, time, tty
from pathlib import Path
tty.setraw(0)
os.write(1, b'\\x1b[?2004hHOST_REPLY_READY')
data = b''
while not data.endswith(b'Z'):
    data += os.read(0, 4096)
Path({str(captured)!r}).write_bytes(data)
time.sleep(60)
"""
        session = self.server.create_session(
            "host_replies", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("HOST_REPLY_READY")
        time.sleep(0.25)  # The initial 100 ms host-theme query window has expired.
        client.send(b"\x1b]10;rgb:ffff/")
        time.sleep(
            0.15
        )  # A recognized report is not subject to Escape-key disambiguation.
        client.send(b"ffff/ffff\x1b\\\x1b]11;rgb:0000/0000/0000\x1b\\")
        paste = b"\x1b[200~\x1b]4;1;rgb:ffff/0000/0000\x1b\\\x1b[201~"
        client.send(paste + b"Z")
        wait_until(
            "late host-reply input recorded",
            lambda: True if captured.exists() else None,
            diagnostics=client.diagnostics,
        )
        # Ghostty's native paste encoder replaces embedded ESC with spaces. The host-reply
        # filter must preserve the pasted content for that encoder, not consume it as metadata.
        expected = b"\x1b[200~ ]4;1;rgb:ffff/0000/0000 \\\x1b[201~Z"
        self.assertEqual(captured.read_bytes(), expected)

    def test_short_pty_read_does_not_replay_the_previous_read_tail(self) -> None:
        release = self.server.root / "short-read.gate"
        finish = self.server.root / "short-read-finish.gate"
        script = (
            "printf '%16000s' x; "
            "printf '\\033[2J\\033[H__LONG_READ_READY__'; "
            f"while [ ! -e {shlex.quote(str(release))} ]; do sleep 0.01; done; "
            "printf '\\033[2J\\033[H__SHORT_READ__'; "
            f"while [ ! -e {shlex.quote(str(finish))} ]; do sleep 0.01; done"
        )
        session = self.server.create_session(
            "short_pty_read", command=("/bin/sh", "-c", script)
        )
        client = session.require_client()
        client.expect_output("__LONG_READ_READY__")

        release.touch()
        client.expect_output("__SHORT_READ__")
        client.drain()
        screen = client.screen_text()
        self.assertIn("__SHORT_READ__", screen)
        self.assertNotIn("__LONG_READ_READY__", screen)
        session.pane().expect_alive()
        finish.touch()

    def test_application_cursor_mode_changes_bytes_delivered_to_child(self) -> None:
        session = self.server.create_session("application_cursor")
        pane = session.pane()
        pane.send(
            "stty -echo -icanon min 1 time 0; r='__APP_CURSOR_'; "
            "printf '\\033[?1h%s\\n' \"${r}READY__\"; "
            "code=$(dd bs=1 count=3 2>/dev/null | od -An -tx1 | tr -d ' \\n'); "
            "printf '\\033[?1l'; stty sane; "
            'printf \'%s%s__\\n\' "$r" "$code"\r'
        )
        pane.expect_output("__APP_CURSOR_READY__")

        # One typed outer Up press. Lemma must query Ghostty's canonical DECCKM state and encode SS3.
        session.require_client().send(b"\x1b[1;1:1A")

        pane.expect_output("__APP_CURSOR_1b4f41__")

    def test_default_modes_replace_status_without_pane_overlays(self) -> None:
        session = self.server.create_session("compiled_copy_policy")
        client = session.require_client()
        pane = session.pane()
        pane.send("printf '__COPY_POLICY_LINE__\\n'\r")
        pane.expect_output("__COPY_POLICY_LINE__")

        def status_row() -> str:
            client.drain()
            return client.screen_text().splitlines()[0]

        def status_starts_with(prefix: str) -> bool | None:
            return True if status_row().startswith(prefix) else None

        client.prefix("[")
        wait_until("copy mode status row", lambda: status_starts_with("COPY [0/0]"))
        self.assertNotIn("compiled_copy_policy", status_row())

        client.prefix("/")
        client.send(b"ls")
        wait_until("copy search status prompt", lambda: status_starts_with("/ls"))
        self.assertNotIn("SEARCH", status_row())
        client.send(b"\x1b")
        wait_until(
            "copy mode after search cancel", lambda: status_starts_with("COPY [0/0]")
        )
        client.send(b"\x07")
        wait_until(
            "normal status after leaving copy mode",
            lambda: True if not status_row().startswith("COPY") else None,
        )

        client.prefix("m")
        wait_until("resize mode status row", lambda: status_starts_with("RESIZE"))
        self.assertNotIn("compiled_copy_policy", status_row())
        client.send(b"q")
        wait_until(
            "normal status after leaving resize mode",
            lambda: True if not status_row().startswith("RESIZE") else None,
        )
        pane.expect_alive()

    def test_escape_then_plain_key_leaves_copy_and_forwards_the_key(self) -> None:
        session = self.server.create_session("copy_escape_plain_key")
        client = session.require_client()
        pane = session.pane()
        pane.send(
            "stty -echo -icanon min 1 time 0; printf '__COPY_ESCAPE_READY__\\n'; "
            "code=$(dd bs=1 count=1 2>/dev/null | od -An -tx1 | tr -d ' \\n'); "
            "stty sane; printf '__COPY_ESCAPE_%s__\\n' \"$code\"\r"
        )
        pane.expect_output("__COPY_ESCAPE_READY__")

        client.prefix("[")
        client.expect_output("COPY")
        client.send(b"\x1bx")

        pane.expect_output("__COPY_ESCAPE_78__")
        pane.expect_alive()

    def test_default_rename_editing_uses_the_compiled_policy(self) -> None:
        session = self.server.create_session("compiled_rename_policy")
        client = session.require_client()

        client.prefix("$")
        client.send(b"\x15renamed-policy\r")
        client.expect_output("renamed-policy")
        session.pane().expect_alive()

    def test_styled_output_is_confined_to_its_composed_pane(self) -> None:
        session = self.server.create_session("styled_output")
        left = session.pane()
        left.send("printf '\\033[2J\\033[H__LEFT_NEIGHBOR__\\n'\r")
        left.expect_output("__LEFT_NEIGHBOR__")
        right = left.split_right()

        right.send("printf '\\033[1;31m__RIGHT_STYLED__\\033[0m\\n'\r")
        right.expect_output("__RIGHT_STYLED__")
        session.require_client().expect_output("__LEFT_NEIGHBOR__")
        session.require_client().expect_raw("38;5;1")
        left.expect_alive()
        right.expect_alive()

    def test_output_while_unpresented_is_current_when_presented_again(self) -> None:
        # Unpresented panes release their render rows; presenting them again must rebuild the
        # complete pane, including content drawn before and output parsed while hidden.
        session = self.server.create_session("unpresented_output")
        pane = session.pane()
        hidden_gate = self.server.root / "unpresented-tab.gate"
        hidden_done = self.server.root / "unpresented-tab.done"
        pane.send(
            "r='__UNPRESENTED_'; "
            'printf "\\033[1;32m${r}READY__\\033[0m\\n"; '
            f"while [ ! -e {shlex.quote(str(hidden_gate))} ]; do sleep 0.01; done; "
            'printf "\\033[1;35m${r}TAB__\\033[0m\\n"; '
            f": > {shlex.quote(str(hidden_done))}\r"
        )
        pane.expect_output("__UNPRESENTED_READY__")
        client = session.require_client()

        client.prefix("c")
        second = self.server.wait_for_state(
            session.name,
            lambda state: state.tabs == 2 and state.focused_pane != pane.id,
            "a second tab to become active",
        )
        client.send("s='__SECOND_'; printf \"${s}TAB__\\n\"\r")
        client.expect_output("__SECOND_TAB__")
        self.assertNotIn("__UNPRESENTED_READY__", client.screen_text())
        hidden_gate.touch()
        wait_until(
            "the hidden pane to emit output",
            lambda: True if hidden_done.exists() else None,
        )

        client.prefix("p")
        self.server.wait_for_state(
            session.name,
            lambda state: state.active_tab != second.active_tab,
            "the first tab to become active again",
        )
        client.expect_output("__UNPRESENTED_TAB__")
        client.expect_raw("38;5;5")
        self.assertIn("__UNPRESENTED_READY__", client.screen_text())
        self.assertNotIn("__SECOND_TAB__", client.screen_text())

        # Zoom hides a sibling within the active tab; unzooming presents it again.
        sibling = pane.split_right()
        sibling.send("z='__ZOOM_'; printf \"${z}SIBLING__\\n\"\r")
        sibling.expect_output("__ZOOM_SIBLING__")
        pane.focus()
        client.prefix("z")

        def sibling_hidden() -> bool | None:
            client.drain(0.01)
            return True if "__ZOOM_SIBLING__" not in client.screen_text() else None

        wait_until("the zoomed pane to hide its sibling", sibling_hidden)
        client.prefix("z")
        client.expect_output("__ZOOM_SIBLING__")
        self.assertIn("__UNPRESENTED_TAB__", client.screen_text())
        pane.expect_alive()
        sibling.expect_alive()

    def test_synchronized_output_holds_one_pane_while_sibling_progresses(self) -> None:
        session = self.server.create_session("synchronized_output")
        left = session.pane()
        right = left.split_right()
        start_gate = self.server.root / "synchronized-output-start.gate"
        started = self.server.root / "synchronized-output-started"
        release_gate = self.server.root / "synchronized-output-release.gate"
        left.send(
            "r='__SYNC_'; h='__HELD_'; x='__RELEASE_'; "
            'printf "${r}READY__\\n"; '
            f"while [ ! -e {shlex.quote(str(start_gate))} ]; do sleep 0.01; done; "
            f"printf '\\033[?2026h%s' \"${{h}}A__\"; : > {shlex.quote(str(started))}; "
            f"while [ ! -e {shlex.quote(str(release_gate))} ]; do sleep 0.01; done; "
            "printf '%s\\033[?2026l\\n' \"${x}A__\"\r"
        )
        left.expect_output("__SYNC_READY__")
        right.focus()

        start_gate.touch()
        wait_until(
            "pane A to enter synchronized output",
            lambda: True if started.exists() else None,
        )
        session.require_client().send("v='__LIVE_'; printf \"${v}B__\\n\"\r")
        session.require_client().expect_output("__LIVE_B__")
        self.assertNotIn("__HELD_A__", session.require_client().screen_text())

        release_gate.touch()
        session.require_client().expect_output("__HELD_A__")
        session.require_client().expect_output("__RELEASE_A__")
        left.expect_alive()
        right.expect_alive()


class GraphicsMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)

    def test_png_is_reprojected_after_resize_and_reattach(self) -> None:
        png = b"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="
        packet = b"\x1b_Ga=T,q=2,C=1,f=100,i=1,c=4,r=2;" + png + b"\x1b\\"
        script = f"import os,time; os.write(1, {packet!r}); os.write(1,b'IMAGE_READY'); time.sleep(60)"
        session = self.server.create_session(
            "native_png", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_raw(b"\x1b_Ga=t,q=2,f=32,s=1,v=1,i=")
        client.expect_raw(b"\x1b_Ga=p,q=2,C=1,i=")
        client.expect_output("IMAGE_READY")
        old = client.process.output_tail.count(b"\x1b_Ga=p,q=2,C=1,i=")
        client.resize(100, 30)

        def redrawn() -> bool | None:
            client.drain()
            return (
                True
                if client.process.output_tail.count(b"\x1b_Ga=p,q=2,C=1,i=") > old
                else None
            )

        wait_until("resized image placement", redrawn)
        session.detach()
        fresh = session.attach(columns=100, rows=30)
        fresh.expect_raw(b"\x1b_Ga=t,q=2,f=32,s=1,v=1,i=")
        fresh.expect_raw(b"\x1b_Ga=p,q=2,C=1,i=")
        fresh.expect_output("IMAGE_READY")
        session.pane().expect_alive()

    def test_cell_pixel_resize_updates_pty_and_graphics_without_grid_resize(
        self,
    ) -> None:
        report = self.server.root / "pixel-size.json"
        script = f"""
import fcntl, json, os, signal, struct, termios, time
from pathlib import Path
path = Path({str(report)!r})
def resized(*_):
    size = struct.unpack('HHHH', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\\0' * 8))
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(size))
    temporary.replace(path)
signal.signal(signal.SIGWINCH, resized)
resized()
os.write(1, b'\\x1b_Ga=T,q=2,C=1,i=1,s=1,v=1,f=32,c=15;/wAA/w==\\x1b\\\\PIXEL_READY')
while True: time.sleep(1)
"""
        session = self.server.create_session(
            "pixel_resize", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("PIXEL_READY")
        client.expect_raw(b"\x1b_Ga=t,q=2,f=32,s=120,v=120,")
        self.assertEqual(json.loads(report.read_text()), [23, 80, 640, 368])
        fcntl.ioctl(
            client.process.descriptor,
            termios.TIOCSWINSZ,
            struct.pack("HHHH", 24, 80, 960, 576),
        )
        os.killpg(client.pid, signal.SIGWINCH)
        # Drain image output while waiting for resize so the client can handle SIGWINCH even
        # when the outer PTY fills. Observe the new upload before its header leaves the raw tail.
        client.expect_raw(b"\x1b_Ga=t,q=2,f=32,s=180,v=180,")
        wait_until(
            "cell-only resize reaches Pane PTY",
            lambda: (
                True if json.loads(report.read_text()) == [23, 80, 960, 552] else None
            ),
            diagnostics=lambda: report.read_text() + "\n" + client.diagnostics(),
        )
        state = session.state()
        self.assertEqual((state.columns, state.rows), (80, 24))
        session.pane().expect_alive()

    def test_animation_advances_without_more_pty_output(self) -> None:
        packets = (
            b"\x1b_Ga=T,q=2,C=1,i=1,s=1,v=1,f=32;/wAA/w==\x1b\\"
            b"\x1b_Ga=f,q=2,i=1,s=1,v=1,f=32,z=100;AAD//w==\x1b\\"
            b"\x1b_Ga=a,q=2,i=1,r=1,z=100,s=3\x1b\\"
        )
        script = f"import os,time; os.write(1, {packets!r}); os.write(1,b'ANIMATION_READY'); time.sleep(60)"
        session = self.server.create_session(
            "native_animation", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_raw(b";/wAA/w==\x1b\\")
        client.expect_raw(b";AAD//w==\x1b\\")
        client.expect_output("ANIMATION_READY")
        session.pane().expect_alive()


class ClipboardMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        cache = tempfile.TemporaryDirectory(prefix="lemma clipboard's-")
        self.addCleanup(cache.cleanup)
        self.server = LemmaServer.from_environment(
            config_text='require("lemma").setup({terminal={clipboard_read=true,clipboard_write=true}})',
            environment={"XDG_CACHE_HOME": cache.name},
        )
        self.addCleanup(self.server.close)

    def test_osc52_large_text_preserves_native_protocol_and_reply_order(self) -> None:
        write_gate = self.server.root / "osc52-write.gate"
        read_gate = self.server.root / "osc52-read.gate"
        captured = self.server.root / "osc52-input"
        script = f"""
import base64, os, time, tty
from pathlib import Path
tty.setraw(0)
os.write(1, b'OSC52_READY')
while not Path({str(write_gate)!r}).exists(): time.sleep(0.005)
os.write(1, b'\\x1b]52;c;' + base64.b64encode(b'x' * 200003) + b'\\x1b\\\\OSC52_WRITTEN')
while not Path({str(read_gate)!r}).exists(): time.sleep(0.005)
os.write(1, b'\\x1b]52;c;?\\x1b\\\\')
data = bytearray()
while not data.endswith(b'Z'): data.extend(os.read(0, 8192))
Path({str(captured)!r}).write_bytes(data)
os.write(1, b'OSC52_CAPTURED')
while True: time.sleep(1)
"""
        session = self.server.create_session(
            "osc52", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("OSC52_READY")
        client.send(
            b"\x1b[?5522;0$y"
        )  # Like stable Ghostty: MIME protocol is unsupported.
        write_gate.touch()
        # An OSC 52 body must not contain interleaved frames, even across publication chunks.
        client.expect_raw(b"\x1b]52;c;" + base64.b64encode(b"x" * 200003) + b"\x1b\\")
        client.expect_output("OSC52_WRITTEN")
        read_gate.touch()
        client.expect_raw(b"\x1b]52;c;?\x1b\\")
        encoded = base64.b64encode(b"y" * (1024 * 1024))
        client.send(b"\x1b]52;c;" + encoded[:12000])
        client.drain(0.15)
        client.send(encoded[12000:] + b"\x1b")
        client.drain(0.15)
        client.send(b"\\Z")
        client.expect_output("OSC52_CAPTURED")
        self.assertEqual(captured.read_bytes(), b"\x1b]52;c;" + encoded + b"\x1b\\Z")
        self.assertTrue(session.state().attached)

    def test_abandoned_osc52_read_cannot_deliver_late_data_to_a_new_owner(self) -> None:
        old_reply = self.server.root / "osc52-old"
        old_gate = self.server.root / "osc52-old.gate"
        new_reply = self.server.root / "osc52-new"
        old_script = f"""
import os, time, tty
from pathlib import Path
tty.setraw(0)
os.write(1, b'OLD_READ_READY')
while not Path({str(old_gate)!r}).exists(): time.sleep(0.005)
os.write(1, b'\\x1b]52;c;?\\x1b\\\\')
data = b''
while not data.endswith(b'\\x1b\\\\'): data += os.read(0, 8192)
Path({str(old_reply)!r}).write_bytes(data)
while True: time.sleep(1)
"""
        session = self.server.create_session(
            "osc52_owner", command=(sys.executable, "-c", old_script)
        )
        client = session.require_client()
        client.expect_output("OLD_READ_READY")
        old_gate.touch()
        client.expect_raw(b"\x1b]52;c;?\x1b\\")
        right = session.pane().split_right()
        wait_until(
            "cancelled read",
            lambda: old_reply.read_bytes() if old_reply.exists() else None,
        )
        self.assertEqual(old_reply.read_bytes(), b"\x1b]52;c;\x1b\\")
        new_script = f"""
import os, time, tty
from pathlib import Path
tty.setraw(0)
os.write(1, b'RETIRED_READ_READY\\x1b]52;c;?\\x1b\\\\')
data = b''
while not data.endswith(b'Z'): data += os.read(0, 8192)
Path({str(new_reply)!r}).write_bytes(data)
os.write(1, b'RETIRED_READ_CAPTURED')
while True: time.sleep(1)
"""
        script_path = self.server.root / "osc52-new-owner.py"
        script_path.write_text(new_script)
        right.send("exec " + shlex.join((sys.executable, str(script_path))) + "\r")
        client.expect_output("RETIRED_READ_READY")
        client.send(
            b"\x1b]52;c;" + base64.b64encode(b"old owner's secret") + b"\x1b\\Z"
        )
        client.expect_output("RETIRED_READ_CAPTURED")
        self.assertEqual(new_reply.read_bytes(), b"\x1b]52;c;\x1b\\Z")
        self.assertEqual(client.process.output_tail.count(b"\x1b]52;c;?\x1b\\"), 1)

    def test_image_paste_rejects_an_outer_terminal_without_mime_clipboard_support(
        self,
    ) -> None:
        script = (
            "import os,time,tty; tty.setraw(0); os.write(1,b'CAP_READY'); "
            "os.read(0,1); os.write(1,b'CAP_OBSERVED'); time.sleep(60)"
        )
        session = self.server.create_session(
            "no_mime_clipboard", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("CAP_READY")
        client.send(b"\x1b[?5522;0$yZ")
        client.expect_output("CAP_OBSERVED")
        result = self.server.command(
            "paste-image",
            "--session",
            session.name,
            "--pane",
            session.pane().id,
            "--json",
        )
        self.assertNotEqual(result.status, 0, result.output)
        self.assertIn("clipboard_unavailable", result.output)
        self.assertNotIn(b"\x1b]5522;type=read", client.process.output_tail)

    def run_worker(self, image: bytes, mode: str) -> tuple[bytes, int]:
        script = """
import os, resource, sys
fd = int(sys.argv[1])
os.dup2(fd, 3, inheritable=True)
if fd != 3: os.close(fd)
if sys.argv[3] == 'limited': resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
os.execve(sys.argv[2], [sys.argv[2]], os.environ)
"""
        helper = self.server.server_path.with_name("lemma-clipboard-host")
        parent, child = socket.socketpair()
        with parent, child:
            parent.settimeout(15)
            process = subprocess.Popen(
                (sys.executable, "-c", script, str(child.fileno()), str(helper), mode),
                pass_fds=(child.fileno(),),
                env=self.server.environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                child.close()
                try:
                    parent.sendall(image)
                    if mode != "deadline":
                        parent.shutdown(socket.SHUT_WR)
                except BrokenPipeError:
                    pass  # An over-limit worker may reject input before the final chunk.
                output = bytearray()
                if mode == "disconnected":
                    parent.close()
                else:
                    while data := parent.recv(4096):
                        output.extend(data)
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(stdout, b"")
                self.assertEqual(stderr, b"")
                assert process.returncode is not None
                return bytes(output), process.returncode
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)

    def test_clipboard_worker_rejects_bad_input_and_cleans_failed_saves(self) -> None:
        png = base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="
        )
        cases = (
            (b"not a PNG", "invalid"),
            (b"x" * (1024 * 1024 + 1), "oversized"),
            (png, "limited"),
            (png, "disconnected"),
        )
        for image, mode in cases:
            with self.subTest(mode=mode):
                output, status = self.run_worker(image, mode)
                self.assertEqual(status, 1)
                self.assertEqual(output, b"")
                self.assertEqual(
                    list(
                        Path(self.server.environment["XDG_CACHE_HOME"]).rglob("image-*")
                    ),
                    [],
                )

    def test_clipboard_worker_deadline_does_not_publish_a_partial_result(self) -> None:
        output, status = self.run_worker(b"", "deadline")
        self.assertEqual(status, 1)
        self.assertEqual(output, b"")

    def start_read(self) -> tuple[Session, Client, Path, bytes]:
        gate = self.server.root / "clipboard.gate"
        result = self.server.root / "clipboard.reply"
        script = f"""
import os, select, time, tty
from pathlib import Path
tty.setraw(0)
os.write(1, b'CLIPBOARD_READY')
while not Path({str(gate)!r}).exists(): time.sleep(0.005)
os.write(1, b'\\x1b]5522;type=read:id=child;aW1hZ2UvcG5n\\x1b\\\\CLIPBOARD_PROGRESS')
reply = bytearray()
while True:
    if not select.select([0], [], [], 10)[0]: raise RuntimeError('clipboard reply timeout')
    reply.extend(os.read(0, 8192))
    if b':status=DONE' in reply or b':status=EPERM' in reply: break
Path({str(result)!r}).write_bytes(reply)
os.write(1, b'CLIPBOARD_FINISHED')
while True: time.sleep(1)
"""
        session = self.server.create_session(
            "clipboard", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("CLIPBOARD_READY")
        gate.touch()
        client.expect_raw(b"\x1b]5522;type=read:id=")
        client.expect_output("CLIPBOARD_PROGRESS")
        match = re.search(
            rb"\x1b]5522;type=read:id=([0-9]+):", client.process.output_tail
        )
        self.assertIsNotNone(match)
        assert match is not None
        return session, client, result, match.group(1)

    def test_image_read_progress_and_reply_use_separate_input_channels(self) -> None:
        session, client, result, correlation = self.start_read()
        image = b"\x89PNG\r\n\x1a\n\x00\xff\x80"
        client.send(
            b"\x1b]5522;type=read:status=OK:id="
            + correlation
            + b"\x1b\\"
            + b"\x1b]5522;type=read:status=DATA:id="
            + correlation
            + b":mime=aW1hZ2UvcG5n;"
            + base64.b64encode(image)
            + b"\x1b\\"
            + b"\x1b]5522;type=read:status=DONE:id="
            + correlation
            + b"\x1b\\"
        )
        wait_until(
            "clipboard completion",
            lambda: result.read_bytes() if result.exists() else None,
        )
        reply = result.read_bytes()
        self.assertIn(b"status=DONE:id=child", reply)
        self.assertIn(base64.b64encode(image), reply)
        self.assertNotIn(b":name=", reply)
        session.pane().expect_alive()

    def test_clipboard_reply_fragments_can_exceed_the_escape_key_timeout(self) -> None:
        session, client, result, correlation = self.start_read()
        prefix = b"\x1b]5522;type=read:id=" + correlation + b":status="
        client.send(prefix + b"OK\x1b\\" + prefix + b"DATA:mime=aW1hZ2UvcG5n;")
        client.drain(0.15)
        self.assertTrue(
            session.state().attached, "fragment header disconnected the client"
        )
        client.send(b"AP94\x1b")
        client.drain(0.15)
        self.assertTrue(
            session.state().attached, "fragmented ST disconnected the client"
        )
        client.send(b"\\" + prefix + b"DONE\x1b\\")
        client.expect_output("CLIPBOARD_FINISHED")
        self.assertIn(b"AP94", result.read_bytes())
        self.assertIn(b"status=DONE:id=child", result.read_bytes())
        self.assertTrue(session.state().attached)

    def test_incomplete_clipboard_record_has_a_nonrenewable_transport_deadline(
        self,
    ) -> None:
        session = self.server.create_session(
            "reply_deadline",
            command=(
                sys.executable,
                "-c",
                "import os,time,tty; tty.setraw(0); os.write(1,b'REPLY_READY'); time.sleep(60)",
            ),
        )
        client = session.require_client()
        client.expect_output("REPLY_READY")
        client.send(b"\x1b]5522;type=read:status=DATA:id=1;")
        client.drain(4)
        self.assertTrue(
            session.state().attached, "clipboard record used a keyboard deadline"
        )
        client.send(b"A")  # Progress must not renew the original 30-second deadline.
        wait_until(
            "incomplete clipboard record expires",
            lambda: True if not session.state().attached else None,
            timeout=28,
            diagnostics=client.diagnostics,
        )
        session.pane().expect_alive()

    def test_explicit_image_paste_creates_private_png_and_pastes_only_its_path(
        self,
    ) -> None:
        # Explicit user actions do not require granting applications clipboard access.
        (self.server.root / "config" / "lemma" / "init.lua").write_text(
            'require("lemma").setup({})'
        )
        reloaded = self.server.command("config", "reload")
        self.assertEqual(reloaded.status, 0, reloaded.output)
        result = self.server.root / "pasted-path"
        script = f"""
import os, time, tty
from pathlib import Path
tty.setraw(0)
os.write(1, b'\\x1b[?2004hPASTE_READY')
data = bytearray()
while not data.endswith(b'\\x1b[201~'): data.extend(os.read(0, 8192))
Path({str(result)!r}).write_bytes(data)
os.write(1, b'PASTE_FINISHED')
while True: time.sleep(1)
"""
        session = self.server.create_session(
            "clipboard_file", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("PASTE_READY")
        process = subprocess.Popen(
            [
                str(self.server.cli_path),
                str(self.server.socket_path),
                "paste-image",
                "--session",
                session.name,
                "--pane",
                session.pane().id,
                "--json",
            ],
            env=self.server.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.addCleanup(process.wait)
        self.addCleanup(process.terminate)
        client.expect_raw(b"\x1b]5522;type=read:id=")
        match = re.search(
            rb"\x1b]5522;type=read:id=([0-9]+):", client.process.output_tail
        )
        self.assertIsNotNone(match)
        assert match is not None
        correlation = match.group(1)

        def chunk(kind: bytes, data: bytes) -> bytes:
            return (
                struct.pack(">I", len(data))
                + kind
                + data
                + struct.pack(">I", zlib.crc32(kind + data))
            )

        png = (
            b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"\0\xff\0\0\xff"))
            + chunk(b"IEND", b"")
        )
        client.send(
            b"\x1b]5522;type=read:status=OK:id="
            + correlation
            + b"\x1b\\"
            + b"\x1b]5522;type=read:status=DATA:id="
            + correlation
            + b":mime=aW1hZ2UvcG5n;"
            + base64.b64encode(png)
            + b"\x1b\\"
            + b"\x1b]5522;type=read:status=DONE:id="
            + correlation
            + b"\x1b\\"
        )
        wait_until(
            "pasted PNG path", lambda: result.read_bytes() if result.exists() else None
        )
        output, error = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, (output, error))
        received = result.read_bytes()
        self.assertTrue(received.startswith(b"\x1b[200~"), received)
        self.assertTrue(received.endswith(b"\x1b[201~"), received)
        paths = shlex.split(received[6:-6].decode())
        self.assertEqual(len(paths), 1)
        path = Path(paths[0])
        self.assertTrue(
            path.is_relative_to(
                Path(self.server.environment["XDG_CACHE_HOME"]) / "lemma" / "clipboard"
            )
        )
        self.assertEqual(path.read_bytes(), png)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
        self.assertIn(str(path).encode(), output)

    def test_image_paste_keeps_session_identity_after_rename_and_name_reuse(
        self,
    ) -> None:
        def reader(path: Path) -> tuple[str, ...]:
            return (
                sys.executable,
                "-c",
                f"""
import os, time, tty
from pathlib import Path
tty.setraw(0)
os.write(1, b'PASTE_READY')
data = bytearray()
while not data.endswith(b'\\0'): data.extend(os.read(0, 8192))
Path({str(path)!r}).write_bytes(data[:-1])
os.write(1, b'PASTE_CAPTURED')
while True: time.sleep(1)
""",
            )

        original_path = self.server.root / "original-paste"
        replacement_path = self.server.root / "replacement-paste"
        original = self.server.create_session(
            "paste_owner", command=reader(original_path)
        )
        client = original.require_client()
        client.expect_output("PASTE_READY")
        session_id = original.state().id
        pane_id = original.pane().id
        request = {
            "schema": "lemma.proc/v1",
            "commands": [
                {
                    "command": "pane.paste-image",
                    "session": {"name": original.name},
                    "pane": {"id": pane_id},
                }
            ],
        }
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
            peer.settimeout(5)
            peer.connect(str(self.server.socket_path))
            peer.sendall(json.dumps(request).encode() + b"\n")
            client.expect_raw(b"\x1b]5522;type=read:id=")
            match = re.search(
                rb"\x1b]5522;type=read:id=([0-9]+):", client.process.output_tail
            )
            assert match is not None
            self.server.require_command(
                "proc", "session", "rename", "--session", session_id, "renamed_owner"
            )
            replacement = self.server.create_session(
                "paste_owner", command=reader(replacement_path)
            )
            replacement_client = replacement.require_client()
            replacement_client.expect_output("PASTE_READY")
            self.assertNotEqual(replacement.state().id, session_id)
            self.assertEqual(replacement.pane().id, pane_id)
            prefix = b"\x1b]5522;type=read:id=" + match.group(1) + b":status="
            png = b"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg=="
            client.send(
                prefix
                + b"OK\x1b\\"
                + prefix
                + b"DATA:mime=aW1hZ2UvcG5n;"
                + png
                + b"\x1b\\"
                + prefix
                + b"DONE\x1b\\"
            )
            with peer.makefile("rb") as response:
                completed = json.loads(response.readline())
            self.assertTrue(completed["ok"], completed)
        client.send(b"\0")
        replacement_client.send(b"\0")
        client.expect_output("PASTE_CAPTURED")
        replacement_client.expect_output("PASTE_CAPTURED")
        self.assertEqual(replacement_path.read_bytes(), b"")
        pasted = shlex.split(original_path.read_text())
        self.assertEqual(len(pasted), 1)
        self.assertEqual(Path(pasted[0]).read_bytes(), base64.b64decode(png))

    def test_large_image_write_interleaves_frames_and_orders_reply_before_input(
        self,
    ) -> None:
        gate = self.server.root / "write.gate"
        result = self.server.root / "write.reply"
        script = f"""
import base64, os, time, tty
from pathlib import Path
tty.setraw(0)
os.write(1, b'WRITE_READY')
while not Path({str(gate)!r}).exists(): time.sleep(0.005)
image = b'\\xa7' * 200003
os.write(1, b'\\x1b]5522;type=write:id=child-write\\x1b\\\\')
for offset in range(0, len(image), 3072):
    os.write(1, b'\\x1b]5522;type=wdata:mime=aW1hZ2UvcG5n;' + base64.b64encode(image[offset:offset+3072]) + b'\\x1b\\\\')
os.write(1, b'\\x1b]5522;type=wdata\\x1b\\\\WRITE_STAGED')
reply = bytearray()
while not reply.endswith(b'Z'): reply.extend(os.read(0, 8192))
Path({str(result)!r}).write_bytes(reply)
os.write(1, b'WRITE_FINISHED')
while True: time.sleep(1)
"""
        session = self.server.create_session(
            "clipboard_write", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("WRITE_READY")
        gate.touch()
        wire = bytearray()
        commit = b"\x1b]5522;type=wdata\x1b\\"
        deadline = time.monotonic() + 10
        while commit not in wire:
            self.assertLess(time.monotonic(), deadline, "clipboard write stalled")
            if not select.select([client.process.descriptor], [], [], 0.05)[0]:
                continue
            data = os.read(client.process.descriptor, 65536)
            self.assertTrue(data)
            wire.extend(data)
            client.process.screen.feed(data)
        packets = re.findall(
            rb"\x1b\]5522;type=wdata:mime=aW1hZ2UvcG5n;([A-Za-z0-9+/=]*)\x1b\\", wire
        )
        self.assertEqual(
            b"".join(base64.b64decode(packet, validate=True) for packet in packets),
            b"\xa7" * 200003,
        )
        self.assertIn(b"WRITE_STAGED", wire)
        self.assertLess(wire.index(b"WRITE_STAGED"), wire.index(commit))
        correlation = re.search(rb"\x1b\]5522;type=write:id=([0-9]+):", wire)
        self.assertIsNotNone(correlation)
        assert correlation is not None
        client.send(
            b"\x1b]5522;type=write:status=DONE:id=" + correlation.group(1) + b"\x1b\\Z"
        )
        client.expect_output("WRITE_FINISHED")
        self.assertEqual(
            result.read_bytes(),
            b"\x1b]5522;type=write:status=DONE:id=child-write\x1b\\Z",
        )

    def test_policy_reload_revokes_a_pending_application_read(self) -> None:
        session, client, result, _correlation = self.start_read()
        (self.server.root / "config" / "lemma" / "init.lua").write_text(
            'require("lemma").setup({})'
        )
        reloaded = self.server.command("config", "reload")
        self.assertEqual(reloaded.status, 0, reloaded.output)
        client.expect_output("CLIPBOARD_FINISHED")
        self.assertIn(b"status=EPERM:id=child", result.read_bytes())
        session.pane().expect_alive()

    def test_focus_change_revokes_a_pending_application_read(self) -> None:
        session, _client, result, _correlation = self.start_read()
        session.split()
        wait_until(
            "focus revocation", lambda: result.read_bytes() if result.exists() else None
        )
        self.assertIn(b"status=EPERM:id=child", result.read_bytes())

    def test_detach_revokes_the_request_without_stranding_the_application(self) -> None:
        _session, client, result, _correlation = self.start_read()
        client.prefix("d")
        client.wait_for_exit()
        wait_until(
            "clipboard cancellation",
            lambda: result.read_bytes() if result.exists() else None,
        )
        self.assertIn(b"status=EPERM:id=child", result.read_bytes())


if __name__ == "__main__":
    unittest.main()
