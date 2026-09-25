from __future__ import annotations

import json
import os
import select
import socket
import struct
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import Event
from typing import Any

from benchmarks.mux_benchmark import attach_frame, connect_blocked_client
from extensions.lemma_client import EVENT as CLIENT_EVENT
from extensions.lemma_client import PROC as PROC_REQUEST
from extensions.lemma_client import Client
from tests.support.mux_harness import LemmaServer, Session, wait_until

MAGIC = b"\x8aLME"
HEADER = struct.Struct(">4sBBBBII")
HELLO = 1
PROC = 3
PROC_RESULT = 4
SURFACE_UPDATE = 5
EVENT = 6


class ExtensionPeer:
    def __init__(self, path: str) -> None:
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(3.0)
        self.socket.connect(path)
        self.input = bytearray()

    def close(self) -> None:
        self.socket.close()

    @staticmethod
    def encode(kind: int, sequence: int, document: dict[str, Any]) -> bytes:
        payload = json.dumps(document, separators=(",", ":")).encode()
        return HEADER.pack(MAGIC, 1, 0, kind, 0, len(payload), sequence) + payload

    def send(self, kind: int, sequence: int, document: dict[str, Any]) -> None:
        self.socket.sendall(self.encode(kind, sequence, document))

    def send_batch(self, records: list[tuple[int, int, dict[str, Any]]]) -> None:
        self.socket.sendall(
            b"".join(
                self.encode(kind, sequence, document)
                for kind, sequence, document in records
            )
        )

    def receive(self) -> tuple[int, int, dict[str, Any]]:
        # Preserve partial records across bounded waits, including handshake-drain probes.
        def fill(size: int) -> None:
            while len(self.input) < size:
                chunk = self.socket.recv(size - len(self.input))
                if not chunk:
                    raise EOFError("extension peer closed while reading a record")
                self.input.extend(chunk)

        fill(HEADER.size)
        magic, major, minor, kind, flags, size, sequence = HEADER.unpack(
            self.input[: HEADER.size]
        )
        if (
            (magic, major, minor, flags) != (MAGIC, 1, 0, 0)
            or not 0 < sequence <= 0xFFFFFFFF
            or size > 1024 * 1024
        ):
            raise AssertionError("invalid extension response header")
        fill(HEADER.size + size)
        payload = bytes(self.input[HEADER.size : HEADER.size + size])
        del self.input[: HEADER.size + size]
        return kind, sequence, json.loads(payload.decode("utf-8"))

    def receive_matching(
        self, kind: int, sequence: int | None = None
    ) -> dict[str, Any]:
        for _ in range(16):
            received_kind, received_sequence, document = self.receive()
            if received_kind == kind and (
                sequence is None or received_sequence == sequence
            ):
                return document
        raise AssertionError(f"did not receive record kind={kind} sequence={sequence}")

    def receive_proc_before_surface_event(
        self, sequence: int, event_name: str, surface: str | None = None
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        result: dict[str, Any] | None = None
        for _ in range(32):
            kind, received_sequence, document = self.receive()
            if kind == PROC_RESULT and received_sequence == sequence:
                result = document
                continue
            if (
                kind == EVENT
                and document.get("event") == event_name
                and (surface is None or document.get("surface") == surface)
            ):
                if result is None:
                    raise AssertionError(f"{event_name} preceded ProcResult {sequence}")
                return result, document
        raise AssertionError(
            f"did not receive ProcResult {sequence} followed by {event_name}"
        )


class ExtensionRuntimeMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)

    def test_python_client_defaults_negotiate_without_a_session(self) -> None:
        with Client(str(self.server.socket_path), name="unscoped-python") as client:
            self.assertEqual(set(client.welcome["capabilities"]), {"observe", "proc"})
            self.assertTrue(client.proc({"command": "session.list"})["ok"])
            self.assertEqual(client.event()["event"], "snapshot")

    def test_signal_observation_reports_latest_pane_attention(self) -> None:
        # Each read gates one phase, so every observed record is a committed latest value.
        script = (
            r"printf '\033]133;A\007$ \033]133;B\007'; read step; "
            r"printf '\033]133;C\007\033]9;4;1;40\033\\'; read step; "
            r"printf '\007\033]777;notify;Agent;needs input\007\033]9;hello nine\007'; "
            r"read step; "
            r"printf '\033]9;4;0\033\\\033]133;D;3\007\033]133;A\007$ \033]133;B\007'; "
            r"read step; "
            r"printf '\033]133;C\007done\r\n\033]133;D;0\007\033]133;A\007$ '; sleep 30"
        )
        session = self.server.create_session(
            "signals", attach=False, hold=True, command=("/bin/sh", "-c", script)
        )
        session_id = session.state().id
        pane = "0:1"

        def enter() -> dict[str, Any]:
            return {
                "command": "pane.input",
                "session": {"id": session_id},
                "pane": {"id": pane},
                "events": [{"kind": "key", "key": "enter"}],
            }

        with (
            Client(
                str(self.server.socket_path),
                name="agent-dashboard",
                session=session_id,
                signals=True,
            ) as dashboard,
            Client(
                str(self.server.socket_path), name="no-signals", session=session_id
            ) as quiet,
        ):
            self.assertEqual(dashboard.event()["event"], "snapshot")
            self.assertEqual(quiet.event()["event"], "snapshot")

            def signal(predicate: Any) -> dict[str, Any]:
                deadline = time.monotonic() + 5.0
                seen: list[dict[str, Any]] = []
                while (remaining := deadline - time.monotonic()) > 0:
                    event = dashboard.event(timeout=remaining)
                    if event["event"] != "pane.signal":
                        continue
                    self.assertEqual(
                        (event["session"], event["pane"]), (session_id, pane)
                    )
                    seen.append(event["signals"])
                    if predicate(event["signals"]):
                        return event["signals"]
                self.fail(f"no matching pane.signal: {seen}\n{self.server.logs()}")

            prompt = signal(lambda value: value["command"] is not None)
            self.assertEqual(prompt["command"], {"state": "prompt", "exit_code": None})
            self.assertEqual(prompt["commands"], 0)
            self.assertIsNone(prompt["notification"])
            self.assertIsNone(prompt["progress"])

            self.assertTrue(dashboard.proc(enter())["ok"])
            running = signal(lambda value: value["progress"] is not None)
            self.assertEqual(
                running["command"], {"state": "running", "exit_code": None}
            )
            self.assertEqual(running["progress"], {"state": "normal", "percent": 40})
            self.assertGreater(running["generation"], prompt["generation"])

            self.assertTrue(dashboard.proc(enter())["ok"])
            waiting = signal(lambda value: value["notifications"] == 2)
            self.assertEqual(waiting["bells"], 1)
            self.assertEqual(
                waiting["notification"],
                {"title": "", "body": "hello nine", "truncated": False},
            )

            self.assertTrue(dashboard.proc(enter())["ok"])
            failed = signal(lambda value: value["commands"] == 1)
            self.assertEqual(failed["command"], {"state": "prompt", "exit_code": 3})
            self.assertIsNone(failed["progress"])

            completed = dashboard.proc(
                enter(),
                {
                    "command": "pane.wait",
                    "session": {"id": session_id},
                    "pane": {"id": pane},
                    "until_command": True,
                    "timeout_ms": 5000,
                },
            )
            self.assertTrue(completed["ok"], completed)
            waited = completed["results"][1]["result"]
            self.assertEqual(waited["condition"], "command")
            self.assertEqual(waited["completion"], {"commands": 2, "exit_code": 0})
            succeeded = signal(lambda value: value["commands"] == 2)
            self.assertEqual(succeeded["command"], {"state": "prompt", "exit_code": 0})

            inspected = dashboard.command(
                "pane.inspect", session={"id": session_id}, pane={"id": pane}
            )["pane_state"]["signals"]
            self.assertEqual(inspected["commands"], 2)
            self.assertEqual(inspected["notification"]["body"], "hello nine")
            listed = dashboard.command("pane.list", session={"id": session_id})
            summary = listed["panes"][0]["signals"]
            self.assertNotIn("notification", summary)
            self.assertEqual(
                {key: summary[key] for key in ("bells", "notifications", "commands")},
                {"bells": 1, "notifications": 2, "commands": 2},
            )

            # Signals are opt-in: the other observer received none of these records.
            events: list[dict[str, Any]] = []
            while True:
                try:
                    events.append(quiet.event(timeout=0.2))
                except TimeoutError:
                    break
            self.assertNotIn("pane.signal", [event["event"] for event in events])

    def test_signals_are_not_starved_by_a_streaming_selected_pane(self) -> None:
        # Both Panes wait for one Proc. The streaming Pane then changes on every drain, and the
        # other signals on its own because control Commands are slow behind a sustained flood.
        session = self.server.create_session(
            "signal-stream",
            attach=False,
            hold=True,
            command=("/bin/sh", "-c", "read go; exec yes streaming"),
        )
        session_id = session.state().id
        with Client(
            str(self.server.socket_path), name="signal-control", session=session_id
        ) as control:
            signaling = control.command(
                "pane.split",
                session={"id": session_id},
                pane={"id": "0:1"},
                direction="right",
                focus="preserve",
                hold=True,
                argv=[
                    "/bin/sh",
                    "-c",
                    r"read go; sleep 1; printf '\007\033]9;attention\007'; sleep 30",
                ],
            )["pane"]
        with Client(
            str(self.server.socket_path),
            name="signal-stream-observer",
            session=session_id,
            panes=("0:1", signaling),
            signals=True,
        ) as observer:
            self.assertEqual(observer.event()["event"], "snapshot")
            observer.send(
                PROC_REQUEST,
                {
                    "schema": "lemma.proc/v1",
                    "commands": [
                        {
                            "command": "pane.input",
                            "session": {"id": session_id},
                            "pane": {"id": pane},
                            "events": [{"kind": "key", "key": "enter"}],
                        }
                        for pane in (signaling, "0:1")
                    ],
                },
            )
            deadline = time.monotonic() + 8.0
            streamed = 0
            signal: dict[str, Any] | None = None
            while signal is None and time.monotonic() < deadline:
                try:
                    record = observer.receive(deadline)
                except TimeoutError:
                    break
                event = record.document
                if record.kind != CLIENT_EVENT:
                    continue
                streamed += event["event"] == "pane.terminal" and event["pane"] == "0:1"
                if (
                    event["event"] == "pane.signal"
                    and event["pane"] == signaling
                    and event["signals"]["notifications"] == 1
                ):
                    signal = event["signals"]
            self.assertIsNotNone(
                signal, f"signal starved behind {streamed} pane.terminal Events"
            )
            assert signal is not None
            self.assertEqual(signal["bells"], 1)
            self.assertGreater(streamed, 0)

    def test_signal_turns_do_not_withhold_other_selected_pane_changes(self) -> None:
        # The first selected Pane changes and rings BEL on every drain; the second has a pending
        # change. Alternating with signals must not skip the second Pane's terminal Events.
        session = self.server.create_session(
            "signal-lockstep",
            attach=False,
            hold=True,
            command=("/bin/sh", "-c", r"read go; exec yes \"$(printf '\007')signal\""),
        )
        session_id = session.state().id
        with Client(
            str(self.server.socket_path), name="lockstep-control", session=session_id
        ) as control:
            quiet = control.command(
                "pane.split",
                session={"id": session_id},
                pane={"id": "0:1"},
                direction="right",
                focus="preserve",
                hold=True,
                argv=[
                    "/bin/sh",
                    "-c",
                    "read go; while :; do echo stream; sleep 0.1; done",
                ],
            )["pane"]
        with Client(
            str(self.server.socket_path),
            name="lockstep-observer",
            session=session_id,
            panes=("0:1", quiet),
            signals=True,
        ) as observer:
            self.assertEqual(observer.event()["event"], "snapshot")
            observer.send(
                PROC_REQUEST,
                {
                    "schema": "lemma.proc/v1",
                    "commands": [
                        {
                            "command": "pane.input",
                            "session": {"id": session_id},
                            "pane": {"id": pane},
                            "events": [{"kind": "key", "key": "enter"}],
                        }
                        for pane in (quiet, "0:1")
                    ],
                },
            )
            required = {
                ("pane.terminal", "0:1"),
                ("pane.terminal", quiet),
                ("pane.signal", "0:1"),
            }
            counts: dict[tuple[str, str], int] = {}
            # Delivery is slow while a Pane floods, so allow a generous bounded deadline.
            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline and not all(
                counts.get(key, 0) >= 2 for key in required
            ):
                try:
                    record = observer.receive(deadline)
                except TimeoutError:
                    break
                if record.kind == CLIENT_EVENT and "pane" in record.document:
                    key = (record.document["event"], record.document["pane"])
                    counts[key] = counts.get(key, 0) + 1
            self.assertTrue(
                all(counts.get(key, 0) >= 2 for key in required), f"delivered: {counts}"
            )

    def test_python_client_negotiates_explicit_session_scoped_surfaces(self) -> None:
        session = self.server.create_session("python-surface", command=("cat",))
        with Client(
            str(self.server.socket_path),
            name="scoped-python",
            session=session.state().id,
            capabilities=("observe", "proc", "surface"),
        ) as client:
            self.assertIn("surface", client.welcome["capabilities"])
            result = client.command(
                "surface.create",
                placement={
                    "kind": "overlay",
                    "column": 0,
                    "row": 0,
                    "columns": 10,
                    "rows": 2,
                },
            )
            self.assertEqual(result["status"], "applied")
            self.assertIn("surface", result)

    def test_documented_hello_negotiates_and_observes_its_session(self) -> None:
        self.server.create_session("example", attach=False, command=("cat",))
        example = Path(__file__).resolve().parents[2] / "examples/extension-hello.json"
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(HELLO, 1, json.loads(example.read_text()))
        welcome = peer.receive_matching(2, 1)
        self.assertEqual(welcome["schema"], "lemma.extension-welcome/v1")
        self.assertEqual(set(welcome["capabilities"]), {"observe", "proc", "surface"})
        self.assertIn("attachment", welcome)
        self.assertEqual(peer.receive_matching(EVENT)["event"], "snapshot")

    def test_independent_surface_update_conformance_corpus(self) -> None:
        session = self.server.create_session(
            "conformance", attach=False, command=("cat",)
        )
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "conformance",
                "capabilities": ["proc", "surface"],
                "events": {
                    "schema": "lemma.events/v1",
                    "session": {"id": session.state().id},
                },
            },
        )
        peer.receive_matching(2, 1)
        peer.send(
            PROC,
            2,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.create",
                        "placement": {
                            "kind": "overlay",
                            "column": 0,
                            "row": 0,
                            "columns": 20,
                            "rows": 2,
                        },
                    }
                ],
            },
        )
        result, _ = peer.receive_proc_before_surface_event(2, "surface.resized")
        surface = result["results"][0]["result"]["surface"]
        corpus = json.loads(
            (
                Path(__file__).parent / "fixtures" / "extension_conformance.json"
            ).read_text()
        )
        for index, case in enumerate(corpus):
            with self.subTest(case=case["name"]):
                sequence = index * 2 + 3
                peer.send(
                    SURFACE_UPDATE,
                    sequence,
                    {
                        "schema": "lemma.surface-update/v1",
                        "surface": surface,
                        **case["patch"],
                    },
                )
                # A following Proc is a record-order barrier, not a sleep or a guessed response end.
                peer.send(
                    PROC,
                    sequence + 1,
                    {
                        "schema": "lemma.proc/v1",
                        "commands": [{"command": "session.list"}],
                    },
                )
                errors = []
                while True:
                    kind, received_sequence, document = peer.receive()
                    if kind == 7:
                        self.assertEqual(received_sequence, sequence)
                        errors.append(document)
                    if kind == PROC_RESULT:
                        self.assertEqual(received_sequence, sequence + 1)
                        self.assertTrue(document["ok"], document)
                        break
                self.assertEqual(not errors, case["accepted"], errors)

    def test_semantic_focus_handoff_is_consistent(self) -> None:
        session = self.server.create_session("focus-handoff", command=("cat",))
        first = session.state()
        self.server.require_command(
            "proc", "tab", "new", "--session", first.id, "--", "cat"
        )
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "focus-handoff",
                "capabilities": ["proc", "surface"],
                "events": {"schema": "lemma.events/v1", "session": {"id": first.id}},
            },
        )
        peer.receive_matching(2, 1)
        peer.send(
            PROC,
            2,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.create",
                        "placement": {
                            "kind": "overlay",
                            "column": 0,
                            "row": 0,
                            "columns": 1,
                            "rows": 1,
                        },
                    }
                ],
            },
        )
        result, _ = peer.receive_proc_before_surface_event(2, "surface.resized")
        surface = result["results"][0]["result"]["surface"]
        sequence = 2
        client = session.require_client()
        client.drain(0.2)

        def focus() -> None:
            nonlocal sequence
            sequence += 1
            peer.send(
                PROC,
                sequence,
                {
                    "schema": "lemma.proc/v1",
                    "commands": [
                        {"command": "surface.focus", "surface": {"id": surface}}
                    ],
                },
            )
            focused = peer.receive_matching(PROC_RESULT, sequence)
            self.assertTrue(focused["ok"], focused)
            # Drain owner focus Events, without requiring a duplicate focused Event on no_effect.
            peer.socket.settimeout(0.05)
            try:
                while True:
                    peer.receive()
            except TimeoutError:
                pass
            peer.socket.settimeout(3.0)

        for command in [
            b"\x021",
            b"\x02n",
            b"\x02p",
            b"\x02:tab select 1\r",
            f"\x02:pane focus {first.focused.id}\r".encode(),
        ]:
            with self.subTest(command=command):
                focus()
                client.send(command)
                client.drain(0.2)
                client.send(b"marker\n")
                client.drain(0.2)
                event = peer.receive_matching(EVENT)
                self.assertEqual(event["event"], "surface.blurred", event)

        for command in [
            (
                "proc",
                "tab",
                "new",
                "--session",
                first.id,
                "--focus",
                "preserve",
                "--",
                "cat",
            ),
            ("proc", "pane", "focus", "--session", first.id, "--pane", "0:999999"),
        ]:
            with self.subTest(proc=command):
                focus()
                result = self.server.command(*command)
                self.assertEqual(
                    result.status == 0, "preserve" in command, result.output
                )
                client.send(b"preserved")
                client.drain(0.1)
                event = peer.receive_matching(EVENT)
                self.assertEqual(
                    (event["event"], event["text"]), ("surface.key", "preserved")
                )
        for command in ["tab new --focus preserve -- cat", "pane focus 0:999999"]:
            with self.subTest(prompt=command):
                focus()
                client.prefix(":")
                client.send(command + "\r")
                client.drain(0.2)
                client.send("prompt-preserved")
                client.drain(0.1)
                event = peer.receive_matching(EVENT)
                self.assertEqual(
                    (event["event"], event["text"]), ("surface.key", "prompt-preserved")
                )

        # A failed native split must not hand off semantic focus just because parsing succeeded.
        focus()
        client.resize(2, 3)
        self.server.wait_for_state(
            session.name, lambda value: value.columns == 2, "tiny focus viewport"
        )
        client.send(b"\x02%rejected")
        event = peer.receive_matching(EVENT)
        self.assertEqual(event["event"], "surface.key", event)
        self.assertEqual(event["text"], "rejected")

    def _focused_overlay_peer(self, session: Session) -> ExtensionPeer:
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "opaque-input",
                "capabilities": ["proc", "surface"],
                "events": {
                    "schema": "lemma.events/v1",
                    "session": {"id": session.state().id},
                },
            },
        )
        peer.receive_matching(2, 1)
        peer.send(
            PROC,
            2,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.create",
                        "placement": {
                            "kind": "overlay",
                            "column": 0,
                            "row": 0,
                            "columns": 2,
                            "rows": 1,
                        },
                    }
                ],
            },
        )
        result, _ = peer.receive_proc_before_surface_event(2, "surface.resized")
        surface = result["results"][0]["result"]["surface"]
        peer.send(
            PROC,
            3,
            {
                "schema": "lemma.proc/v1",
                "commands": [{"command": "surface.focus", "surface": {"id": surface}}],
            },
        )
        peer.receive_proc_before_surface_event(3, "surface.focused")
        return peer

    def test_surface_paste_preserves_opaque_bytes_with_strict_json(self) -> None:
        session = self.server.create_session("opaque-paste", command=("cat",))
        peer = self._focused_overlay_peer(session)
        client = session.require_client()
        client.drain(0.2)
        for data in [
            b"\xff\x00\xc0\xaf\xed\xa0\x80",
            b"\xe2",
            b"\x82\xac",
            "hé🙂".encode(),
            (b'\x00\x01"\\\n' * 4096),
            b"\x00" * (1024 * 1024),
        ]:
            with self.subTest(size=len(data), prefix=data[:8]):
                with ThreadPoolExecutor(max_workers=1) as sender:
                    sent = sender.submit(
                        client.send, b"\x1b[200~" + data + b"\x1b[201~", timeout=5.0
                    )
                    if len(data) == 1024 * 1024:
                        # A briefly descheduled consumer must backpressure, not lose its owner.
                        time.sleep(0.1)
                    received = bytearray()
                    while len(received) < len(data):
                        event = peer.receive_matching(EVENT)
                        if event.get("event") != "surface.paste":
                            continue
                        # Python's strict UTF-8/JSON decoder and bytes.fromhex are independent
                        # of Lemma's parser, serializer, and framing implementation.
                        self.assertNotIn("text", event)
                        chunk = bytes.fromhex(event["bytes_hex"])
                        self.assertLessEqual(len(chunk), 4096)
                        received.extend(chunk)
                    sent.result(timeout=5.0)
                    self.assertEqual(received, data)
        peer.send(
            PROC,
            4,
            {"schema": "lemma.proc/v1", "commands": [{"command": "session.list"}]},
        )
        self.assertTrue(peer.receive_matching(PROC_RESULT, 4)["ok"])

    def test_surface_paste_stall_expires_without_reading_or_closing_the_attachment(
        self,
    ) -> None:
        session = self.server.create_session("stalled-paste", command=("cat",))
        peer = self._focused_overlay_peer(session)
        client = session.require_client()
        client.drain(0.2)
        poller = select.poll()
        poller.register(peer.socket, select.POLLHUP | select.POLLERR)
        with ThreadPoolExecutor(max_workers=1) as sender:
            started = time.monotonic()
            sent = sender.submit(
                client.send,
                b"\x1b[200~" + b"\0" * (1024 * 1024) + b"\x1b[201~",
                timeout=10,
            )
            # Poll hangup without consuming queued bytes: only the reactor's deadline can wake it.
            self.assertTrue(
                poller.poll(8000), "stalled Surface owner was never revoked"
            )
            self.assertGreaterEqual(time.monotonic() - started, 4.5)
            sent.result(timeout=3)
        self.assertTrue(session.state().attached)

    def test_switch_retries_decoder_held_input_after_surface_backpressure(self) -> None:
        source = self.server.create_session(
            "paste-source", attach=False, command=("cat",)
        )
        target = self.server.create_session(
            "paste-target", attach=False, command=("cat",)
        )
        name = source.name.encode()
        raw, minor = connect_blocked_client(
            self.server.socket_path, struct.pack("!BHHB", len(name), 80, 24, 0) + name
        )
        self.addCleanup(raw.close)
        stopped = Event()

        def drain() -> None:
            while not stopped.is_set():
                if select.select([raw], [], [], 0.05)[0]:
                    self.assertTrue(raw.recv(65536), "attachment closed during handoff")

        with ThreadPoolExecutor(max_workers=1) as reader:
            draining = reader.submit(drain)
            try:
                peer = self._focused_overlay_peer(source)
                # Both records fit the expanded decoder. No later socket input wakes the marker.
                raw.sendall(
                    attach_frame(9, b"\0" * (512 * 1024), 2, minor=minor)
                    + attach_frame(2, b"HANDOFF_INPUT\n", 3, minor=minor)
                )
                first = peer.receive_matching(EVENT)
                self.assertEqual(first["event"], "surface.paste")
                time.sleep(0.15)
                with Client(
                    str(self.server.socket_path),
                    name="paste-switch",
                    capabilities=("proc",),
                ) as control:

                    def switch(connection: str, session: Session) -> str:
                        def attempt() -> str | None:
                            result = control.proc(
                                {
                                    "command": "attachment.switch",
                                    "connection": connection,
                                    "session": {"id": session.state().id},
                                }
                            )["results"][0]["result"]
                            if (
                                result["status"] == "conflict"
                                and result["error"]["reason"] == "output_pending"
                            ):
                                return None
                            self.assertEqual(result["status"], "applied", result)
                            return result["connection"]

                        return wait_until("paste handoff", attempt, timeout=2)

                    # The first terminal controller in this isolated daemon has generation 1.
                    switch("0:1", target)
                    peer.send(
                        PROC,
                        4,
                        {
                            "schema": "lemma.proc/v1",
                            "commands": [{"command": "session.list"}],
                        },
                    )
                    received = len(bytes.fromhex(first["bytes_hex"]))
                    while True:
                        kind, sequence, document = peer.receive()
                        if kind == PROC_RESULT and sequence == 4:
                            break
                        if kind == EVENT and document["event"] == "surface.paste":
                            received += len(bytes.fromhex(document["bytes_hex"]))
                    self.assertLess(
                        received, 512 * 1024, "fixture did not establish backpressure"
                    )
                    state = target.state()
                    observed = control.proc(
                        {
                            "command": "pane.wait",
                            "session": {"id": state.id},
                            "pane": {"id": state.focused_pane},
                            "contains": "HANDOFF_INPUT",
                            "timeout_ms": 2000,
                        }
                    )
                    self.assertTrue(observed["ok"], observed)
            finally:
                stopped.set()
                draining.result(timeout=2)

    def test_global_extension_service_is_fair_across_buffered_structural_work(
        self,
    ) -> None:
        # Exercise the full public peer budget without the shipped status observer occupying it.
        self.server.close()
        self.server = LemmaServer.from_environment(
            config_text='require("lemma").extension.set("statusline", false)'
        )
        self.addCleanup(self.server.close)
        session = self.server.create_session("global-fairness", command=("cat",))
        state = session.state()
        client = session.require_client()
        peers: list[ExtensionPeer] = []
        surfaces: list[str] = []
        for index in range(32):
            peer = ExtensionPeer(str(self.server.socket_path))
            peers.append(peer)
            self.addCleanup(peer.close)
            peer.send(
                HELLO,
                1,
                {
                    "schema": "lemma.extension/v1",
                    "name": f"fair-{index}",
                    "capabilities": ["observe", "proc", "surface"],
                    "events": {
                        "schema": "lemma.events/v1",
                        "session": {"id": state.id},
                        "panes": [{"id": state.focused.id}],
                        "screen": True,
                    },
                },
            )
            peer.receive_matching(2, 1)
            peer.receive_matching(EVENT)
            placement = (
                {"kind": "dock.right", "size": 2}
                if index == 0
                else {
                    "kind": "overlay",
                    "column": index % 8,
                    "row": index % 4,
                    "columns": 8,
                    "rows": 1,
                }
            )
            peer.send(
                PROC,
                2,
                {
                    "schema": "lemma.proc/v1",
                    "commands": [
                        {
                            "command": "surface.create",
                            "placement": placement,
                            "opaque": index % 2 == 0,
                        }
                    ],
                },
            )
            result, _ = peer.receive_proc_before_surface_event(2, "surface.resized")
            self.assertTrue(result["ok"], result)
            surfaces.append(result["results"][0]["result"]["surface"])

        for index, (peer, surface) in enumerate(zip(peers, surfaces, strict=True)):
            updates = []
            for sequence in range(10, 14):
                document: dict[str, Any] = {
                    "schema": "lemma.surface-update/v1",
                    "surface": surface,
                    "rows": [
                        {
                            "row": 0,
                            "runs": [{"column": 0, "text": f"{index:02d}-{sequence}"}],
                        }
                    ],
                }
                if index == 0 and sequence == 10:
                    # One near-limit complete record pays the global parse/validation budget and
                    # is rejected without preventing small records on other peers from progressing.
                    document = {
                        "schema": "lemma.surface-update/v1",
                        "surface": surface,
                        "padding": "x" * 900_000,
                    }
                updates.append((SURFACE_UPDATE, sequence, document))
            updates.append(
                (
                    PROC,
                    14,
                    {
                        "schema": "lemma.proc/v1",
                        "commands": [{"command": "session.list"}],
                    },
                )
            )
            peer.send_batch(updates)

        marker = "GLOBAL_FAIRNESS_PTY"
        client.send((marker + "\n").encode())
        session.pane().expect_output(marker)
        for index, peer in enumerate(peers):
            barrier = False
            observed = False
            rejected = False
            for _ in range(64):
                kind, sequence, document = peer.receive()
                barrier = barrier or (
                    kind == PROC_RESULT
                    and sequence == 14
                    and document.get("ok") is True
                )
                rejected = rejected or (kind == 7 and sequence == 10)
                observed = observed or (
                    kind == EVENT
                    and document.get("event") == "pane.screen"
                    and marker in json.dumps(document)
                )
                if barrier and observed and (index != 0 or rejected):
                    break
            self.assertTrue(barrier, f"peer {index} did not cross its Proc barrier")
            self.assertTrue(observed, f"peer {index} did not observe terminal progress")
            self.assertEqual(rejected, index == 0)

        for peer in peers:
            peer.close()

        def restored() -> bool | None:
            panes = self.server.require_command(
                "proc", "pane", "list", "--session", session.name
            )
            pane = json.loads(panes.output)["results"][0]["result"]["panes"][0]
            return True if pane["columns"] == 80 else None

        wait_until("global extension cleanup repair", restored)

    def test_blocked_paste_owner_does_not_starve_other_peers_or_pane(self) -> None:
        session = self.server.create_session("blocked-extension", command=("cat",))
        state = session.state()
        client = session.require_client()
        blocked = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(blocked.close)
        blocked.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "blocked",
                "capabilities": ["proc", "surface"],
                "events": {"schema": "lemma.events/v1", "session": {"id": state.id}},
            },
        )
        blocked.receive_matching(2, 1)
        blocked.send(
            PROC,
            2,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.create",
                        "placement": {"kind": "dock.right", "size": 8},
                    }
                ],
            },
        )
        created, _ = blocked.receive_proc_before_surface_event(2, "surface.resized")
        surface = created["results"][0]["result"]["surface"]
        blocked.send(
            PROC,
            3,
            {
                "schema": "lemma.proc/v1",
                "commands": [{"command": "surface.focus", "surface": {"id": surface}}],
            },
        )
        blocked.receive_proc_before_surface_event(3, "surface.focused")

        others: list[ExtensionPeer] = []
        for index in range(4):
            peer = ExtensionPeer(str(self.server.socket_path))
            others.append(peer)
            self.addCleanup(peer.close)
            peer.send(
                HELLO,
                1,
                {
                    "schema": "lemma.extension/v1",
                    "name": f"other-{index}",
                    "capabilities": ["proc"],
                },
            )
            peer.receive_matching(2, 1)

        data = b"x" * (1024 * 1024)
        with ThreadPoolExecutor(max_workers=1) as sender:
            paste = sender.submit(
                client.send, b"\x1b[200~" + data + b"\x1b[201~", timeout=5.0
            )
            for sequence in range(2, 10):
                for peer in others:
                    peer.send(
                        PROC,
                        sequence,
                        {
                            "schema": "lemma.proc/v1",
                            "commands": [{"command": "session.list"}],
                        },
                    )
                    result = peer.receive_matching(PROC_RESULT, sequence)
                    self.assertTrue(result["ok"], result)
                # Keep the independent physical client writable; only the extension reader is
                # intentionally blocked in this fixture.
                client.drain(0.01)
            paste.result(timeout=5.0)
            client.drain(0.01)

        # The peer has not drained any paste Event. Its kernel socket may still absorb the bounded
        # daemon queue, so disconnect explicitly rather than relying on platform buffer size.
        blocked.close()

        marker = "AFTER_BLOCKED_EXTENSION"
        client.send((marker + "\n").encode())
        session.pane().expect_output(marker)

        def restored() -> bool | None:
            panes = self.server.require_command(
                "proc", "pane", "list", "--session", session.name
            )
            pane = json.loads(panes.output)["results"][0]["result"]["panes"][0]
            return True if pane["columns"] == 80 else None

        wait_until("blocked extension cleanup repair", restored)

    def test_process_exit_and_pane_closure_are_observed(self) -> None:
        self.server.require_command(
            "proc", "session", "start", "process-events", "--hold", "--", "/bin/sh"
        )
        state = self.server.session_state("process-events")
        assert state is not None
        # Keep the Session alive when the selected held Pane is explicitly removed.
        self.server.require_command(
            "proc",
            "pane",
            "split",
            "--session",
            state.id,
            "--pane",
            state.focused.id,
            "--right",
            "--",
            "cat",
        )
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "process-events",
                "capabilities": ["observe"],
                "events": {
                    "schema": "lemma.events/v1",
                    "session": {"id": state.id},
                    "pane": {"id": state.focused.id},
                },
            },
        )
        peer.receive_matching(2, 1)
        peer.receive_matching(EVENT)
        self.server.require_command(
            "proc",
            "pane",
            "input",
            "--session",
            state.id,
            "--pane",
            state.focused.id,
            "--text",
            "exit 7\r",
        )
        while True:
            event = peer.receive_matching(EVENT)
            if (
                event.get("event") == "pane.process"
                and event.get("process", {}).get("state") == "exited"
            ):
                self.assertEqual(event["process"]["code"], 7)
                break
        self.server.require_command(
            "proc", "pane", "kill", "--session", state.id, "--pane", state.focused.id
        )
        while True:
            event = peer.receive_matching(EVENT)
            if event.get("event") == "pane.closed":
                self.assertEqual(event["pane"], state.focused.id)
                break

    def test_later_observed_pane_delivers_without_unrelated_daemon_activity(
        self,
    ) -> None:
        session = self.server.create_session(
            "quiescent", attach=False, command=("cat",)
        )
        first = session.state().focused
        gate = self.server.root / "observation-gate"
        os.mkfifo(gate)
        # A FIFO write wakes only this child. No Proc, client input, or periodic daemon probe
        # supplies extra reactor turns after the one terminal invalidation.
        result = self.server.require_command(
            "proc",
            "pane",
            "split",
            "--session",
            session.name,
            "--pane",
            first.id,
            "--right",
            "--",
            "/bin/sh",
            "-c",
            f'while read -r token; do printf "\\n%s\\n" "$token"; done < "{gate}"',
        )
        second = json.loads(result.output)["results"][0]["result"]["pane"]
        panes = [{"id": first.id}]
        for _ in range(2):
            result = self.server.require_command(
                "proc",
                "pane",
                "split",
                "--session",
                session.name,
                "--pane",
                first.id,
                "--down",
                "--",
                "cat",
            )
            panes.append(
                {"id": json.loads(result.output)["results"][0]["result"]["pane"]}
            )
        panes.append({"id": second})
        writer = os.open(gate, os.O_WRONLY)
        self.addCleanup(os.close, writer)
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "quiescent",
                "capabilities": ["observe"],
                "events": {
                    "schema": "lemma.events/v1",
                    "session": {"id": session.state().id},
                    "panes": panes,
                    "screen": True,
                },
            },
        )
        peer.receive_matching(2, 1)
        peer.receive_matching(EVENT)
        peer.socket.settimeout(1.0)
        try:
            while True:
                peer.receive()
        except TimeoutError:
            pass
        peer.socket.settimeout(0.3)
        for index in range(3):
            marker = f"QUIESCENT_{index}"
            os.write(writer, (marker + "\n").encode())
            while True:
                _, _, event = peer.receive()
                if (
                    event.get("event") == "pane.screen"
                    and event.get("pane") == second
                    and marker in json.dumps(event)
                ):
                    break

    def test_capability_combinations_through_listener(self) -> None:
        session = self.server.create_session(
            "capabilities", attach=False, command=("cat",)
        )
        state = session.state()
        for mask in range(1, 8):
            capabilities = [
                name
                for bit, name in enumerate(["observe", "proc", "surface"])
                if mask & (1 << bit)
            ]
            for scoped in [False, True]:
                with self.subTest(capabilities=capabilities, scoped=scoped):
                    peer = ExtensionPeer(str(self.server.socket_path))
                    try:
                        hello: dict[str, Any] = {
                            "schema": "lemma.extension/v1",
                            "name": "capabilities",
                            "capabilities": capabilities,
                        }
                        if scoped:
                            hello["events"] = {
                                "schema": "lemma.events/v1",
                                "session": {"id": state.id},
                            }
                        peer.send(HELLO, 1, hello)
                        if not scoped and mask != 2:
                            with self.assertRaises(EOFError):
                                peer.receive()
                            continue
                        welcome = peer.receive_matching(2, 1)
                        self.assertEqual(welcome["capabilities"], capabilities)
                        self.assertEqual(
                            {
                                key: welcome["limits"][key]
                                for key in [
                                    "peers",
                                    "surfaces_aggregate",
                                    "input_bytes_aggregate",
                                    "output_bytes_aggregate",
                                    "read_bytes_per_turn",
                                    "record_work_bytes_per_turn",
                                    "records_per_turn",
                                    "write_bytes_per_turn",
                                ]
                            },
                            {
                                "peers": 32,
                                "surfaces_aggregate": 128,
                                "input_bytes_aggregate": 33_554_944,
                                "output_bytes_aggregate": 67_108_864,
                                "read_bytes_per_turn": 262_144,
                                "record_work_bytes_per_turn": 1_048_832,
                                "records_per_turn": 16,
                                "write_bytes_per_turn": 524_288,
                            },
                        )
                        if scoped:
                            self.assertRegex(
                                welcome["attachment"], r"^[0-9]+:[1-9][0-9]*$"
                            )
                        else:
                            self.assertNotIn("attachment", welcome)
                        if "observe" in capabilities:
                            kind, sequence, event = peer.receive()
                            self.assertEqual(
                                (kind, sequence, event["event"]), (EVENT, 1, "snapshot")
                            )
                        if "proc" in capabilities:
                            peer.send(
                                PROC,
                                2,
                                {
                                    "schema": "lemma.proc/v1",
                                    "commands": [{"command": "session.list"}],
                                },
                            )
                            if "observe" in capabilities:
                                result = peer.receive_matching(PROC_RESULT, 2)
                            else:
                                kind, sequence, result = peer.receive()
                                self.assertEqual((kind, sequence), (PROC_RESULT, 2))
                            self.assertTrue(result["ok"], result)
                    finally:
                        peer.close()

    def test_surface_recovery_after_shrink_reaches_real_pty(self) -> None:
        session = self.server.create_session("shrink-surfaces")
        state = session.state()
        client = session.require_client()
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "shrink",
                "capabilities": ["proc", "surface"],
                "events": {"schema": "lemma.events/v1", "session": {"id": state.id}},
            },
        )
        peer.receive_matching(2, 1)
        sequence = 1

        def proc(command: dict[str, Any]) -> dict[str, Any]:
            nonlocal sequence
            sequence += 1
            peer.send(
                PROC, sequence, {"schema": "lemma.proc/v1", "commands": [command]}
            )
            return peer.receive_matching(PROC_RESULT, sequence)

        surfaces = []
        for placement in [
            {"kind": "dock.right", "size": 35},
            {"kind": "dock.left", "size": 35},
            {"kind": "float", "column": 60, "row": 15, "columns": 10, "rows": 2},
        ]:
            result = proc({"command": "surface.create", "placement": placement})
            self.assertTrue(result["ok"], result)
            surfaces.append(result["results"][0]["result"]["surface"])
        self.assertTrue(
            proc({"command": "surface.focus", "surface": {"id": surfaces[2]}})["ok"]
        )
        client.resize(30, 10)
        self.server.wait_for_state(
            session.name,
            lambda value: value.columns == 30 and value.rows == 10,
            "small viewport",
        )
        # Closing one dock must not be rejected because another dock and the float no longer fit.
        closed = proc({"command": "surface.close", "surface": {"id": surfaces[0]}})
        self.assertTrue(closed["ok"], closed)
        repaired = proc(
            {
                "command": "surface.configure",
                "surface": {"id": surfaces[1]},
                "placement": {"kind": "dock.left", "size": 5},
            }
        )
        self.assertTrue(repaired["ok"], repaired)
        rejected = proc(
            {
                "command": "surface.configure",
                "surface": {"id": surfaces[1]},
                "placement": {"kind": "dock.left", "size": 100},
            }
        )
        self.assertFalse(rejected["ok"], rejected)

        def geometry(columns: int, rows: int, marker: str) -> None:
            panes = self.server.require_command(
                "proc", "pane", "list", "--session", session.name
            )
            pane = json.loads(panes.output)["results"][0]["result"]["panes"][0]
            self.assertEqual((pane["columns"], pane["rows"]), (columns, rows))
            # Query the slave PTY, not just Core's published rectangle. Hidden Surface focus must
            # have fallen back natively so physical input reaches this unchanged shell.
            client.send(
                f"m='{marker[:4]}'; printf \"${{m}}{marker[4:]} \"; stty size\r"
            )
            session.pane().expect_output(f"{marker} {rows} {columns}")
            self.assertEqual(pane["process"]["pid"], state.focused.pid)

        geometry(25, 9, "SMALL_PTY")
        client.resize(80, 24)
        self.server.wait_for_state(
            session.name,
            lambda value: value.columns == 80 and value.rows == 24,
            "recovered viewport",
        )
        geometry(75, 23, "LARGE_PTY")
        self.assertTrue(
            proc({"command": "surface.focus", "surface": {"id": surfaces[2]}})["ok"]
        )
        peer.close()

        def restored() -> bool | None:
            panes = self.server.require_command(
                "proc", "pane", "list", "--session", session.name
            )
            pane = json.loads(panes.output)["results"][0]["result"]["panes"][0]
            return True if pane["columns"] == 80 else None

        wait_until("disconnect repair", restored)
        geometry(80, 23, "CLEAN_PTY")

    def test_destroyed_surface_scope_cannot_capture_reused_attachment(self) -> None:
        session = self.server.create_session("old-scope", attach=True, command=("cat",))
        self.server.create_session("keep-daemon", attach=False, command=("cat",))
        original = session.state()
        client = session.require_client()
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "scope-test",
                "capabilities": ["proc", "surface"],
                "events": {"schema": "lemma.events/v1", "session": {"id": original.id}},
            },
        )
        peer.receive_matching(2, 1)
        peer.send(
            PROC,
            2,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.create",
                        "placement": {"kind": "dock.right", "size": 20},
                    }
                ],
            },
        )
        created, _ = peer.receive_proc_before_surface_event(2, "surface.resized")
        surface = created["results"][0]["result"]["surface"]
        peer.send(
            PROC,
            3,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.focus",
                        "surface": {"id": surface},
                    }
                ],
            },
        )
        peer.receive_proc_before_surface_event(3, "surface.focused", surface)
        client.drain(0.2)
        client.send(b"\x1b[<0;70;2M")
        mouse = peer.receive_matching(EVENT)
        self.assertEqual(mouse["event"], "surface.mouse")
        session.destroy()
        replacement = self.server.create_session(
            "new-scope", attach=True, command=("cat",)
        )
        state = replacement.state()
        self.assertEqual(original.id.split(":")[0], state.id.split(":")[0])
        self.assertNotEqual(original.id, state.id)
        fresh = replacement.require_client()
        fresh.send("new-key\n")
        fresh.send(b"\x1b[200~new-paste\n\x1b[201~")
        fresh.send(b"\x1b[<0;70;2m")
        fresh.expect_output("new-key")
        fresh.expect_output("new-paste")
        # All owned resources are revoked, even though observe was never granted.
        with self.assertRaises(EOFError):
            peer.receive()
        panes = self.server.command(
            "proc", "pane", "list", "--session", replacement.name
        )
        pane = json.loads(panes.output)["results"][0]["result"]["panes"][0]
        self.assertEqual((pane["columns"], pane["rows"]), (80, 23))

    def test_buffered_updates_progress_after_service_budget(self) -> None:
        session = self.server.create_session(
            "extension-buffered", attach=True, command=("cat",)
        )
        client = self.server.clients[0]
        state = session.state()
        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "buffered",
                "capabilities": ["proc", "surface"],
                "events": {"schema": "lemma.events/v1", "session": {"id": state.id}},
            },
        )
        peer.receive_matching(2, 1)
        peer.send(
            PROC,
            2,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.create",
                        "placement": {
                            "kind": "overlay",
                            "column": 0,
                            "row": 0,
                            "columns": 40,
                            "rows": 8,
                        },
                    }
                ],
            },
        )
        created = peer.receive_matching(PROC_RESULT, 2)
        self.assertTrue(created["ok"], created)
        surface = created["results"][0]["result"]["surface"]
        records = bytearray()
        # More than one native read, with many complete records left after the
        # four-record peer budget. Dependent row patches must all make progress.
        for index in range(512):
            payload = json.dumps(
                {
                    "schema": "lemma.surface-update/v1",
                    "surface": surface,
                    "rows": [
                        {
                            "row": index % 8,
                            "runs": [
                                {
                                    "column": 0,
                                    "text": f"row-{index % 8}-update-{index:04d}",
                                }
                            ],
                        }
                    ],
                },
                separators=(",", ":"),
            ).encode()
            records.extend(
                HEADER.pack(MAGIC, 1, 0, SURFACE_UPDATE, 0, len(payload), index + 3)
            )
            records.extend(payload)
        peer.socket.sendall(records)
        for index in range(504, 512):
            client.expect_output(f"row-{index % 8}-update-{index:04d}")
        client.send("\n" * 10 + "native-progress\n")
        client.expect_output("native-progress")

    def test_docked_surface_input_proc_and_generation_cleanup(self) -> None:
        session = self.server.create_session(
            "extension-runtime", attach=True, command=("cat",)
        )
        client = self.server.clients[0]
        initial = self.server.session_state(session.name)
        assert initial is not None
        pane = initial.focused

        peer = ExtensionPeer(str(self.server.socket_path))
        self.addCleanup(peer.close)
        peer.send(
            HELLO,
            1,
            {
                "schema": "lemma.extension/v1",
                "name": "runtime-test",
                "capabilities": ["observe", "proc", "surface"],
                "events": {
                    "schema": "lemma.events/v1",
                    "session": {"id": initial.id},
                },
            },
        )
        welcome_kind, welcome_sequence, welcome = peer.receive()
        self.assertEqual((welcome_kind, welcome_sequence), (2, 1))
        self.assertEqual(welcome["schema"], "lemma.extension-welcome/v1")
        snapshot_kind, snapshot_sequence, snapshot = peer.receive()
        self.assertEqual((snapshot_kind, snapshot_sequence), (EVENT, 1))
        self.assertEqual(snapshot["event"], "snapshot")

        peer.send(
            PROC,
            2,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.create",
                        "placement": {"kind": "dock.right", "size": 20},
                    }
                ],
            },
        )
        created, resized = peer.receive_proc_before_surface_event(2, "surface.resized")
        surface = created["results"][0]["result"]["surface"]
        self.assertTrue(created["ok"])
        self.assertEqual(resized["surface"], surface)
        self.assertEqual((resized["columns"], resized["rows"]), (20, 23))

        peer.send(
            SURFACE_UPDATE,
            3,
            {
                "schema": "lemma.surface-update/v1",
                "surface": surface,
                "rows": [
                    {
                        "row": 0,
                        "runs": [
                            {"column": 0, "text": "extension surface", "style": 0}
                        ],
                    }
                ],
            },
        )
        client.expect_output("extension surface")

        peer.send(
            PROC,
            4,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "surface.configure",
                        "surface": {"id": surface},
                        "placement": {"kind": "dock.right", "size": 18},
                    }
                ],
            },
        )
        configured, configured_event = peer.receive_proc_before_surface_event(
            4, "surface.resized", surface
        )
        self.assertTrue(configured["ok"])
        self.assertEqual(
            (configured_event["columns"], configured_event["rows"]), (18, 23)
        )

        peer.send(
            PROC,
            5,
            {
                "schema": "lemma.proc/v1",
                "commands": [{"command": "surface.focus", "surface": {"id": surface}}],
            },
        )
        focused, focused_event = peer.receive_proc_before_surface_event(
            5, "surface.focused", surface
        )
        self.assertTrue(focused["ok"])
        self.assertEqual(focused_event["surface"], surface)
        client.drain(0.2)
        client.send("XYZ")
        client.drain(0.2)
        key_event = peer.receive_matching(EVENT)
        while key_event.get("event") != "surface.key":
            key_event = peer.receive_matching(EVENT)
        self.assertEqual(key_event["surface"], surface)
        self.assertEqual(key_event["text"], "XYZ")

        client.send(b"\x1b[<0;70;2M")
        mouse_press = peer.receive_matching(EVENT)
        while mouse_press.get("event") != "surface.mouse":
            mouse_press = peer.receive_matching(EVENT)
        self.assertEqual(mouse_press["surface"], surface)
        self.assertEqual((mouse_press["column"], mouse_press["row"]), (7, 0))
        client.send(b"\x1b[<0;10;2m")
        mouse_release = peer.receive_matching(EVENT)
        while mouse_release.get("event") != "surface.mouse":
            mouse_release = peer.receive_matching(EVENT)
        self.assertEqual(mouse_release["surface"], surface)
        self.assertEqual((mouse_release["column"], mouse_release["row"]), (-53, 0))

        client.send(b"\x1b[200~pasted\x1b[201~")
        paste_event = peer.receive_matching(EVENT)
        while paste_event.get("event") != "surface.paste":
            paste_event = peer.receive_matching(EVENT)
        self.assertEqual(paste_event["surface"], surface)
        self.assertEqual(bytes.fromhex(paste_event["bytes_hex"]), b"pasted")

        peer.send(
            PROC,
            6,
            {
                "schema": "lemma.proc/v1",
                "commands": [
                    {
                        "command": "pane.send",
                        "session": {"id": initial.id},
                        "pane": {"id": pane.id},
                        "text": "P",
                    }
                ],
            },
        )
        pane_proc = peer.receive_matching(PROC_RESULT, 6)
        self.assertTrue(pane_proc["ok"])

        # The semantic Attachment survives ordinary detach and Session switching;
        # its Surface belongs to that Session, not to the transferred connection.
        session.detach()
        client = session.attach()
        client.drain(0.2)
        client.send("reattached")
        reattached = peer.receive_matching(EVENT)
        while reattached.get("event") != "surface.key":
            reattached = peer.receive_matching(EVENT)
        self.assertEqual(reattached["surface"], surface)
        self.assertEqual(reattached["text"], "reattached")

        target = self.server.create_session(
            "switch-target", attach=False, command=("cat",)
        )
        client.prefix(":")
        client.send("switch switch-target\r")
        self.server.wait_for_state(
            target.name, lambda value: value.attached, "switch to target"
        )
        client.send("target-input\n")
        client.expect_output("target-input")
        client.prefix(":")
        client.send(f"switch {session.name}\r")
        self.server.wait_for_state(
            session.name, lambda value: value.attached, "switch back"
        )
        client.drain(0.2)
        client.send("returned")
        returned = peer.receive_matching(EVENT)
        while returned.get("event") != "surface.key":
            returned = peer.receive_matching(EVENT)
        self.assertEqual(returned["surface"], surface)
        self.assertEqual(returned["text"], "returned")
        peer.close()

        def restored() -> bool | None:
            result = self.server.command(
                "proc", "pane", "list", "--session", session.name
            )
            if result.status != 0:
                return None
            document = json.loads(result.output)["results"][0]["result"]
            current = document["panes"][0]
            return (
                True
                if current["columns"] == 80
                and current["rows"] == 23
                and current["process"]["pid"] == pane.pid
                else None
            )

        wait_until(
            "extension disconnect to restore pane viewport",
            restored,
            diagnostics=lambda: self.server.diagnostics(session.name),
        )
        client.send("alive")
        client.expect_output("alive")


if __name__ == "__main__":
    unittest.main()
