from __future__ import annotations

import os
import select
import time
import unittest

from tests.support.mux_harness import FrameServer


class BackpressureRecoveryTest(unittest.TestCase):
    def test_exact_two_mib_payload_recovers_after_outer_input_stalls(self) -> None:
        server = FrameServer.from_environment()
        self.addCleanup(server.close)
        gate = server.root / "release-blocked-pty"
        size = 2 * 1024 * 1024
        session = server.create_session(
            "blocked-recovery",
            command=(str(server.peer_path), "block", str(gate), str(size)),
            hold=True,
        )
        client = session.require_client()
        client.expect_output("__FRAME_PTY_READY__")

        payload = b"x" * size
        offset = 0
        stalled = False
        deadline = time.monotonic() + 20.0
        last_progress = time.monotonic()
        while offset < len(payload) and time.monotonic() < deadline:
            try:
                offset += os.write(client.process.descriptor, payload[offset:])
                last_progress = time.monotonic()
            except BlockingIOError:
                if (
                    not stalled
                    and offset >= 32 * 1024
                    and time.monotonic() - last_progress >= 0.1
                ):
                    stalled = True
                    gate.touch()
                client.drain(0.002)
                select.select([], [client.process.descriptor], [], 0.01)
        self.assertTrue(
            stalled, "2 MiB input never established observable backpressure"
        )
        self.assertEqual(
            offset, size, f"payload write stopped at {offset}/{size} bytes"
        )

        digest = 14_695_981_039_346_656_037
        for byte in payload:
            digest ^= byte
            digest = (digest * 1_099_511_628_211) & ((1 << 64) - 1)
        client.expect_output(
            f"__FRAME_PTY_DONE__ bytes={size} digest={digest:x}", timeout=15.0
        )
