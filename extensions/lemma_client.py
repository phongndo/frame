"""Small synchronous client for Lemma's public extension interface.

No daemon imports, event polling timer, automatic reconnect, or mutation replay. A
new Client is a new owner: reconnect from a fresh snapshot and recreate Surfaces.
Use a separate connection for slow catalogue requests if input must remain live.
"""

from __future__ import annotations

import json
import os
import socket
import struct
import time
from collections import deque
from dataclasses import dataclass
from typing import Any

HEADER = struct.Struct(">4sBBBBII")
MAGIC = b"\x8aLME"
HELLO, WELCOME, PROC, RESULT, UPDATE, EVENT, ERROR = range(1, 8)
MAX_RECORD = 1024 * 1024
MAX_EVENTS = 64


class ProtocolError(RuntimeError):
    """Framing, ordering, or resource failure. Discard this connection."""


class Rejected(RuntimeError):
    """A correlated protocol Error, not an executed Proc result."""

    def __init__(self, sequence: int, document: dict[str, Any]) -> None:
        super().__init__(f"request {sequence} rejected: {document}")
        self.sequence = sequence
        self.document = document


@dataclass(frozen=True)
class Record:
    kind: int
    sequence: int
    document: dict[str, Any]


def command_context() -> dict[str, Any]:
    """Read the daemon-captured invocation context, never infer current focus."""
    context = json.loads(os.environ["LEMMA_COMMAND_CONTEXT"])
    if (
        not isinstance(context, dict)
        or context.get("schema") != "lemma.command-context/v1"
    ):
        raise ProtocolError("invalid command context")
    return context


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ProtocolError("duplicate JSON field")
        result[key] = value
    return result


def _invalid_constant(value: str) -> Any:
    raise ProtocolError(f"invalid JSON constant: {value}")


class Client:
    """One bounded framed connection. Not thread-safe.

    request/proc retain interleaved Events under a fixed limit. All I/O in a
    request shares one total deadline, including partial reads and writes. Idle
    event() blocks without periodic wakeups. Timeout or transport failure makes
    a mutation's outcome unknown; callers must not blindly retry it.
    """

    def __init__(
        self,
        endpoint: str,
        *,
        name: str,
        session: str | None = None,
        capabilities: tuple[str, ...] = ("observe", "proc"),
        presentation: bool = False,
        signals: bool = False,
        timeout: float = 3.0,
    ) -> None:
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        if "surface" in capabilities and session is None:
            raise ValueError("surface capability requires a session")
        self.timeout = timeout
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.input = bytearray()
        self.sequence = 0
        self.events: deque[dict[str, Any]] = deque()
        self.record_limit = MAX_RECORD
        self.pending: int | None = None
        self.closed = False
        hello: dict[str, Any] = {
            "schema": "lemma.extension/v1",
            "name": name,
            "capabilities": capabilities,
        }
        if session is not None or "observe" in capabilities:
            subscription: dict[str, Any] = {"schema": "lemma.events/v1"}
            if session is not None:
                subscription["session"] = {"id": session}
            if presentation:
                subscription["presentation"] = True
            if signals:
                subscription["signals"] = True
            hello["events"] = subscription
        try:
            deadline = time.monotonic() + timeout
            self._timeout(deadline)
            self.socket.connect(endpoint)
            self.welcome = self.request(HELLO, WELCOME, hello, deadline=deadline)
            limit = self.welcome.get("limits", {}).get("record_bytes")
            if type(limit) is not int or not 0 < limit <= MAX_RECORD:
                raise ProtocolError("invalid negotiated record limit")
            self.record_limit = limit
        except BaseException:
            # Failed or interrupted negotiation owns no usable connection. Release its socket
            # and propagate the original transport, protocol, or cancellation failure.
            self.close()
            raise

    def __enter__(self) -> Client:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        self.closed = True
        self.socket.close()

    def _timeout(self, deadline: float | None) -> None:
        if self.closed:
            raise EOFError("extension connection is closed")
        remaining = None if deadline is None else deadline - time.monotonic()
        if remaining is not None and remaining <= 0:
            raise TimeoutError("extension deadline expired")
        self.socket.settimeout(remaining)

    def send(
        self, kind: int, document: dict[str, Any], *, deadline: float | None = None
    ) -> int:
        if kind not in (HELLO, PROC, UPDATE):
            raise ValueError("invalid request kind")
        if self.pending is not None and kind == PROC:
            raise ProtocolError("only one Proc may be outstanding")
        payload = json.dumps(document, separators=(",", ":"), allow_nan=False).encode()
        if len(payload) > self.record_limit or self.sequence == 0xFFFFFFFF:
            raise ProtocolError("extension record limit exceeded")
        self.sequence += 1
        try:
            self._timeout(
                deadline if deadline is not None else time.monotonic() + self.timeout
            )
            self.socket.sendall(
                HEADER.pack(MAGIC, 1, 0, kind, 0, len(payload), self.sequence) + payload
            )
        except (OSError, EOFError):
            self.close()
            raise
        if kind == PROC:
            self.pending = self.sequence
        return self.sequence

    def receive(self, deadline: float | None = None) -> Record:
        """Read one record. A timeout preserves any incomplete input record."""

        def fill(size: int) -> None:
            while len(self.input) < size:
                self._timeout(deadline)
                data = self.socket.recv(size - len(self.input))
                if not data:
                    raise EOFError("extension connection closed")
                self.input.extend(data)

        try:
            fill(HEADER.size)
            magic, major, minor, kind, flags, size, sequence = HEADER.unpack(
                self.input[: HEADER.size]
            )
            if (
                (magic, major, minor, flags) != (MAGIC, 1, 0, 0)
                or kind not in (WELCOME, RESULT, EVENT, ERROR)
                or sequence == 0
                or size > self.record_limit
            ):
                raise ProtocolError("invalid extension response header")
            fill(HEADER.size + size)
            document = json.loads(
                self.input[HEADER.size : HEADER.size + size],
                object_pairs_hook=_unique_object,
                parse_constant=_invalid_constant,
            )
            del self.input[: HEADER.size + size]
            if not isinstance(document, dict):
                raise ProtocolError("response is not an object")
            if kind == RESULT:
                if sequence != self.pending:
                    raise ProtocolError("unexpected Proc result")
                self.pending = None
            if kind == ERROR and sequence == self.pending:
                self.pending = None
            return Record(kind, sequence, document)
        except TimeoutError:
            raise
        except (OSError, EOFError, ValueError, RecursionError, ProtocolError):
            self.close()
            raise

    def request(
        self,
        kind: int,
        expected: int,
        document: dict[str, Any],
        *,
        deadline: float | None = None,
    ) -> dict[str, Any]:
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        sequence = self.send(kind, document, deadline=deadline)
        try:
            while True:
                record = self.receive(deadline)
                if record.kind == ERROR:
                    raise Rejected(record.sequence, record.document)
                if record.kind == expected and record.sequence == sequence:
                    return record.document
                if record.kind != EVENT or len(self.events) == MAX_EVENTS:
                    raise ProtocolError(
                        "unexpected response or event capacity exceeded"
                    )
                self.events.append(record.document)
        except (TimeoutError, ProtocolError, Rejected):
            # Abandoning a request must not leave an ambiguous result for a future request.
            self.close()
            raise

    def proc(self, *commands: dict[str, Any], on_error: str = "stop") -> dict[str, Any]:
        """Execute an ordered, non-atomic Proc. Inspect every nested result."""
        return self.request(
            PROC,
            RESULT,
            {"schema": "lemma.proc/v1", "commands": commands, "on_error": on_error},
        )

    def command(self, command: str, **fields: Any) -> dict[str, Any]:
        result = self.proc({"command": command, **fields})
        if not result["ok"]:
            raise RuntimeError(f"{command} failed: {result}")
        return result["results"][0]["result"]

    def update(self, surface: str, **content: Any) -> int:
        """Submit retained content; acceptance has no acknowledgement.

        Await a successful create/configure Proc before sending dependent content.
        A later Error correlates to the returned sequence; it is not a ProcResult.
        """
        return self.send(
            UPDATE,
            {"schema": "lemma.surface-update/v1", "surface": surface, **content},
        )

    def event(self, timeout: float | None = None) -> dict[str, Any]:
        if self.pending is not None:
            raise ProtocolError("consume the outstanding Proc result first")
        if self.events:
            return self.events.popleft()
        record = self.receive(None if timeout is None else time.monotonic() + timeout)
        if record.kind == ERROR:
            raise Rejected(record.sequence, record.document)
        if record.kind != EVENT:
            self.close()
            raise ProtocolError("unexpected response while waiting for an Event")
        return record.document
