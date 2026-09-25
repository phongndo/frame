"""Exercise the real native probe and PTY fixture through their process interfaces."""

import json
import os
import select
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tests.support.pty_process import PtyOutputMonitor, PtyProcess  # noqa: E402


class CommandProbeTest(unittest.TestCase):
    probe: Path
    peer: Path

    def test_output_flood_keeps_a_dense_screen_changing(self) -> None:
        with subprocess.Popen(
            [str(self.peer), "output-flood"], stdout=subprocess.PIPE
        ) as process:
            try:
                assert process.stdout is not None
                output = bytearray()
                deadline = time.monotonic() + 2.0
                while len(output) < 400 * 501:
                    remaining = deadline - time.monotonic()
                    self.assertGreater(remaining, 0, "output flood stalled")
                    readable, _, _ = select.select([process.stdout], [], [], remaining)
                    self.assertTrue(readable, "output flood stalled")
                    chunk = os.read(process.stdout.fileno(), 400 * 501 - len(output))
                    if not chunk:
                        break
                    output.extend(chunk)
                rows = output.splitlines(keepends=True)
                self.assertEqual(len(rows), 400)
                self.assertTrue(all(len(row) == 501 for row in rows))
                self.assertTrue(all(row.endswith(b"\r\n") for row in rows))
                self.assertTrue(
                    all(all(33 <= byte <= 126 for byte in row[:-2]) for row in rows)
                )
                # Scrolling by a whole screen must not restore identical content.
                self.assertTrue(all(rows[i] != rows[i + 200] for i in range(200)))
            finally:
                process.kill()
                process.wait(timeout=2)

    def run_probe(self, last_reply: bytes) -> subprocess.CompletedProcess[str]:
        outer, fixture = socket.socketpair()
        with outer, fixture:
            outer.setblocking(False)
            fixture.settimeout(5)
            with subprocess.Popen(
                [str(self.probe), "command", str(outer.fileno()), "2", "\n", "DONE"],
                pass_fds=(outer.fileno(),),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            ) as process:
                try:
                    self.assertEqual(fixture.recv(1), b"\n")
                    fixture.sendall(b"DONE0|")
                    self.assertEqual(fixture.recv(1), b"\n")
                    # EOF makes rejection deterministic; no timing threshold or sleep is needed.
                    fixture.sendall(last_reply)
                    fixture.shutdown(socket.SHUT_WR)
                    stdout, stderr = process.communicate(timeout=5)
                    return subprocess.CompletedProcess(
                        process.args, process.returncode, stdout, stderr
                    )
                finally:
                    if process.poll() is None:
                        process.kill()

    def test_stale_completion_cannot_finish_the_next_command(self) -> None:
        result = self.run_probe(b"DONE0|")
        self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_longer_sequence_is_not_a_prefix_match(self) -> None:
        result = self.run_probe(b"DONE10|")
        self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_current_completion_can_follow_a_stale_redraw(self) -> None:
        result = self.run_probe(b"DONE0|DONE\x1b[2;1H1|")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(json.loads(result.stdout)["latency"]["samples_ns"]), 2)

    def test_fixture_numbers_each_completed_burst(self) -> None:
        result = subprocess.run(
            [str(self.peer), "warm-scroll-loop"],
            input=b"\n\n",
            capture_output=True,
            timeout=10,
            check=True,
        )
        marker = b"__LEMMA_WARM_SCROLL_DONE__"
        self.assertEqual(result.stdout.count(marker), 2)
        self.assertTrue(marker + b"0|\r\n" in result.stdout, "missing completion 0")
        self.assertTrue(result.stdout.endswith(marker + b"1|\r\n"))


class TriggerProbeTest(unittest.TestCase):
    probe: Path

    def run_trigger(self, reply: bytes) -> subprocess.CompletedProcess[str]:
        outer, fixture = socket.socketpair()
        with outer, fixture:
            outer.setblocking(False)
            fixture.settimeout(5)
            with subprocess.Popen(
                [
                    str(self.probe),
                    "trigger",
                    str(outer.fileno()),
                    b"\x02n".hex(),
                    "BCDEFG",
                ],
                pass_fds=(outer.fileno(),),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            ) as process:
                try:
                    self.assertEqual(fixture.recv(2), b"\x02n")
                    fixture.sendall(reply)
                    fixture.shutdown(socket.SHUT_WR)
                    stdout, stderr = process.communicate(timeout=10)
                    return subprocess.CompletedProcess(
                        process.args, process.returncode, stdout, stderr
                    )
                finally:
                    if process.poll() is None:
                        process.kill()

    def test_only_the_prepared_token_completes_the_sample(self) -> None:
        result = self.run_trigger(b"__LEMMA_TAB_0000_AAAAAA__")
        self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_token_is_decoded_across_differential_cursor_moves(self) -> None:
        reply = b"__LEMMA_TAB_0001_BC\x1b[3;20HDEFG__"
        result = self.run_trigger(reply)
        self.assertEqual(result.returncode, 0, result.stderr)
        measured = json.loads(result.stdout)
        self.assertGreater(measured["latency_ns"], 0)
        self.assertEqual(measured["outer_bytes"], len(reply))

    def test_rejects_an_undecodable_trigger(self) -> None:
        result = subprocess.run(
            [str(self.probe), "trigger", "0", "0", "BCDEFG"],
            capture_output=True,
            timeout=5,
            check=False,
        )
        self.assertEqual(result.returncode, 2)


class PainterTest(unittest.TestCase):
    probe: Path
    peer: Path

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.control = root / "control.sock"
        self.receipts = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.addCleanup(self.receipts.close)
        self.receipts.bind(str(root / "receipt.sock"))
        self.receipts.settimeout(5)
        self.receipt_path = root / "receipt.sock"

    def start(self, mode: str, ready: bytes) -> PtyProcess:
        process = PtyProcess(
            [str(self.peer), mode, str(self.receipt_path), str(self.control)],
            dict(os.environ),
        )
        self.addCleanup(process.close)
        process.read_until(ready, 2.0)
        return process

    def test_paints_a_hidden_frame_before_acknowledging(self) -> None:
        process = self.start("paint", b"__LEMMA_PAINT_READY__")
        self.receipts.sendto(b"c__LEMMA_SESSION_0002_CCCCCC__", str(self.control))
        self.assertEqual(self.receipts.recv(256), b"__LEMMA_SESSION_0002_CCCCCC__")
        process.read_until(b"__LEMMA_SESSION_0002_CCCCCC__", 2.0)
        self.assertIn("c" * 40, process.screen.text())

    def test_rejects_a_control_without_a_fill_letter(self) -> None:
        process = self.start("paint", b"__LEMMA_PAINT_READY__")
        self.receipts.sendto(b"__LEMMA_SESSION_0002_CCCCCC__", str(self.control))
        with self.assertRaisesRegex(RuntimeError, "exited unsuccessfully"):
            process.wait_for_exit(2.0)

    def test_resize_reports_the_new_geometry_before_the_repaint(self) -> None:
        process = self.start("resize-paint", b"__LEMMA_RESIZE_READY__")
        marker = "__LEMMA_RESIZE_0000_AAAAAA__"
        self.receipts.sendto(b"a" + marker.encode(), str(self.control))
        self.assertEqual(self.receipts.recv(256), b"__LEMMA_RESIZE_ARMED__")
        self.receipts.setblocking(False)
        completed = subprocess.run(
            [
                str(self.probe),
                "resize",
                str(process.descriptor),
                str(self.receipts.fileno()),
                "30",
                "100",
                marker,
                "AAAAAA",
            ],
            pass_fds=(process.descriptor, self.receipts.fileno()),
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
        measured = json.loads(completed.stdout)
        self.assertEqual(measured["pane_geometry"], "100x30")
        self.assertGreater(measured["resize_to_pty_ns"], 0)
        self.assertGreaterEqual(
            measured["resize_to_outer_bytes_ns"], measured["resize_to_pty_ns"]
        )


class BlockedPeerTest(unittest.TestCase):
    peer: Path

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.gate = Path(temporary.name) / "gate"

    def start_peer(self, size: int, idle_timeout_ms: int) -> PtyProcess:
        process = PtyProcess(
            [str(self.peer), "block", str(self.gate), str(size), str(idle_timeout_ms)],
            dict(os.environ),
        )
        self.addCleanup(process.close)
        process.read_until(b"__LEMMA_PTY_READY__", 2.0)
        self.gate.touch()
        return process

    def test_progress_can_outlive_the_receive_idle_guard(self) -> None:
        payload = b"q" * 16
        digest = 14_695_981_039_346_656_037
        for byte in payload:
            digest = ((digest ^ byte) * 1_099_511_628_211) & ((1 << 64) - 1)
        marker = f"__LEMMA_PTY_DONE__ bytes={len(payload)} digest={digest:x}".encode()
        process = self.start_peer(len(payload), 1_000)
        with PtyOutputMonitor(
            process, marker, failure_markers=(b"__LEMMA_PTY_FAILED__",)
        ) as completion:
            for index, byte in enumerate(payload):
                completion.check()
                process.write_all(bytes([byte]), 1.0)
                if index + 1 < len(payload):
                    # Deliberately span more than one idle window, while each
                    # delivery is well within it. This is fixture liveness, not
                    # a measured latency or throughput threshold.
                    time.sleep(0.1)
            completion.wait(2.0)

    def test_stalled_partial_payload_still_reports_its_received_count(self) -> None:
        process = self.start_peer(8, 300)
        process.write_all(b"qqq", 1.0)
        process.read_until(b"__LEMMA_PTY_FAILED__ received=3", 2.0)

    def test_override_cannot_disable_or_raise_the_idle_guard(self) -> None:
        for timeout in ("0", "30001", "invalid"):
            with self.subTest(timeout=timeout):
                result = subprocess.run(
                    [str(self.peer), "block", str(self.gate), "1", timeout],
                    capture_output=True,
                    timeout=2,
                    check=False,
                )
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    CommandProbeTest.probe = TriggerProbeTest.probe = PainterTest.probe = Path(
        sys.argv[1]
    ).resolve()
    CommandProbeTest.peer = BlockedPeerTest.peer = PainterTest.peer = Path(
        sys.argv[2]
    ).resolve()
    unittest.main(argv=[sys.argv[0]])
