from __future__ import annotations

import json
import os
import signal
import sys
import time
import unittest

from tests.support.mux_harness import LemmaServer


class ResizeMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)

    def test_in_band_geometry_overrides_stale_proxy_size_without_typing_reports(
        self,
    ) -> None:
        report = self.server.root / "geometry.json"
        captured = self.server.root / "input.bin"
        script = f"""
import fcntl, json, os, struct, termios, tty
from pathlib import Path
report = Path({str(report)!r})
captured = Path({str(captured)!r})
def record_size():
    size = struct.unpack('HHHH', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\\0' * 8))
    temporary = report.with_suffix('.tmp')
    temporary.write_text(json.dumps(size))
    temporary.replace(report)
tty.setraw(0)
# Make the resize redraw exceed the PTY output buffer on Linux as well as macOS.
row = b'\\x1b[38;2;123;45;67mA\\x1b[38;2;89;123;45mB' * 39
output = (row + b'\\r\\n') * 22
while output:
    output = output[os.write(1, output):]
os.write(1, b'GEOMETRY_READY')
data = b''
while True:
    data += os.read(0, 4096)
    record_size()
    captured.write_bytes(data)
    os.write(1, b'INPUT_' + data)
"""
        session = self.server.create_session(
            "in_band_geometry", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("GEOMETRY_READY")
        # Sample geometry at input delivery, after attachment setup and each ordered resize.
        # A signal handler can reenter and overwrite a newer sample with intermediate geometry.
        client.send(b"I")
        client.expect_output("INPUT_I")
        self.assertEqual(json.loads(report.read_text()), [23, 80, 640, 368])
        # A PTY proxy still reports 80x24, but Ghostty sends its real grid and pixels in band.
        # Mouse input shares this read and must be classified against the new geometry.
        client.send(b"\x1b[48;50;160;1700;2560t\x1b[<0;114;35M\x1b[<0;114;35mZ")
        self.server.wait_for_state(
            session.name,
            lambda state: (state.columns, state.rows) == (160, 50),
            "in-band size supersedes stale PTY geometry",
        )
        # Observing the acknowledgement drains the redraw so output backpressure cannot
        # block the client's next input read while the test waits on the capture file.
        client.expect_output("INPUT_IZ")
        self.assertEqual(captured.read_bytes(), b"IZ")
        self.assertEqual(json.loads(report.read_text()), [49, 160, 2560, 1666])
        # A later SIGWINCH must not replace known native metrics with the stale proxy ioctl.
        os.killpg(client.pid, signal.SIGWINCH)
        client.send(b"Q")
        client.expect_output("INPUT_IZQ")
        self.assertEqual(captured.read_bytes(), b"IZQ")
        state = session.state()
        self.assertEqual((state.columns, state.rows), (160, 50))
        self.assertEqual(json.loads(report.read_text()), [49, 160, 2560, 1666])

    def test_drag_commits_immediately_and_coalesces_to_final_size(self) -> None:
        # Ghostty reports every applied Pane geometry change in band (mode 2048), so the child's
        # report count is the number of resizes the daemon actually performed.
        report = self.server.root / "reports.json"
        script = f"""
import json, os, re, tty
from pathlib import Path
report = Path({str(report)!r})
pattern = re.compile(rb'\\x1b\\[48;(\\d+);(\\d+);\\d+;\\d+t')
tty.setraw(0)
os.write(1, b'\\x1b[?2048h')
data = b''
count = 0
while True:
    chunk = os.read(0, 4096)
    if not chunk:
        break
    data += chunk
    end = 0
    for match in pattern.finditer(data):
        count += 1
        size = [int(match[1]), int(match[2])]
        end = match.end()
    data = data[end:] if end else data[-64:]
    if end:
        temporary = report.with_suffix('.tmp')
        temporary.write_text(json.dumps({{'count': count, 'size': size}}))
        temporary.replace(report)
        if count == 1:
            os.write(1, b'DRAG_' + b'READY')
"""
        session = self.server.create_session(
            "resize_drag", command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        client.expect_output("DRAG_READY")

        def reports() -> tuple[int, list[int]]:
            value = json.loads(report.read_text())
            return int(value["count"]), list(value["size"])

        self.assertEqual(reports(), (1, [23, 80]))

        # Keep resizing faster than any quiet period would allow. The first step must reach the
        # Pane while the drag continues rather than waiting for the gesture to end.
        started = time.monotonic()
        columns = 80
        while reports()[0] == 1:
            self.assertLess(
                columns, 230, "drag geometry was deferred until the drag ended"
            )
            columns += 1
            client.resize(columns, 24)
            client.drain(0.002)
        for _ in range(100):
            columns += 1
            client.resize(columns, 24)
            client.drain(0.002)
        deadline = time.monotonic() + 5.0
        while reports()[1] != [23, columns]:
            self.assertLess(
                time.monotonic(), deadline, f"final size never settled: {reports()}"
            )
            client.drain(0.01)
        settled = time.monotonic()

        # Commits are at least one interval apart, so the daemon's work is bounded by the drag's
        # duration rather than by the number of SIGWINCH samples.
        commit_interval = 0.016
        commits = reports()[0] - 1
        bound = int((settled - started) / commit_interval) + 1
        self.assertGreaterEqual(commits, 2)
        self.assertLessEqual(
            commits, bound, f"{commits} resizes for {columns - 80} steps"
        )
        state = session.state()
        self.assertEqual((state.columns, state.rows), (columns, 24))

    def test_nested_resize_reaches_each_real_child_pty(self) -> None:
        session = self.server.create_session("nested_resize")
        left = session.pane()
        top_right = left.split_right()
        bottom_right = top_right.split_down()
        client = session.require_client()

        client.resize(100, 30)
        self.server.wait_for_state(
            session.name,
            lambda state: state.columns == 100 and state.rows == 30,
            "settled 100x30 outer resize",
        )

        left.send("m='__LEFT_'; printf \"${m}GEOMETRY__ \"; stty size\r")
        left.expect_output("__LEFT_GEOMETRY__ 29 50")
        top_right.send("m='__TOP_'; printf \"${m}GEOMETRY__ \"; stty size\r")
        top_right.expect_output("__TOP_GEOMETRY__ 14 49")
        bottom_right.send("m='__BOTTOM_'; printf \"${m}GEOMETRY__ \"; stty size\r")
        bottom_right.expect_output("__BOTTOM_GEOMETRY__ 14 49")

        left.expect_alive()
        top_right.expect_alive()
        bottom_right.expect_alive()

    def test_split_delivers_in_band_size_without_another_keystroke(self) -> None:
        # After this Ghostty pin, resize itself emits CSI 48. That reply must reach the child PTY
        # from the split transaction, not from a later keystroke or PTY read. This test asserts the
        # cell geometry of an 80x24 client minus status; terminal-boundary tests cover pixel metrics.
        session = self.server.create_session("in_band_split")
        pane = session.pane()
        pane.send(
            "stty -echo -icanon min 1 time 0; "
            "esc=$(printf '\\033'); "
            "printf '%s[?2048h' \"$esc\"; "
            "buf=; "
            "while :; do "
            "c=$(dd bs=1 count=1 2>/dev/null) || exit 1; "
            'buf="${buf}${c}"; '
            'case $buf in *"${esc}[48;"*t) break ;; esac; '
            "done; "
            "armed='__2048_''ARMED__'; "
            "printf '%s\\n' \"$armed\"; "
            "buf=; "
            "while :; do "
            "c=$(dd bs=1 count=1 2>/dev/null) || exit 1; "
            'buf="${buf}${c}"; '
            'case $buf in *"${esc}[48;"*t) '
            'report=${buf#*"${esc}[48;"}; '
            "report=${report%%t*}; "
            "rows=${report%%;*}; "
            "rest=${report#*;}; "
            "cols=${rest%%;*}; "
            'printf \'__2048_SPLIT_%s_%s__\\n\' "$rows" "$cols"; '
            "break ;; esac; "
            "done\r"
        )
        pane.expect_output("__2048_ARMED__")

        pane.split_down()
        pane.expect_output("__2048_SPLIT_11_80__")
        pane.expect_alive()


if __name__ == "__main__":
    unittest.main()
