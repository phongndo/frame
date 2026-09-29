from __future__ import annotations

import json
import socket
import unittest
from unittest.mock import patch

from extensions.frame_client import (
    ERROR,
    EVENT,
    HEADER,
    MAGIC,
    MAX_EVENTS,
    MAX_RECORD,
    PROC,
    RESULT,
    UPDATE,
    WELCOME,
    Client,
    ProtocolError,
    Rejected,
    command_context,
)


def record(kind: int, sequence: int, document: object) -> bytes:
    payload = json.dumps(document, separators=(",", ":")).encode()
    return HEADER.pack(MAGIC, 1, 0, kind, 0, len(payload), sequence) + payload


class ConnectedSocket:
    """Keep real stream framing while bypassing only AF_UNIX endpoint lookup."""

    def __init__(self, connection: socket.socket) -> None:
        self.connection = connection

    def connect(self, _endpoint: str) -> None:
        pass

    def __getattr__(self, name: str):  # type: ignore[no-untyped-def]
        return getattr(self.connection, name)


class ExtensionClientTest(unittest.TestCase):
    def client(self, limit: int = MAX_RECORD) -> tuple[Client, socket.socket]:
        local, server = socket.socketpair()
        self.addCleanup(local.close)
        self.addCleanup(server.close)
        server.sendall(record(WELCOME, 1, {"limits": {"record_bytes": limit}}))
        with patch(
            "extensions.frame_client.socket.socket", return_value=ConnectedSocket(local)
        ):
            client = Client("unused", name="test", session="0:1")
        self.addCleanup(client.close)
        server.recv(MAX_RECORD)  # Consume Hello, not any future Proc.
        return client, server

    def test_surface_capability_requires_a_session_before_connecting(self) -> None:
        with patch("extensions.frame_client.socket.socket") as create_socket:
            with self.assertRaisesRegex(
                ValueError, "surface capability requires a session"
            ):
                Client("unused", name="test", capabilities=("proc", "surface"))
            create_socket.assert_not_called()

    def test_proc_preserves_interleaved_events_and_partial_completion(self) -> None:
        client, server = self.client()
        event = {"event": "state.changed"}
        result = {"ok": False, "results": [{"result": {"status": "applied"}}]}
        server.sendall(record(EVENT, 1, event) + record(RESULT, 2, result))
        self.assertEqual(client.proc({"command": "session.list"}), result)
        self.assertEqual(client.event(), event)
        self.assertIsNone(client.pending)

    def test_event_timeout_preserves_partial_header_and_body(self) -> None:
        client, server = self.client()
        data = record(EVENT, 1, {"event": "state.changed"})
        server.sendall(data[:7])
        with self.assertRaises(TimeoutError):
            client.event(0.01)
        self.assertFalse(client.closed)
        server.sendall(data[7:20])
        with self.assertRaises(TimeoutError):
            client.event(0.01)
        server.sendall(data[20:])
        self.assertEqual(client.event()["event"], "state.changed")

    def test_request_timeout_closes_instead_of_replaying_mutation(self) -> None:
        client, server = self.client()
        client.timeout = 0.01
        with self.assertRaises(TimeoutError):
            client.proc({"command": "session.start"})
        self.assertTrue(client.closed)
        wire = server.recv(MAX_RECORD)
        self.assertEqual(HEADER.unpack(wire[: HEADER.size])[3], PROC)
        self.assertEqual(server.recv(MAX_RECORD), b"")

    def test_one_total_deadline_is_used_for_interleaved_events(self) -> None:
        client, server = self.client()
        server.sendall(record(EVENT, 1, {}) * 3 + record(RESULT, 2, {"ok": True}))
        client.timeout = 3
        with patch(
            "extensions.frame_client.time.monotonic", side_effect=[0, 0, 1, 2, 4]
        ):
            with self.assertRaises(TimeoutError):
                client.proc({"command": "session.list"})
        self.assertTrue(client.closed)

    def test_only_one_outstanding_proc(self) -> None:
        client, server = self.client()
        sequence = client.send(PROC, {"schema": "frame.proc/v1", "commands": []})
        with self.assertRaises(ProtocolError):
            client.send(PROC, {})
        with self.assertRaises(ProtocolError):
            client.event()
        server.sendall(record(RESULT, sequence, {"ok": True}))
        self.assertTrue(client.receive().document["ok"])
        self.assertIsNone(client.pending)

    def test_update_error_is_correlated_not_a_proc_result(self) -> None:
        client, server = self.client()
        sequence = client.update("0:1", rows=[])
        data = server.recv(MAX_RECORD)
        self.assertEqual(HEADER.unpack(data[: HEADER.size])[3], UPDATE)
        server.sendall(record(ERROR, sequence, {"reason": "stale"}))
        with self.assertRaises(Rejected) as failure:
            client.event()
        self.assertEqual(failure.exception.sequence, sequence)
        self.assertEqual(failure.exception.document, {"reason": "stale"})
        self.assertFalse(client.closed)

    def test_negotiated_limit_is_enforced_before_send(self) -> None:
        client, server = self.client(100)
        with self.assertRaises(ProtocolError):
            client.update("0:1", text="x" * 101)
        self.assertEqual(client.sequence, 1)
        server.setblocking(False)
        with self.assertRaises(BlockingIOError):
            server.recv(MAX_RECORD)

    def test_bad_headers_close_the_connection(self) -> None:
        for kind, sequence, size in [
            (0, 1, 0),
            (EVENT, 0, 0),
            (EVENT, 1, MAX_RECORD + 1),
        ]:
            with self.subTest(kind=kind, sequence=sequence, size=size):
                client, server = self.client()
                server.sendall(HEADER.pack(MAGIC, 1, 0, kind, 0, size, sequence))
                with self.assertRaises(ProtocolError):
                    client.event()
                self.assertTrue(client.closed)

    def test_invalid_json_is_rejected(self) -> None:
        for payload in (b'{"x":1,"x":2}', b'{"x":NaN}', b"[]", b'"\xff"'):
            with self.subTest(payload=payload):
                client, server = self.client()
                server.sendall(
                    HEADER.pack(MAGIC, 1, 0, EVENT, 0, len(payload), 1) + payload
                )
                with self.assertRaises((ValueError, ProtocolError)):
                    client.event()
                self.assertTrue(client.closed)

    def test_unexpected_result_cannot_complete_another_request(self) -> None:
        client, server = self.client()
        server.sendall(record(RESULT, 3, {"ok": True}))
        with self.assertRaises(ProtocolError):
            client.proc({"command": "session.list"})
        self.assertTrue(client.closed)

    def test_event_queue_is_bounded(self) -> None:
        client, server = self.client()
        server.sendall(record(EVENT, 1, {}) * (MAX_EVENTS + 1))
        with self.assertRaises(ProtocolError):
            client.proc({"command": "session.list"})
        self.assertLessEqual(len(client.events), MAX_EVENTS)
        self.assertTrue(client.closed)

    def test_context_is_captured_not_inferred_from_focus(self) -> None:
        value = {"schema": "frame.command-context/v1", "pane": "0:7"}
        with patch.dict("os.environ", {"FRAME_COMMAND_CONTEXT": json.dumps(value)}):
            self.assertEqual(command_context(), value)
        with patch.dict("os.environ", {"FRAME_COMMAND_CONTEXT": "{}"}):
            with self.assertRaises(ProtocolError):
                command_context()


if __name__ == "__main__":
    unittest.main()
