from __future__ import annotations

import json
import os
import select
import socket
import subprocess
import time
import unittest
from pathlib import Path
from typing import Any

from tests.support.mux_harness import LemmaServer


class AgentInterfaceMuxTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)

    def json_command(
        self, *arguments: str, timeout: float = 5.0, unwrap: bool = True
    ) -> tuple[int, dict[str, Any]]:
        result = self.server.command(*arguments, timeout=timeout)
        try:
            document = json.loads(result.output)
        except json.JSONDecodeError as error:
            self.fail(
                f"command {result.arguments!r} returned invalid JSON with status "
                f"{result.status}:\n{result.output}\n{self.server.logs()}\n{error}"
            )
        if unwrap and document.get("schema") == "lemma.proc-result/v1":
            results = document.get("results", [])
            if results:
                return result.status, results[0]["result"]
        return result.status, document

    def first_event(self, *arguments: str) -> dict[str, Any]:
        process = subprocess.Popen(
            [str(self.server.cli_path), str(self.server.socket_path), *arguments],
            env=self.server.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert process.stdout is not None
        ready, _, _ = select.select([process.stdout], [], [], 2.0)
        if not ready:
            process.kill()
            stdout, stderr = process.communicate(timeout=1.0)
            self.fail(
                f"event stream produced no initial snapshot: stdout={stdout!r} stderr={stderr!r}\n"
                f"{self.server.logs()}"
            )
        line = process.stdout.readline()
        process.kill()
        process.communicate(timeout=1.0)
        return json.loads(line)

    def test_root_help_prioritizes_daily_commands(self) -> None:
        help_result = self.server.command("--help")
        self.assertEqual(help_result.status, 0, help_result.output)
        self.assertIn("Usage:\n  lemma [command] [options]\n", help_result.output)
        self.assertEqual(
            [line for line in help_result.output.splitlines() if line.endswith(":")],
            ["Usage:", "Basic:", "Resources:", "Automation:", "Other:"],
        )
        basic = help_result.output.split("Basic:\n", 1)[1].split("\nResources:", 1)[0]
        self.assertEqual(
            [line.split()[0] for line in basic.splitlines() if line.strip()],
            [
                "new",
                "start",
                "attach",
                "ls,",
                "split",
                "send",
                "paste-image",
                "wait",
                "capture",
                "focus",
                "zoom",
                "swap",
                "resize",
                "rename",
                "kill",
            ],
        )
        self.assertIn("Send text or key presses to a pane's running program", basic)
        self.assertIn("Print text from a pane's screen or scrollback", basic)
        self.assertIn("Running lemma without a command creates", help_result.output)
        self.assertNotIn("lemma inspect", help_result.output)

        removed = self.server.command("inspect", "missing")
        self.assertEqual(removed.status, 2, removed.output)
        self.assertIn("invalid lemma command or arguments: inspect", removed.output)

    def public_command(
        self, *arguments: str, environment: dict[str, str] | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.server.cli_path), str(self.server.socket_path), *arguments],
            env=self.server.environment if environment is None else environment,
            capture_output=True,
            text=True,
            timeout=5.0,
            check=False,
        )

    def test_basic_help_explains_behavior_and_uses_the_invoked_spelling(self) -> None:
        for name in (
            "split",
            "send",
            "wait",
            "capture",
            "focus",
            "zoom",
            "swap",
            "resize",
        ):
            with self.subTest(command=name):
                result = self.server.command(name, "--help")
                self.assertEqual(result.status, 0, result.output)
                self.assertIn(f"Usage:\n  lemma {name} ", result.output)
                self.assertIn("Example", result.output)
                self.assertIn("--json", result.output)
                self.assertIn("Outside Lemma, provide --session", result.output)
                self.assertNotIn("lemma proc pane", result.output)
                self.assertEqual(
                    self.server.command("help", name).output, result.output
                )
        send = self.server.command("send", "--help").output
        self.assertIn("launch a process directly or press Enter automatically", send)
        self.assertIn("lemma send --paste 'just test' --key enter", send)
        capture = self.server.command("capture", "--help").output
        self.assertIn("text, not an image screenshot", capture)
        wait = self.server.command("wait", "--help").output
        self.assertIn("any process exit succeeds", wait)
        for name in ("new", "start", "attach", "rename", "kill", "ls"):
            self.assertIn("Usage:", self.server.command(name, "--help").output)
        self.assertIn("--cwd", self.server.command("start", "--help").output)
        self.assertIn(
            "lemma pane capture", self.server.command("help", "pane", "capture").output
        )
        self.assertIn(
            "lemma proc pane capture",
            self.server.command("proc", "pane", "capture", "--help").output,
        )
        self.assertIn(
            "Pane Commands", self.server.command("proc", "pane", "--help").output
        )

    def test_basic_send_wait_capture_share_the_structured_execution_path(self) -> None:
        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "basic-input",
            "--hold",
            "--",
            "/bin/sh",
            "-c",
            "printf '__READY__\\n'; IFS= read -r value; printf 'received:<%s>\\n' \"$value\"",
        )
        self.assertEqual(status, 0, started)
        target = ("--session", "basic-input", "--pane", started["pane"])
        ready = self.public_command(
            "wait", *target, "--contains", "__READY__", "--timeout", "2s"
        )
        self.assertEqual((ready.returncode, ready.stdout, ready.stderr), (0, "", ""))
        # Option-looking input stays opaque; it must not become CLI flags or help.
        sent = self.public_command(
            "send",
            *target,
            "--text",
            "--session",
            "--paste",
            "--json",
            "--text",
            "--help",
        )
        self.assertEqual((sent.returncode, sent.stdout, sent.stderr), (0, "", ""))
        status, entered = self.json_command("send", *target, "--json", "--key", "enter")
        self.assertEqual(status, 0, entered)
        self.assertEqual(entered["command"], "pane.input")
        exited = self.public_command(
            "wait", *target, "--exit-code", "0", "--timeout", "2s"
        )
        self.assertEqual((exited.returncode, exited.stdout, exited.stderr), (0, "", ""))
        capture = self.public_command(
            "capture", *target, "--source", "recent", "--lines", "100"
        )
        self.assertEqual(capture.returncode, 0, capture.stderr)
        self.assertEqual(capture.stderr, "")
        self.assertIn("received:<--session--json--help>", capture.stdout)
        status, structured = self.json_command(
            "capture",
            *target,
            "--json",
            "--source",
            "recent",
            "--lines",
            "100",
            unwrap=False,
        )
        self.assertEqual(status, 0, structured)
        self.assertEqual(structured["schema"], "lemma.proc-result/v1")
        self.assertEqual(len(structured["results"]), 1)
        self.assertEqual(
            capture.stdout, structured["results"][0]["result"]["capture"]["text"]
        )
        rejected = self.public_command("send", *target, "--text", "after-exit")
        self.assertEqual(rejected.returncode, 1)
        self.assertEqual(rejected.stdout, "")
        self.assertIn("input_unavailable", rejected.stderr)

    def test_basic_layout_verbs_apply_to_stable_pane_ids(self) -> None:
        self.server.require_command("start", "basic-layout", "--", "/bin/sh")
        target = ("--session", "basic-layout", "--pane", "0:1")
        split = self.public_command(
            "split", *target, "--right", "--focus", "preserve", "--", "/bin/sh"
        )
        self.assertEqual(split.returncode, 0, split.stderr)
        self.assertEqual(split.stderr, "")
        self.assertRegex(split.stdout, r"^\d+:\d+\n$")
        pane = split.stdout.strip()
        status, before = self.json_command(
            "proc", "tab", "inspect", "--session", "basic-layout", "--tab", "0:1"
        )
        self.assertEqual(status, 0, before)
        self.assertEqual(before["tab_state"]["focused_pane"], "0:1")
        for arguments in (
            ("focus", "--session", "basic-layout", "--pane", pane),
            ("zoom", *target, "--on"),
            ("zoom", *target, "--off"),
            ("resize", *target, "right", "3"),
            ("swap", *target, pane),
        ):
            with self.subTest(arguments=arguments):
                result = self.public_command(*arguments)
                self.assertEqual(
                    (result.returncode, result.stdout, result.stderr), (0, "", "")
                )
        status, after = self.json_command(
            "proc", "tab", "inspect", "--session", "basic-layout", "--tab", "0:1"
        )
        self.assertEqual(status, 0, after)
        self.assertFalse(after["tab_state"]["zoomed"])
        self.assertNotEqual(before["tab_state"]["layout"], after["tab_state"]["layout"])
        status, focused = self.json_command("focus", *target, "--json")
        self.assertEqual(status, 0, focused)
        self.assertEqual(focused["command"], "pane.focus")

    def test_basic_split_preserves_exact_argv_after_separator(self) -> None:
        self.server.require_command("start", "basic-argv", "--", "/bin/sh")
        status, split = self.json_command(
            "split",
            "--session",
            "basic-argv",
            "--pane",
            "0:1",
            "--json",
            "--down",
            "--hold",
            "--",
            "/bin/sh",
            "-c",
            "printf '%s\\n' \"$@\"",
            "sh",
            "--session",
            "--json",
            "--help",
        )
        self.assertEqual(status, 0, split)
        self.assertEqual(split["command"], "pane.split")
        target = ("--session", "basic-argv", "--pane", split["pane"])
        waited = self.public_command(
            "wait", *target, "--exit-code", "0", "--timeout", "2s"
        )
        self.assertEqual(waited.returncode, 0, waited.stderr)
        capture = self.public_command("capture", *target)
        self.assertEqual(capture.returncode, 0, capture.stderr)
        self.assertIn("--session\n--json\n--help", capture.stdout)

    def test_basic_commands_reject_invalid_input_before_execution(self) -> None:
        self.server.require_command("start", "basic-invalid", "--", "/bin/sh")
        target = ("--session", "basic-invalid", "--pane", "0:1")
        for arguments in (
            ("capture",),
            ("capture", "--session", "basic-invalid"),
            ("capture", *target, "--json", "--json"),
            ("capture", *target, "--pane", "1:1"),
            ("capture", *target, "--source", "last-command", "--lines", "10"),
            ("send", *target, "--text", "must-not-be-sent", "--unknown"),
            ("send", *target, "--key", "not-a-key"),
            ("wait", *target, "--contains", "x", "--until-prompt"),
            ("split", *target, "--left"),
        ):
            with self.subTest(arguments=arguments):
                result = self.public_command(*arguments)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("invalid lemma", result.stderr)
        capture = self.public_command("capture", *target)
        self.assertNotIn("must-not-be-sent", capture.stdout)
        for name in ("capture", "wait", "send", "focus"):
            suffix = ("--text", "x") if name == "send" else ()
            result = self.public_command(
                name, "--session", "basic-invalid", "--pane", "63:99", *suffix
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertEqual(result.stdout, "")
            self.assertIn("stale", result.stderr)

    def test_documented_job_runs_captures_and_cleans_up(self) -> None:
        example = Path(__file__).resolve().parents[2] / "examples/job.json"
        status, result = self.json_command("proc", "--file", str(example), unwrap=False)
        self.assertEqual(status, 0, result)
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["schema"], "lemma.proc-result/v1")
        self.assertEqual(
            [entry["result"]["status"] for entry in result["results"]],
            ["applied"] * 4,
        )
        self.assertIn(
            "hello from Lemma", result["results"][2]["result"]["capture"]["text"]
        )

    def test_coding_agent_skill_is_valid_and_teaches_a_safe_job_workflow(self) -> None:
        result = self.server.command("skill")
        self.assertEqual(result.status, 0, result.output)
        self.assertTrue(result.output.startswith("---\nname: lemma\ndescription:"))
        self.assertIn("license: MIT\n", result.output)
        normalized = " ".join(result.output.split())
        self.assertIn("Use **closed-loop control**", normalized)
        self.assertIn("Creation does not change those environment values.", normalized)
        self.assertIn("the first Session starts the daemon", normalized)
        self.assertIn("JSON Proc selectors are objects", normalized)
        self.assertIn("`id` is not a universal label", normalized)
        self.assertIn("Command success is not process success.", normalized)

        example = (
            result.output.split("## Preferred detached-job Proc", maxsplit=1)[1]
            .split("```json\n", maxsplit=1)[1]
            .split("\n```", maxsplit=1)[0]
        )
        procedure = json.loads(example)
        self.assertEqual(procedure["schema"], "lemma.proc/v1")
        self.assertEqual(procedure["on_error"], "continue")
        self.assertEqual(
            [command["command"] for command in procedure["commands"]],
            ["session.start", "pane.wait", "pane.capture", "session.kill"],
        )
        self.assertTrue(procedure["commands"][0]["hold"])
        self.assertEqual(procedure["commands"][1]["exit_code"], 0)
        self.assertEqual(procedure["commands"][2]["pane"], {"result": "job"})
        self.assertEqual(procedure["commands"][3]["session"], {"result": "job"})

        procedure["commands"][0] |= {
            "name": "skill-job",
            "cwd": str(self.server.root),
            "argv": ["/bin/sh", "-c", "printf '__SKILL_JOB__\\n'"],
        }
        proc = self.server.root / "skill-job.json"
        proc.write_text(json.dumps(procedure))
        status, execution = self.json_command("proc", "--file", str(proc), unwrap=False)
        self.assertEqual(status, 0, execution)
        self.assertEqual(
            [entry["result"]["status"] for entry in execution["results"]],
            ["applied"] * 4,
        )
        self.assertIn(
            "__SKILL_JOB__", execution["results"][2]["result"]["capture"]["text"]
        )

    def test_focus_preserving_creation_keeps_controller_selection(self) -> None:
        status, started = self.json_command(
            "proc", "session", "start", "focus-preserve", "--", "/bin/sh"
        )
        self.assertEqual(status, 0, started)
        self.assertEqual(started["session"]["revision"], 1)
        status, daemon = self.json_command("proc", "daemon", "inspect")
        self.assertEqual(status, 0, daemon)
        self.assertEqual(daemon["daemon"]["resources"]["sessions"]["used"], 1)
        status, session = self.json_command(
            "proc", "session", "inspect", "--session", "focus-preserve"
        )
        self.assertEqual(status, 0, session)
        self.assertEqual(session["session_state"]["active_tab"], "0:1")

        status, tab = self.json_command(
            "proc",
            "tab",
            "new",
            "--session",
            "focus-preserve",
            "--focus",
            "preserve",
            "--",
            "/bin/sh",
        )
        self.assertEqual(status, 0, tab)
        self.assertEqual(tab["tab"], "1:1")

        status, inspected = self.json_command(
            "proc", "tab", "inspect", "--session", "focus-preserve", "--tab", "1:1"
        )
        self.assertEqual(status, 0, inspected)
        self.assertFalse(inspected["tab_state"]["active"])

        status, split = self.json_command(
            "proc",
            "pane",
            "split",
            "--session",
            "focus-preserve",
            "--pane",
            "0:1",
            "--right",
            "--focus",
            "preserve",
            "--",
            "/bin/sh",
        )
        self.assertEqual(status, 0, split)

        status, panes = self.json_command(
            "proc", "pane", "list", "--session", "focus-preserve"
        )
        self.assertEqual(status, 0, panes)
        active_tab = session["session_state"]["active_tab"]
        focused = [
            pane["id"]
            for pane in panes["panes"]
            if pane["tab"] == active_tab and pane["focused"]
        ]
        self.assertEqual(focused, ["0:1"])
        created = next(pane for pane in panes["panes"] if pane["id"] == split["pane"])
        self.assertFalse(created["focused"])
        status, pane = self.json_command(
            "proc", "pane", "inspect", "--session", "focus-preserve", "--pane", "0:1"
        )
        self.assertEqual(status, 0, pane)
        self.assertEqual(pane["pane_state"]["terminal"]["screen"], "primary")

        status, conflict = self.json_command(
            "proc",
            "pane",
            "kill",
            "--session",
            "focus-preserve",
            "--pane",
            split["pane"],
            "--if-session-revision",
            "1",
        )
        self.assertEqual(status, 1, conflict)
        self.assertEqual(conflict["status"], "conflict")

    def test_atomic_logical_keys_preserve_order_and_modifier_semantics(self) -> None:
        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "logical-key",
            "--hold",
            "--",
            str(self.server.peer_path),
            "logical-keys",
        )
        self.assertEqual(status, 0, started)
        status, ready = self.json_command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "logical-key",
            "--contains",
            "__READY__",
            "--timeout",
            "2s",
        )
        self.assertEqual(status, 0, ready)

        status, sent = self.json_command(
            "proc",
            "pane",
            "input",
            "--session",
            "logical-key",
            "--pane",
            "0:1",
            "--key",
            "shift+a",
            "--key",
            "control+a",
        )
        self.assertEqual(status, 0, sent)
        status, received = self.json_command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "logical-key",
            "--contains",
            "__BYTES_4101__",
            "--timeout",
            "2s",
        )
        self.assertEqual(status, 0, received)
        self.assertIn("__BYTES_4101__", received["capture"]["text"])
        status, exited = self.json_command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "logical-key",
            "--exit-code",
            "0",
            "--timeout",
            "2s",
        )
        self.assertEqual(status, 0, exited)
        status, inspected = self.json_command(
            "proc", "pane", "inspect", "--session", "logical-key", "--pane", "0:1"
        )
        self.assertEqual(status, 0, inspected)
        self.assertEqual(inspected["pane_state"]["process"]["state"], "exited")
        self.assertEqual(inspected["pane_state"]["process"]["code"], 0)
        self.assertFalse(inspected["pane_state"]["terminal"]["input_accepted"])
        status, capture = self.json_command(
            "proc",
            "pane",
            "capture",
            "--session",
            "logical-key",
            "--pane",
            "0:1",
            "--source",
            "recent",
            "--lines",
            "20",
            "--wrap",
            "logical",
        )
        self.assertEqual(status, 0, capture)
        self.assertIn("__BYTES_4101__", capture["capture"]["text"])
        status, rejected = self.json_command(
            "proc",
            "pane",
            "input",
            "--session",
            "logical-key",
            "--pane",
            "0:1",
            "--key",
            "enter",
        )
        self.assertEqual(status, 1, rejected)
        self.assertEqual(
            rejected["error"], {"reason": "input_unavailable", "retryable": False}
        )

    def test_visible_and_recent_capture_have_distinct_bounded_sources(self) -> None:
        script = "i=1; while [ $i -le 30 ]; do printf '__LINE_%02d__\\n' $i; i=$((i+1)); done"
        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "capture-sources",
            "--hold",
            "--",
            "/bin/sh",
            "-c",
            script,
        )
        self.assertEqual(status, 0, started)
        status, exited = self.json_command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "capture-sources",
            "--timeout",
            "2s",
        )
        self.assertEqual(status, 0, exited)
        status, visible = self.json_command(
            "proc",
            "pane",
            "capture",
            "--session",
            "capture-sources",
            "--pane",
            "0:1",
            "--source",
            "visible",
        )
        self.assertEqual(status, 0, visible)
        self.assertIn("__LINE_30__", visible["capture"]["text"])
        self.assertNotIn("__LINE_01__", visible["capture"]["text"])

        status, recent = self.json_command(
            "proc",
            "pane",
            "capture",
            "--session",
            "capture-sources",
            "--pane",
            "0:1",
            "--source",
            "recent",
            "--lines",
            "31",
        )
        self.assertEqual(status, 0, recent)
        self.assertIn("__LINE_01__", recent["capture"]["text"])
        self.assertIn("__LINE_30__", recent["capture"]["text"])

    def test_wait_matches_semantic_prompt_with_capture_evidence(self) -> None:
        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "prompt-wait",
            "--hold",
            "--",
            "/bin/sh",
            "-c",
            "printf '\\033]133;A\\007$ '; sleep 1",
        )
        self.assertEqual(status, 0, started)
        status, prompted = self.json_command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "prompt-wait",
            "--until-prompt",
            "--timeout",
            "2s",
        )
        self.assertEqual(status, 0, prompted)
        self.assertEqual(prompted["status"], "applied")
        self.assertIn("capture", prompted)

    def test_wait_without_target_uses_current_pane_and_process_completion(self) -> None:
        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "context-wait",
            "--hold",
            "--",
            "/bin/sh",
            "-c",
            "sleep .1; exit 6",
        )
        self.assertEqual(status, 0, started)
        environment = self.server.environment | {
            "LEMMA_SESSION_ID": started["session"]["id"],
            "LEMMA_TAB_ID": started["tab"],
            "LEMMA_PANE_ID": started["pane"],
        }
        completed = subprocess.run(
            [
                str(self.server.cli_path),
                str(self.server.socket_path),
                "proc",
                "pane",
                "wait",
            ],
            env=environment,
            capture_output=True,
            text=True,
            timeout=5.0,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        waited_proc = json.loads(completed.stdout)
        waited = waited_proc["results"][0]["result"]
        self.assertEqual(waited["condition"], "process-exit")
        self.assertEqual(waited["process"], {"state": "exited", "code": 6})
        basic = self.public_command("wait", environment=environment)
        self.assertEqual((basic.returncode, basic.stdout, basic.stderr), (0, "", ""))
        required = self.public_command(
            "wait", "--exit-code", "0", environment=environment
        )
        self.assertEqual(required.returncode, 1)
        self.assertEqual(required.stdout, "")
        self.assertIn("unexpected_exit", required.stderr)

    def test_wait_command_preserves_persistent_control_lockstep(self) -> None:
        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "persistent-wait",
            "--hold",
            "--",
            "/bin/sh",
            "-c",
            "sleep .1; exit 0",
        )
        self.assertEqual(status, 0, started)
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.settimeout(5.0)
        connection.connect(str(self.server.socket_path))
        self.addCleanup(connection.close)
        stream = connection.makefile("r", encoding="utf-8")
        self.addCleanup(stream.close)
        connection.sendall(
            json.dumps(
                {
                    "schema": "lemma.proc/v1",
                    "commands": [
                        {
                            "command": "pane.wait",
                            "session": {"name": "persistent-wait"},
                            "pane": {"id": "0:1"},
                            "timeout_ms": 2000,
                        }
                    ],
                },
                separators=(",", ":"),
            ).encode()
            + b"\n"
        )
        waited = json.loads(stream.readline())
        self.assertEqual(waited["results"][0]["result"]["status"], "applied")
        connection.sendall(
            b'{"schema":"lemma.proc/v1","commands":[{"command":"session.inspect",'
            b'"session":{"name":"persistent-wait"}}]}\n'
        )
        inspected = json.loads(stream.readline())
        self.assertEqual(inspected["results"][0]["result"]["status"], "applied")

    def test_wait_reports_stale_and_unexpected_exit_from_initial_state(self) -> None:
        self.server.require_command("start", "wait-state")
        status, stale = self.json_command(
            "proc",
            "pane",
            "wait",
            "63:99",
            "--session",
            "wait-state",
            "--contains",
            "never",
            "--timeout",
            "100ms",
        )
        self.assertEqual(status, 1, stale)
        self.assertEqual(stale["status"], "stale")

        status, timed_out = self.json_command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "wait-state",
            "--contains",
            "never",
            "--timeout",
            "10ms",
        )
        self.assertEqual(status, 1, timed_out)
        self.assertEqual(timed_out["status"], "failed")
        self.assertEqual(timed_out["error"]["reason"], "timeout")

        timed_out = self.public_command(
            "wait",
            "0:1",
            "--session",
            "wait-state",
            "--contains",
            "never",
            "--timeout",
            "10ms",
        )
        self.assertEqual(timed_out.returncode, 1)
        self.assertEqual(timed_out.stdout, "")
        self.assertIn("timeout", timed_out.stderr)

        invalid = self.server.command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "wait-state",
            "--contains",
            "never",
            "--timeout",
            "10ms",
            "--if-session-revision",
            "1",
        )
        self.assertEqual(invalid.status, 2, invalid.output)

        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "wait-exit",
            "--hold",
            "--",
            "/bin/sh",
            "-c",
            "exit 3",
        )
        self.assertEqual(status, 0, started)
        status, exited = self.json_command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "wait-exit",
            "--contains",
            "never",
            "--timeout",
            "2s",
        )
        self.assertEqual(status, 1, exited)
        self.assertEqual(exited["status"], "failed")
        self.assertEqual(exited["error"]["reason"], "unexpected_exit")
        self.assertEqual(exited["process"], {"state": "exited", "code": 3})

        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "wait-close",
            "--",
            str(self.server.peer_path),
            "delayed-exit",
        )
        self.assertEqual(status, 0, started)
        status, closed = self.json_command(
            "proc",
            "pane",
            "wait",
            "0:1",
            "--session",
            "wait-close",
            "--timeout",
            "2s",
        )
        self.assertEqual(status, 0, closed)
        self.assertEqual(closed["status"], "applied")
        self.assertEqual(closed["process"], {"state": "exited", "code": 4})

    def test_global_signal_events_and_close_on_exit_wait(self) -> None:
        # The daemon exits with its last Session; keep one beyond the closing Pane's Session.
        self.server.require_command(
            "proc",
            "session",
            "start",
            "signal-keeper",
            "--",
            "/bin/sh",
            "-c",
            "sleep 30",
        )
        process = subprocess.Popen(
            [
                str(self.server.cli_path),
                str(self.server.socket_path),
                "events",
                "--signals",
            ],
            env=self.server.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )

        def stop() -> None:
            process.kill()
            process.communicate(timeout=1.0)

        self.addCleanup(stop)
        assert process.stdout is not None
        stream = process.stdout.fileno()
        pending = bytearray()

        def next_event(deadline: float) -> dict[str, Any] | None:
            while b"\n" not in pending:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                ready, _, _ = select.select([stream], [], [], remaining)
                if ready:
                    chunk = os.read(stream, 65536)
                    if not chunk:
                        return None
                    pending.extend(chunk)
            line, _, rest = bytes(pending).partition(b"\n")
            pending[:] = rest
            return json.loads(line)

        snapshot = next_event(time.monotonic() + 2.0)
        self.assertIsNotNone(snapshot)
        assert snapshot is not None
        self.assertEqual(snapshot["event"], "snapshot")
        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "signal-feed",
            "--",
            "/bin/sh",
            "-c",
            r"printf '\033]9;4;3\033\\\033]777;notify;Job;finished\007'; sleep 1; exit 5",
        )
        self.assertEqual(status, 0, started)
        status, waited = self.json_command(
            "proc",
            "pane",
            "wait",
            started["pane"],
            "--session",
            "signal-feed",
            "--timeout",
            "5s",
        )
        # Close-on-exit retains the exit status for the pending wait.
        self.assertEqual(status, 0, waited)
        self.assertEqual(waited["process"], {"state": "exited", "code": 5})

        deadline = time.monotonic() + 5.0
        seen: list[dict[str, Any]] = []
        signals: dict[str, Any] | None = None
        while signals is None and (event := next_event(deadline)) is not None:
            seen.append(event)
            if (
                event["event"] == "pane.signal"
                and event["session"] == started["session"]["id"]
                and event["signals"]["notifications"] == 1
            ):
                signals = event["signals"]
        self.assertIsNotNone(signals, (seen, self.server.logs()))
        assert signals is not None
        self.assertEqual(
            signals["progress"], {"state": "indeterminate", "percent": None}
        )
        self.assertEqual(signals["notification"]["title"], "Job")

    def test_command_wait_after_input_in_one_proc_observes_fast_commands(self) -> None:
        # A minimal OSC 133 shell: prompt (A/B), then C, the command, and D;status per line.
        shell = (
            r"while printf '\033]133;A\007$ \033]133;B\007'; read line; do "
            r"""printf '\033]133;C\007'; sh -c "$line"; """
            r"printf '\033]133;D;%s\007' $?; done"
        )
        status, started = self.json_command(
            "proc", "session", "start", "command-wait", "--", "/bin/sh", "-c", shell
        )
        self.assertEqual(status, 0, started)
        target = {
            "session": {"id": started["session"]["id"]},
            "pane": {"id": started["pane"]},
        }
        # Pending Procs share the one-step-per-turn Proc service, delaying a wait step's start.
        waiters = [
            subprocess.Popen(
                [
                    str(self.server.cli_path),
                    str(self.server.socket_path),
                    "proc",
                    "pane",
                    "wait",
                    started["pane"],
                    "--session",
                    "command-wait",
                    "--contains",
                    "never-matches",
                    "--timeout",
                    "30s",
                ],
                env=self.server.environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            for _ in range(12)
        ]
        for waiter in waiters:
            self.addCleanup(waiter.wait, 5.0)
            self.addCleanup(waiter.kill)
        for index, (line, code) in enumerate([("true", 0), ("exit 7", 7)] * 5, start=1):
            completed = subprocess.run(
                [
                    str(self.server.cli_path),
                    str(self.server.socket_path),
                    "proc",
                    "--stdin",
                ],
                env=self.server.environment,
                input=json.dumps(
                    {
                        "schema": "lemma.proc/v1",
                        "commands": [
                            {
                                "command": "pane.input",
                                **target,
                                "events": [
                                    {"kind": "text", "text": line},
                                    {"kind": "key", "key": "enter"},
                                ],
                            },
                            {
                                "command": "pane.wait",
                                **target,
                                "until_command": True,
                                "timeout_ms": 5000,
                            },
                        ],
                    }
                ),
                capture_output=True,
                text=True,
                timeout=10.0,
                check=False,
            )
            self.assertEqual(
                completed.returncode, 0, completed.stdout + completed.stderr
            )
            waited = json.loads(completed.stdout)["results"][1]["result"]
            self.assertEqual(
                waited["completion"], {"commands": index, "exit_code": code}
            )

        # Readable CLI: print the latest completion's exit code; success requires status 0.
        selectors = ("--session", "command-wait", "--pane", started["pane"])
        failed = self.public_command(
            "wait", *selectors, "--until-command", "--after-commands", "9"
        )
        self.assertEqual((failed.returncode, failed.stdout), (1, "7\n"), failed.stderr)
        self.server.require_command(
            "send", *selectors, "--paste", "true", "--key", "enter"
        )
        passed = self.public_command(
            "wait",
            *selectors,
            "--until-command",
            "--after-commands",
            "10",
            "--timeout",
            "3s",
        )
        self.assertEqual((passed.returncode, passed.stdout), (0, "0\n"), passed.stderr)

    def test_legacy_action_and_op_interfaces_are_rejected(self) -> None:
        cli = self.server.command("action", "daemon", "inspect")
        self.assertEqual(cli.status, 2, cli.output)

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(2.0)
            connection.connect(str(self.server.socket_path))
            connection.sendall(
                b'{"schema":"lemma.action/v1","action":"daemon.inspect"}\n'
            )
            response = json.loads(connection.makefile("r", encoding="utf-8").readline())
        self.assertEqual(response["schema"], "lemma.proc-result/v1")
        self.assertEqual(response["error"]["reason"], "invalid_schema")

        for name, document in (
            (
                "legacy-action-proc.json",
                '{"schema":"lemma.proc/v1","actions":[{"action":"daemon.inspect"}]}',
            ),
            (
                "legacy-op-proc.json",
                '{"schema":"lemma.proc/v1","ops":[{"op":"daemon.inspect"}]}',
            ),
        ):
            legacy = self.server.root / name
            legacy.write_text(document)
            status, rejected = self.json_command("proc", "--file", str(legacy))
            self.assertEqual(status, 2, rejected)
            self.assertEqual(rejected["error"]["reason"], "unknown_field")

    def test_direct_proc_cli_wraps_one_command_in_a_proc_result(self) -> None:
        status, started = self.json_command(
            "proc",
            "session",
            "start",
            "direct-proc",
            "--hold",
            "--",
            "/bin/sh",
            unwrap=False,
        )
        self.assertEqual(status, 0, started)
        self.assertEqual(started["schema"], "lemma.proc-result/v1")
        self.assertTrue(started["ok"])
        self.assertEqual(len(started["results"]), 1)
        nested = started["results"][0]["result"]
        self.assertEqual(nested["schema"], "lemma.command-result/v1")
        self.assertEqual(nested["command"], "session.start")
        self.assertEqual(nested["status"], "applied")

        status, inspected = self.json_command(
            "proc", "session", "inspect", "--session", "direct-proc"
        )
        self.assertEqual(status, 0, inspected)
        self.assertEqual(inspected["schema"], "lemma.command-result/v1")
        self.assertEqual(inspected["session"]["name"], "direct-proc")

        empty = self.server.root / "empty-proc.json"
        empty.write_text('{"schema":"lemma.proc/v1","commands":[]}')
        status, rejected = self.json_command("proc", "--file", str(empty))
        self.assertEqual(status, 2, rejected)
        self.assertEqual(rejected["error"]["reason"], "invalid_document")

    def test_closing_control_owner_cancels_proc_before_later_commands(self) -> None:
        self.server.require_command(
            "proc", "session", "start", "cancel-proc", "--hold", "--", "/bin/sh"
        )
        request = json.dumps(
            {
                "schema": "lemma.proc/v1",
                "on_error": "continue",
                "commands": [
                    {
                        "command": "session.rename",
                        "session": {"name": "cancel-proc"},
                        "name": "cancel-proc-running",
                    },
                    {
                        "command": "pane.wait",
                        "session": {"name": "cancel-proc-running"},
                        "pane": {"id": "0:1"},
                        "contains": "__NEVER__",
                        "timeout_ms": 200,
                    },
                    {
                        "command": "session.kill",
                        "session": {"name": "cancel-proc-running"},
                    },
                ],
            },
            separators=(",", ":"),
        ).encode()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.connect(str(self.server.socket_path))
            connection.sendall(request + b"\n")
            for _ in range(20):
                status, _ = self.json_command(
                    "proc", "session", "inspect", "--session", "cancel-proc-running"
                )
                if status == 0:
                    break
                time.sleep(0.01)
            self.assertEqual(status, 0)
        time.sleep(0.3)

        status, inspected = self.json_command(
            "proc", "session", "inspect", "--session", "cancel-proc-running"
        )
        self.assertEqual(status, 0, inspected)
        self.assertEqual(inspected["status"], "applied")

    def test_proc_composes_input_wait_and_inspection_commands(self) -> None:
        proc = self.server.root / "agent-proc.json"
        proc.write_text(
            json.dumps(
                {
                    "schema": "lemma.proc/v1",
                    "commands": [
                        {
                            "id": "root",
                            "command": "session.start",
                            "name": "agent-proc",
                            "hold": True,
                            "argv": ["/bin/sh"],
                        },
                        {
                            "command": "pane.input",
                            "pane": {"result": "root"},
                            "events": [
                                {"kind": "text", "text": "printf '__PROC_INPUT__\\n'"},
                                {"kind": "key", "key": "enter"},
                            ],
                        },
                        {
                            "command": "pane.wait",
                            "pane": {"result": "root"},
                            "contains": "__PROC_INPUT__",
                            "timeout_ms": 2000,
                        },
                        {"command": "pane.inspect", "pane": {"result": "root"}},
                    ],
                },
                separators=(",", ":"),
            )
        )
        status, result = self.json_command("proc", "--file", str(proc), unwrap=False)
        self.assertEqual(status, 0, result)
        self.assertTrue(result["ok"])
        self.assertEqual(
            [entry["result"]["status"] for entry in result["results"]], ["applied"] * 4
        )
        waited = result["results"][2]["result"]
        self.assertEqual(waited["command"], "pane.wait")
        self.assertIn("__PROC_INPUT__", waited["capture"]["text"])

    def test_proc_reports_unresolved_dependencies_without_placeholder_ids(self) -> None:
        self.server.require_command(
            "proc", "session", "start", "dependency-source", "--hold", "--", "/bin/sh"
        )
        proc = self.server.root / "unresolved-proc.json"
        proc.write_text(
            json.dumps(
                {
                    "schema": "lemma.proc/v1",
                    "on_error": "continue",
                    "commands": [
                        {
                            "id": "failed",
                            "command": "session.start",
                            "name": "dependency-source",
                            "hold": True,
                            "argv": ["/bin/sh"],
                        },
                        {
                            "command": "pane.wait",
                            "pane": {"result": "failed"},
                            "timeout_ms": 100,
                        },
                        {
                            "command": "session.inspect",
                            "session": {"name": "dependency-source"},
                        },
                    ],
                },
                separators=(",", ":"),
            )
        )
        status, result = self.json_command("proc", "--file", str(proc), unwrap=False)
        self.assertEqual(status, 1, result)
        self.assertFalse(result["ok"])
        nested = [entry["result"] for entry in result["results"]]
        self.assertEqual(
            [entry["status"] for entry in nested], ["conflict", "failed", "applied"]
        )
        self.assertEqual(
            nested[1]["error"], {"reason": "unresolved_reference", "retryable": False}
        )
        self.assertNotIn("pane", nested[1])

    def test_multi_pane_snapshot_is_authoritative_before_staged_screens(self) -> None:
        self.server.require_command(
            "proc", "session", "start", "event-baseline", "--", "/bin/sh"
        )
        self.server.require_command(
            "proc",
            "pane",
            "split",
            "--session",
            "event-baseline",
            "--pane",
            "0:1",
            "--right",
            "--focus",
            "preserve",
            "--",
            "/bin/sh",
        )
        snapshot = self.first_event(
            "events",
            "--session",
            "event-baseline",
            "--pane",
            "0:1",
            "--pane",
            "1:1",
            "--screen",
        )
        self.assertEqual(snapshot["event"], "snapshot")
        self.assertEqual(snapshot["sequence"], 0)
        self.assertEqual(
            [(pane["pane"], pane["present"]) for pane in snapshot["panes"]],
            [("0:1", True), ("1:1", True)],
        )
        self.assertTrue(
            all(
                "generation" in pane and "process" in pane for pane in snapshot["panes"]
            )
        )

        missing = self.first_event(
            "events", "--session", "event-baseline", "--pane", "63:99", "--screen"
        )
        self.assertEqual(missing["event"], "snapshot")
        self.assertFalse(missing["present"])


if __name__ == "__main__":
    unittest.main()
