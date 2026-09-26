from __future__ import annotations

import os
import pty
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from tests.support.pty_process import (
    FINAL_PTY_OUTPUT_BYTES,
    AnsiScreenTracker,
    PtyOutputMonitor,
    PtyProcess,
)

LEMMA_OUTER_TERMINAL_RESTORE = (
    b"\x1b[0m\x1b[?2026l\x1b[?1l\x1b[?9l\x1b[?1000l\x1b[?1002l\x1b[?1003l"
    b"\x1b[?1004l\x1b[?1005l\x1b[?1006l\x1b[?1007l\x1b[?1015l\x1b[?1016l"
    b"\x1b[?2004l\x1b]112\x1b\\\x1b[0 q\x1b[?25h\x1b[?7h\x1b[<u\x1b[?1049l"
)


class AnsiScreenTrackerTest(unittest.TestCase):
    def test_bounded_erase_preserves_neighbors_and_cursor(self) -> None:
        tracker = AnsiScreenTracker(12, 1)
        tracker.feed(b"leftxxxxkeep\x1b[1;5H\x1b[4X")
        self.assertEqual(tracker.text(), "left    keep")
        tracker.feed(b"!")
        self.assertEqual(tracker.text(), "left!   keep")
        tracker.feed(b"\x1b[1;12H\x1b[99X")
        self.assertEqual(tracker.text(), "left!   kee")

    def test_scrolls_only_inside_vertical_margins(self) -> None:
        tracker = AnsiScreenTracker(6, 4)
        tracker.feed(b"status\r\none\r\ntwo\r\nthree")
        tracker.feed(b"\x1b[2;4r\x1b[1S\x1b[r\x1b[4;1Hfour")
        self.assertEqual(tracker.text(), "status\ntwo\nthree\nfour")
        tracker.feed(b"\x1b[2;4r\x1b[2T\x1b[r")
        self.assertEqual(tracker.text(), "status\n\n\ntwo")
        # Private modes sharing a final byte do not change margins or scroll.
        tracker.feed(b"\x1b[4;1H\x1b[?2048r\x1b[?1S\n")
        self.assertEqual(tracker.text(), "\n\ntwo\n")

    def test_text_retains_presented_frame_until_synchronized_update_finishes(
        self,
    ) -> None:
        tracker = AnsiScreenTracker(16, 1)
        tracker.feed(b"Session")
        tracker.feed(b"\x1b[?2026h\x1b[2J")
        self.assertEqual(tracker.text(), "Session")
        tracker.feed(b"\x1b[?2026h\x1b[HPane\x1b[?2026")
        self.assertEqual(tracker.text(), "Session")
        tracker.feed(b"l")
        self.assertEqual(tracker.text(), "Pane")
        tracker.feed(b"\x1b[?2026h\x1b[2J\x1b[?2026l")
        self.assertEqual(tracker.text(), "")

    def test_finds_marker_across_fragmented_incremental_cell_updates(self) -> None:
        tracker = AnsiScreenTracker(80, 24)
        for fragment in (
            b"\x1b[23;1H__LEMMA_DONE",
            b"\x1b]0;ignored\x1b\\",
            b"\x1b[23;13H__",
        ):
            tracker.feed(fragment)

        self.assertTrue(tracker.contains(b"__LEMMA_DONE__"))
        self.assertFalse(tracker.contains(b"ignored"))

    def test_reports_a_sparse_marker_overwritten_later_in_the_same_feed(self) -> None:
        tracker = AnsiScreenTracker(80, 24)
        tracker.feed(b"\x1b[23;1HS0030X")

        observed = tracker.feed_observing(b"\x1b[23;5H2\x1b[23;1H      ", b"S0032X")

        self.assertTrue(observed)
        self.assertFalse(tracker.contains(b"S0032X"))


class PtyOutputMonitorTest(unittest.TestCase):
    def start_peer(self, script: str) -> PtyProcess:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.output_finished = Path(temporary.name) / "output-finished"
        process = PtyProcess(
            [
                sys.executable,
                "-c",
                "import os, sys, tty\n"
                "from pathlib import Path\n"
                "tty.setraw(0)\n"
                "def emit(data):\n"
                "    while data:\n"
                "        data = data[os.write(1, data):]\n"
                "emit(b'READY')\n"
                "os.read(0, 1)\n" + script + "Path(sys.argv[1]).touch()\n",
                str(self.output_finished),
            ],
            dict(os.environ),
        )
        self.addCleanup(process.close)
        process.read_until(b"READY", 2.0)
        return process

    def test_retains_transient_visible_completion_and_keeps_draining(self) -> None:
        process = self.start_peer(
            "emit(b'\\x1b[1;1HS0030X')\n"
            "emit(b'\\x1b[1;5H2\\x1b[1;1H      ')\n"
            "emit(b'\\r' * (256 * 1024))\n"
        )
        with PtyOutputMonitor(process, b"S0032X", visible_text=True) as completion:
            process.write_all(b"x", 2.0)
            completion.wait(2.0)
            # The child must be able to finish its flood after completion was
            # observed, without the main thread reading this PTY.
            self.wait_for_output()
        self.assertLessEqual(len(process.output_tail), FINAL_PTY_OUTPUT_BYTES)
        self.assertNotIn(b"S0032X", process.output_tail)
        self.assertFalse(process.screen.contains(b"S0032X"))

    def test_retains_fragmented_failure_and_drains_before_scope_exit(self) -> None:
        process = self.start_peer(
            "emit(b'lost con')\nemit(b'nection')\nemit(b'\\r' * (256 * 1024))\n"
        )
        with self.assertRaisesRegex(RuntimeError, "observed failure.*lost connection"):
            with PtyOutputMonitor(
                process, b"DONE", failure_markers=(b"lost connection",)
            ) as completion:
                process.write_all(b"x", 2.0)
                self.wait_for_output()
                completion.wait(2.0)
        self.assertLessEqual(len(process.output_tail), FINAL_PTY_OUTPUT_BYTES)

    def test_eof_cannot_complete_the_workload(self) -> None:
        process = self.start_peer("emit(b'incomplete')\n")
        with self.assertRaisesRegex(RuntimeError, "PTY closed before.*DONE"):
            with PtyOutputMonitor(process, b"DONE") as completion:
                process.write_all(b"x", 2.0)
                completion.wait(2.0)

    def test_timeout_returns_read_ownership(self) -> None:
        process = self.start_peer("emit(b'LATER')\n")
        with self.assertRaisesRegex(TimeoutError, "did not observe.*DONE"):
            with PtyOutputMonitor(process, b"DONE") as completion:
                completion.wait(0.02)
        process.write_all(b"x", 2.0)
        process.read_until(b"LATER", 2.0)

    def wait_for_output(self) -> None:
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if self.output_finished.exists():
                return
            time.sleep(0.005)
        self.fail("child could not finish writing while the monitor was active")


class PtyProcessBufferingTest(unittest.TestCase):
    def test_resize_waits_for_child_process_group_creation(self) -> None:
        def delayed_fork() -> tuple[int, int]:
            master, slave = os.openpty()
            pid = os.fork()
            if pid == 0:
                os.close(master)
                # Pin the forkpty race: the parent returns before login_tty creates
                # the child's process group, as on a busy sanitizer runner.
                time.sleep(0.1)
                os.login_tty(slave)
                return 0, -1
            os.close(slave)
            return pid, master

        with patch.object(pty, "fork", delayed_fork):
            process = PtyProcess(
                [sys.executable, "-c", "import time; time.sleep(5)"], dict(os.environ)
            )
        self.addCleanup(process.close)
        process.resize(60, 12)
        self.assertEqual(os.getpgid(process.pid), process.pid)

    def test_later_children_do_not_inherit_another_clients_pty(self) -> None:
        first = PtyProcess(["cat"], dict(os.environ))
        self.addCleanup(first.close)
        script = (
            "import os\n"
            "try:\n"
            f"    os.fstat({first.descriptor})\n"
            "except OSError:\n"
            "    print('PTY_NOT_INHERITED', flush=True)\n"
            "else:\n"
            "    raise SystemExit('inherited another client PTY')\n"
        )
        second = PtyProcess([sys.executable, "-c", script], dict(os.environ))
        self.addCleanup(second.close)
        second.read_until(b"PTY_NOT_INHERITED", 1.0)
        second.wait_for_exit(1.0)

    def test_wait_for_exit_drains_child_output(self) -> None:
        script = (
            "import os\n"
            "data = b'x' * (256 * 1024)\n"
            "while data:\n"
            "    data = data[os.write(1, data):]\n"
        )
        process = PtyProcess([sys.executable, "-c", script], dict(os.environ))
        try:
            process.wait_for_exit(5.0)
            self.assertEqual(process.pid, -1)
            self.assertTrue(process.terminal_state_restored)
        finally:
            process.close()

    def test_requires_configured_terminal_mode_cleanup(self) -> None:
        process = PtyProcess(
            [sys.executable, "-c", "pass"],
            dict(os.environ),
            terminal_restore_sequence=LEMMA_OUTER_TERMINAL_RESTORE,
        )
        try:
            process.wait_for_exit(5.0)
            self.assertFalse(process.terminal_modes_restored)
            self.assertFalse(process.terminal_state_restored)
        finally:
            process.close()

    def test_retains_configured_terminal_mode_cleanup(self) -> None:
        script = f"import os; os.write(1, {LEMMA_OUTER_TERMINAL_RESTORE!r})"
        process = PtyProcess(
            [sys.executable, "-c", script],
            dict(os.environ),
            terminal_restore_sequence=LEMMA_OUTER_TERMINAL_RESTORE,
        )
        try:
            process.wait_for_exit(5.0)
            self.assertTrue(process.terminal_modes_restored)
            self.assertTrue(process.terminal_state_restored)
            self.assertIn(LEMMA_OUTER_TERMINAL_RESTORE, process.final_output)
        finally:
            process.close()

    def test_cross_version_cleanup_requires_one_complete_known_sequence(self) -> None:
        alternatives = (b"\x1b[?2048r\x1b[?1049l", b"\x1b[?1049l\x1b[0m")
        for output, restored in (
            (alternatives[0], True),
            (alternatives[1], True),
            (b"\x1b[?1049l", False),
        ):
            with self.subTest(output=output):
                process = PtyProcess(
                    [sys.executable, "-c", f"import os; os.write(1, {output!r})"],
                    dict(os.environ),
                    terminal_restore_sequence=alternatives,
                )
                try:
                    process.wait_for_exit(5.0)
                    self.assertEqual(process.terminal_state_restored, restored)
                finally:
                    process.close()

    def test_handshake_preserves_bytes_from_the_same_read(self) -> None:
        read_descriptor, write_descriptor = os.pipe()
        process = object.__new__(PtyProcess)
        process.descriptor = read_descriptor
        process.pending_read = b""
        process.output_tail = b""
        process.screen = AnsiScreenTracker(80, 24)
        handshake = b"\x1b[?1049h"
        marker = b"__VISIBLE__"
        suffix = marker + b"tail"
        try:
            os.write(write_descriptor, b"prefix" + handshake + suffix)
            _, handshake_bytes = process.read_until(
                handshake, 1.0, preserve_suffix=True
            )
            self.assertEqual(handshake_bytes, len(b"prefix" + handshake))
            self.assertEqual(process.pending_read, suffix)

            _, visible_bytes = process.read_until(marker, 1.0)
            self.assertEqual(visible_bytes, len(suffix))
            self.assertEqual(process.pending_read, b"")
        finally:
            os.close(read_descriptor)
            os.close(write_descriptor)


if __name__ == "__main__":
    unittest.main()
