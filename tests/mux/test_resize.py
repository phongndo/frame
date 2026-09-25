from __future__ import annotations

import itertools
import json
import os
import signal
import subprocess
import sys
import time
import unittest
from pathlib import Path
from typing import Any

from tests.support.mux_harness import Client, LemmaServer, Session

# Mirrors OuterResizeSchedule::commit_interval.
OUTER_RESIZE_COMMIT_INTERVAL = 0.016
HISTORY_LINES = 3_000


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

    def recorder_script(self, log: Path, history_lines: int) -> str:
        # Ghostty reports every applied Pane geometry change in band (mode 2048), so the recorded
        # report count is the number of resizes the daemon actually performed on this Pane.
        return f"""
import fcntl, json, os, re, struct, termios, time, tty
log = open({str(log)!r}, 'a', buffering=1)
tty.setraw(0)
while {history_lines} and not os.path.exists({str(self.history_gate)!r}):
    time.sleep(0.01)
for start in range(0, {history_lines}, 1000):
    block = b''.join(b'%07d\\r\\n' % index
                     for index in range(start, min(start + 1000, {history_lines})))
    while block:
        block = block[os.write(1, block):]
os.write(1, b'\\x1b[?2048h')
pattern = re.compile(rb'\\x1b\\[48;(\\d+);(\\d+);\\d+;\\d+t')
data = b''
size = None
while True:
    chunk = os.read(0, 4096)
    if not chunk:
        break
    data += chunk
    rest = b''
    end = 0
    for match in pattern.finditer(data):
        rest += data[end:match.start()]
        end = match.end()
        size = [int(match[1]), int(match[2])]
        log.write(json.dumps({{'t': time.monotonic(), 'size': size}}) + '\\n')
    tail = data[end:]
    partial = tail.find(b'\\x1b')
    if partial >= 0:
        rest += tail[:partial]
        data = tail[partial:]
    else:
        rest += tail
        data = b''
    for byte in rest:
        winsz = struct.unpack('HHHH', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\\0' * 8))
        log.write(json.dumps({{'t': time.monotonic(), 'input': chr(byte), 'size': size,
                              'winsz': list(winsz[:2])}}) + '\\n')
        os.write(1, b'INPUT_' + bytes([byte]))
"""

    def start_geometry_recorder(self, name: str) -> tuple[Session, Client, Path, int]:
        log = self.server.root / f"{name}.jsonl"
        script = self.recorder_script(log, 0)
        session = self.server.create_session(
            name, command=(sys.executable, "-c", script)
        )
        client = session.require_client()
        self.wait_for_recorded(client, log)
        # The recorder may arm before or after the attach resize, so its early reports vary.
        # Attachment geometry reaches the Pane PTY before later input on that connection: once
        # this byte arrives, every attach-time report precedes it in the log.
        client.send(b"1")
        synced = self.wait_for_input(client, log, "1")
        self.assertEqual(synced["winsz"], [23, 80])
        self.assertEqual(synced["size"], [23, 80])
        return session, client, log, self.recorded(log).index(synced)

    @property
    def history_gate(self) -> Path:
        return self.server.root / "fill-history"

    def wait_for_recorded(self, client: Client, log: Path) -> None:
        deadline = time.monotonic() + 60.0
        while not log.exists() or not self.resizes(log):
            self.assertLess(time.monotonic(), deadline, "geometry recorder never armed")
            client.drain(0.01)

    @staticmethod
    def recorded(log: Path) -> list[dict[str, Any]]:
        lines = log.read_text().split("\n")
        return [json.loads(line) for line in lines[:-1]]

    @classmethod
    def resizes(cls, log: Path, after: int = -1) -> list[dict[str, Any]]:
        return [
            event for event in cls.recorded(log)[after + 1 :] if "input" not in event
        ]

    def wait_for_size(self, client: Client, log: Path, size: list[int]) -> None:
        deadline = time.monotonic() + 30.0
        while self.resizes(log)[-1]["size"] != size:
            self.assertLess(
                time.monotonic(),
                deadline,
                f"size {size} never settled: {self.resizes(log)[-3:]}",
            )
            client.drain(0.01)

    def wait_for_input(self, client: Client, log: Path, byte: str) -> dict[str, Any]:
        deadline = time.monotonic() + 30.0
        while True:
            received = [
                event for event in self.recorded(log) if event.get("input") == byte
            ]
            if received:
                return received[0]
            self.assertLess(time.monotonic(), deadline, f"input {byte!r} never arrived")
            client.drain(0.01)

    def assert_paced(
        self,
        log: Path,
        synced: int,
        started: float,
        settled: float,
        *,
        forced: int = 0,
    ) -> None:
        # Paced commits are at least one interval apart, so daemon work is bounded by the drag's
        # duration rather than by the number of size samples. Input may force out one more.
        # Count grid changes: a first pixel-size change reports the unchanged grid again.
        sizes = [self.recorded(log)[synced]["size"]]
        sizes += [event["size"] for event in self.resizes(log, synced)]
        applied = sum(
            1 for before, after in itertools.pairwise(sizes) if before != after
        )
        bound = int((settled - started) / OUTER_RESIZE_COMMIT_INTERVAL) + 1 + forced
        self.assertGreaterEqual(applied, 2)
        self.assertLessEqual(
            applied, bound, f"{applied} resizes in {settled - started:.3f}s"
        )

    def test_drag_commits_immediately_and_coalesces_to_final_size(self) -> None:
        session, client, log, synced = self.start_geometry_recorder("resize_drag")

        # Keep resizing faster than any quiet period would allow. The first step must reach the
        # Pane while the drag continues rather than waiting for the gesture to end.
        started = time.monotonic()
        columns = 80
        while not self.resizes(log, synced):
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
        self.wait_for_size(client, log, [23, columns])
        self.assert_paced(log, synced, started, time.monotonic())
        state = session.state()
        self.assertEqual((state.columns, state.rows), (columns, 24))

    def test_in_band_size_reports_are_paced_and_precede_later_input(self) -> None:
        session, client, log, synced = self.start_geometry_recorder("in_band_drag")

        def report(columns: int) -> bytes:
            return b"\x1b[48;30;%d;600;%dt" % (columns, columns * 8)

        # Terminals with mode 2048 deliver a drag as a stream of stdin reports. Each report must
        # join the paced commit rather than forcing out the previous size.
        started = time.monotonic()
        columns = 80
        for _ in range(100):
            columns += 1
            client.send(report(columns))
            client.drain(0.002)
        # A key in the same read as the last report still follows the final geometry.
        columns += 1
        client.send(report(columns) + b"K")
        received = self.wait_for_input(client, log, "K")
        self.assertEqual(received["size"], [29, columns])
        self.assertEqual(received["winsz"], [29, columns])
        self.assert_paced(log, synced, started, time.monotonic(), forced=1)
        state = session.state()
        self.assertEqual((state.columns, state.rows), (columns, 30))

    def test_daemon_applies_only_newest_queued_geometry_before_input(self) -> None:
        # Reflow cost grows with history, so a drag over deep scrollback can queue geometry faster
        # than Panes reflow. A raw client delivers that backlog at once: the daemon must apply only
        # the newest queued size, and the input queued behind it must still observe that size.
        self.server = LemmaServer.from_environment(
            config_text='require("lemma").setup({ terminal = { scrollback_lines = %d } })'
            % HISTORY_LINES
        )
        self.addCleanup(self.server.close)
        name = "queued_geometry"
        log = self.server.root / "queued.jsonl"
        self.server.create_session(
            name,
            attach=False,
            command=(sys.executable, "-c", self.recorder_script(log, HISTORY_LINES)),
        )
        initial = self.server.session_state(name)
        assert initial is not None
        filled = []
        for index, axis in enumerate(("--right", "--down")):
            filled.append(self.server.root / f"filled-{index}")
            filler = (
                "import os, sys, time\n"
                f"while not os.path.exists({str(self.history_gate)!r}): time.sleep(0.01)\n"
                f"sys.stdout.writelines('%07d\\n' % i for i in range({HISTORY_LINES}))\n"
                "sys.stdout.flush()\n"
                f"open({str(filled[-1])!r}, 'w').close()\n"
                "time.sleep(3600)\n"
            )
            self.server.require_command(
                "split",
                "--session",
                name,
                "--pane",
                initial.focused_pane,
                axis,
                "--focus",
                "preserve",
                "--",
                sys.executable,
                "-c",
                filler,
            )
        self.history_gate.touch()
        deadline = time.monotonic() + 60.0
        while not all(path.exists() for path in filled) or not (
            log.exists() and self.resizes(log)
        ):
            if time.monotonic() > deadline:
                self.fail(
                    f"history fill did not finish: {[p.exists() for p in filled]}"
                )
            time.sleep(0.05)
        # The recorder arms once, after every layout change, and nothing is attached yet.
        self.assertEqual(len(self.recorded(log)), 1)

        steps = 200
        burst = subprocess.Popen(
            [
                str(self.server.cli_path),
                str(self.server.socket_path),
                "geometry-burst",
                name,
                str(steps),
            ],
            env=self.server.environment,
            stdin=subprocess.PIPE,
        )
        self.addCleanup(burst.kill)
        deadline = time.monotonic() + 30.0
        while not any(event.get("input") == "K" for event in self.recorded(log)):
            if time.monotonic() > deadline:
                self.fail(f"queued input never arrived: {self.resizes(log)[-3:]}")
            time.sleep(0.005)
        assert burst.stdin is not None
        burst.stdin.close()
        self.assertEqual(burst.wait(timeout=10.0), 0)

        events = self.recorded(log)
        index = next(i for i, event in enumerate(events) if event.get("input") == "K")
        received = events[index]
        applied = events[1:index]
        # The Pane PTY already had the newest geometry when the input arrived.
        self.assertEqual(received["winsz"], received["size"])
        final_columns = 100 + 1 + (steps - 1) % 2
        state = self.server.session_state(name)
        assert state is not None
        self.assertEqual((state.columns, state.rows), (final_columns, 30))
        # Attachment resizes once to its hello geometry; the queued backlog then costs one
        # reflow per bounded read rather than one per superseded message.
        self.assertLessEqual(len(applied), 4, applied)

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
