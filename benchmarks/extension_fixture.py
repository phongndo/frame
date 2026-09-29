#!/usr/bin/env python3
"""External language-neutral extension workloads for the process benchmark harness."""

from __future__ import annotations

import argparse
import json
import select
import signal
import socket
import struct
import time
from dataclasses import dataclass
from typing import Any

MAGIC = b"\x8aFME"
HEADER = struct.Struct(">4sBBBBII")
HELLO = 1
WELCOME = 2
PROC = 3
PROC_RESULT = 4
SURFACE_UPDATE = 5
EVENT = 6


class Peer:
    def __init__(self, path: str) -> None:
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(5.0)
        self.socket.connect(path)
        self.input = bytearray()
        self.sequence = 1
        self.surfaces: list[str] = []

    def send(self, kind: int, sequence: int, document: dict[str, Any]) -> None:
        payload = json.dumps(document, separators=(",", ":")).encode()
        self.socket.sendall(
            HEADER.pack(MAGIC, 1, 0, kind, 0, len(payload), sequence) + payload
        )

    def receive(self) -> tuple[int, int, dict[str, Any]]:
        def fill(size: int) -> None:
            while len(self.input) < size:
                chunk = self.socket.recv(size - len(self.input))
                if not chunk:
                    raise EOFError("extension fixture peer closed")
                self.input.extend(chunk)

        fill(HEADER.size)
        magic, major, minor, kind, flags, size, sequence = HEADER.unpack(
            self.input[: HEADER.size]
        )
        if (magic, major, minor, flags) != (MAGIC, 1, 0, 0) or size > 1024 * 1024:
            raise RuntimeError("invalid extension fixture response")
        fill(HEADER.size + size)
        payload = bytes(self.input[HEADER.size : HEADER.size + size])
        del self.input[: HEADER.size + size]
        return kind, sequence, json.loads(payload)

    def receive_matching(self, kind: int, sequence: int) -> dict[str, Any]:
        for _ in range(128):
            received_kind, received_sequence, document = self.receive()
            if received_kind == kind and received_sequence == sequence:
                return document
        raise RuntimeError(f"missing extension record kind={kind} sequence={sequence}")

    def close(self) -> None:
        self.socket.close()


@dataclass
class Fixture:
    peers: list[Peer]
    update_surfaces: list[tuple[Peer, str]]
    blocked: Peer | None = None
    slow: Peer | None = None
    slow_remaining: int = 0

    def close(self) -> None:
        for peer in self.peers:
            peer.close()


def admit(path: str, name: str, session: str | None, capabilities: list[str]) -> Peer:
    peer = Peer(path)
    hello: dict[str, Any] = {
        "schema": "frame.extension/v1",
        "name": name,
        "capabilities": capabilities,
    }
    if session is not None:
        hello["events"] = {
            "schema": "frame.events/v1",
            "session": {"name": session},
        }
    peer.send(HELLO, 1, hello)
    welcome = peer.receive_matching(WELCOME, 1)
    if welcome.get("schema") != "frame.extension-welcome/v1":
        raise RuntimeError("extension fixture did not receive Welcome")
    peer.sequence = 2
    return peer


def proc(peer: Peer, commands: list[dict[str, Any]]) -> dict[str, Any]:
    sequence = peer.sequence
    peer.sequence += 1
    peer.send(PROC, sequence, {"schema": "frame.proc/v1", "commands": commands})
    result = peer.receive_matching(PROC_RESULT, sequence)
    if result.get("ok") is not True:
        raise RuntimeError(f"extension fixture Proc failed: {result}")
    return result


def create_surfaces(peer: Peer, count: int, *, docked: bool = False) -> None:
    commands: list[dict[str, Any]] = []
    for index in range(count):
        placement = (
            {"kind": "dock.right", "size": 2}
            if docked and index == 0
            else {
                "kind": "overlay",
                "column": index % 8,
                "row": index % 4,
                "columns": 24,
                "rows": 4,
            }
        )
        commands.append(
            {
                "command": "surface.create",
                "placement": placement,
                "opaque": docked or index % 3 != 0,
            }
        )
    result = proc(peer, commands)
    peer.surfaces = [entry["result"]["surface"] for entry in result["results"]]
    for index, surface in enumerate(peer.surfaces):
        peer.send(
            SURFACE_UPDATE,
            peer.sequence,
            {
                "schema": "frame.surface-update/v1",
                "surface": surface,
                "rows": [
                    {
                        "row": 0,
                        "runs": [{"column": 0, "text": f"fixture-{index:02d}"}],
                    }
                ],
            },
        )
        peer.sequence += 1
    # A Proc is the ordering barrier for the unacknowledged SurfaceUpdates.
    proc(peer, [{"command": "session.list"}])


def setup(arguments: argparse.Namespace) -> Fixture:
    peers: list[Peer] = []
    updates: list[tuple[Peer, str]] = []
    blocked: Peer | None = None
    if arguments.mode == "idle-peers":
        for index in range(arguments.peers):
            peers.append(
                admit(
                    arguments.socket,
                    f"{arguments.name_prefix}idle-{index}",
                    None,
                    ["proc"],
                )
            )
        return Fixture(peers, updates)
    if arguments.mode == "slow-producer":
        slow = admit(
            arguments.socket,
            f"{arguments.name_prefix}slow",
            None,
            ["proc"],
        )
        peers.append(slow)
        payload_bytes = 1024 * 1024
        slow_tail_bytes = 1024
        initial_payload = b"{" + (b" " * (payload_bytes - slow_tail_bytes - 1))
        slow.socket.sendall(
            HEADER.pack(MAGIC, 1, 0, PROC, 0, payload_bytes, slow.sequence)
            + initial_payload
        )
        return Fixture(
            peers,
            updates,
            slow=slow,
            slow_remaining=slow_tail_bytes,
        )

    if arguments.session is None:
        raise RuntimeError("surface fixture requires --session")
    peer_count = 1 if arguments.mode.startswith("crash-") else arguments.peers
    if arguments.mode == "changing-rows":
        peer_count = min(peer_count, 2)
    elif arguments.mode == "storm":
        peer_count = min(peer_count, 4)
    for index in range(peer_count):
        peer = admit(
            arguments.socket,
            f"{arguments.name_prefix}{arguments.mode}-{index}",
            arguments.session,
            ["proc", "surface"],
        )
        peers.append(peer)
        if arguments.mode == "blocked-reader":
            if index == 0:
                create_surfaces(peer, 1, docked=True)
                proc(
                    peer,
                    [
                        {
                            "command": "surface.focus",
                            "surface": {"id": peer.surfaces[0]},
                        }
                    ],
                )
                blocked = peer
            continue
        surface_count = 1 if arguments.mode.startswith("crash-") else arguments.surfaces
        if arguments.mode in {"changing-rows", "storm"}:
            surface_count = 1
        create_surfaces(peer, surface_count, docked=arguments.mode.startswith("crash-"))
        updates.extend((peer, surface) for surface in peer.surfaces)
    if arguments.mode == "crash-focused":
        proc(
            peers[0],
            [
                {
                    "command": "surface.focus",
                    "surface": {"id": peers[0].surfaces[0]},
                }
            ],
        )
    return Fixture(peers, updates, blocked=blocked)


def drain(peers: list[Peer], blocked: Peer | None, timeout: float = 0.05) -> None:
    candidates = [peer for peer in peers if peer is not blocked]
    if not candidates:
        time.sleep(0.05)
        return
    readable, _, _ = select.select(
        [peer.socket for peer in candidates], [], [], timeout
    )
    by_descriptor = {peer.socket.fileno(): peer for peer in candidates}
    for descriptor in readable:
        peer = by_descriptor[descriptor.fileno()]
        try:
            peer.receive()
        except (EOFError, OSError):
            pass


def run(arguments: argparse.Namespace) -> int:
    fixture = setup(arguments)
    print(
        json.dumps(
            {
                "status": "ready",
                "mode": arguments.mode,
                "peers": len(fixture.peers),
                "surfaces": len(fixture.update_surfaces)
                + (1 if fixture.blocked is not None else 0),
            },
            separators=(",", ":"),
        ),
        flush=True,
    )
    stopped = False

    def stop(_signum: int, _frame: object) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    generation = 0
    proc_completions = 0
    try:
        while not stopped:
            if arguments.mode == "slow-producer":
                if fixture.slow is not None and fixture.slow_remaining > 0:
                    fixture.slow.socket.sendall(b" ")
                    fixture.slow_remaining -= 1
                time.sleep(0.05)
            elif arguments.mode == "blocked-reader":
                for peer in fixture.peers:
                    if peer is fixture.blocked:
                        continue
                    proc(peer, [{"command": "session.list"}])
                    proc_completions += 1
            elif arguments.mode in {"changing-rows", "storm"}:
                generation += 1
                for peer, surface in fixture.update_surfaces:
                    peer.send(
                        SURFACE_UPDATE,
                        peer.sequence,
                        {
                            "schema": "frame.surface-update/v1",
                            "surface": surface,
                            "rows": [
                                {
                                    "row": generation % 4,
                                    "runs": [
                                        {
                                            "column": 0,
                                            "text": f"change-{generation:08d}",
                                        }
                                    ],
                                }
                            ],
                        },
                    )
                    peer.sequence += 1
                drain(fixture.peers, fixture.blocked)
                time.sleep(0.02)
            else:
                drain(fixture.peers, fixture.blocked, timeout=1.0)
    finally:
        print(
            json.dumps(
                {
                    "status": "stopped",
                    "mode": arguments.mode,
                    "surface_updates": generation * len(fixture.update_surfaces),
                    "proc_completions": proc_completions,
                },
                separators=(",", ":"),
            ),
            flush=True,
        )
        fixture.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--mode",
        required=True,
        choices=(
            "idle-peers",
            "idle-surfaces",
            "changing-rows",
            "storm",
            "blocked-reader",
            "slow-producer",
            "crash-focused",
            "crash-docked",
        ),
    )
    parser.add_argument("--socket", required=True)
    parser.add_argument("--session")
    parser.add_argument("--name-prefix", default="")
    parser.add_argument("--peers", type=int, default=8)
    parser.add_argument("--surfaces", type=int, default=8)
    arguments = parser.parse_args()
    if not 1 <= arguments.peers <= 32 or not 1 <= arguments.surfaces <= 16:
        parser.error("peer and Surface counts exceed extension limits")
    return run(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
