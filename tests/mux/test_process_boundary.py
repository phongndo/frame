from __future__ import annotations

import errno
import os
import shlex
import unittest
from pathlib import Path

from tests.support.mux_harness import FrameServer, process_exists, wait_until


def release_fifo(path: Path) -> None:
    def release() -> bool | None:
        try:
            descriptor = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as error:
            if error.errno == errno.ENXIO:
                return None
            raise
        try:
            os.write(descriptor, b"go\n")
        finally:
            os.close(descriptor)
        return True

    wait_until(f"fixture reader for {path.name}", release)


class ProcessBoundaryMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = FrameServer.from_environment()
        self.addCleanup(self.server.close)

    def test_final_pty_output_precedes_structured_process_exit(self) -> None:
        release = self.server.root / "final-output.fifo"
        os.mkfifo(release)
        script = (
            f"IFS= read -r _ < {shlex.quote(str(release))}; "
            "printf '__FINAL_BEFORE_EXIT__\\n'; exit 7"
        )
        session = self.server.create_session(
            "final_output",
            hold=True,
            command=("/bin/sh", "-c", script),
        )
        pane = session.pane()
        child = pane.process
        self.assertGreater(child, 0)

        release_fifo(release)
        session.require_client().expect_output("__FINAL_BEFORE_EXIT__")
        exited = self.server.wait_for_state(
            session.name,
            lambda state: state.pane(pane.id).process_state == "exited",
            "held pane to publish its process exit",
        )
        self.assertEqual(exited.pane(pane.id).pid, 0)
        self.assertFalse(process_exists(child))

    def test_resize_delivers_sigwinch_to_the_real_child(self) -> None:
        session = self.server.create_session(
            "sigwinch",
            command=(str(self.server.peer_path), "winch"),
        )
        pane = session.pane()
        client = session.require_client()
        client.expect_output("__FRAME_WINCH_READY__")

        client.resize(100, 30)
        client.expect_output("__FRAME_WINCH_29_100__")
        resized = session.state()
        self.assertEqual(resized.focused_pane, pane.id)
        self.assertEqual((resized.columns, resized.rows), (100, 30))
        client.send("q")


if __name__ == "__main__":
    unittest.main()
