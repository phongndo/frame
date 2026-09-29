from __future__ import annotations

import json
import os
import socket
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from tools import benchmark_frame_skill as benchmark


class AgentSkillBenchmarkTest(unittest.TestCase):
    def test_pi_trace_is_normalized_for_agent_independent_scoring(self) -> None:
        messages = [
            {
                "role": "assistant",
                "content": [
                    {
                        "type": "toolCall",
                        "name": "read",
                        "arguments": {"path": "/tmp/skill/frame/SKILL.md"},
                    }
                ],
                "usage": {
                    "input": 10,
                    "output": 2,
                    "reasoning": 1,
                    "totalTokens": 13,
                    "cost": {"total": 0.25},
                },
            },
            {
                "role": "toolResult",
                "content": [{"type": "text", "text": "skill body"}],
            },
            {
                "role": "assistant",
                "content": [{"type": "text", "text": "complete"}],
            },
        ]
        output = json.dumps({"type": "agent_end", "messages": messages})

        final, calls, results, loaded, usage = benchmark.parse_pi_events(output)

        self.assertEqual(final, "complete")
        self.assertEqual(calls[0]["name"], "read")
        self.assertEqual(results, ["skill body"])
        self.assertTrue(loaded)
        self.assertEqual(usage["total_tokens"], 13)
        self.assertEqual(usage["cost"], 0.25)

    def test_session_names_are_valid_and_bounded(self) -> None:
        for case in benchmark.DEFAULT_CASES:
            name = benchmark.session_name(case, 999)
            self.assertLessEqual(len(name), 32)
            self.assertRegex(name, r"^[A-Za-z0-9_][A-Za-z0-9_-]*$")

    def test_run_environment_removes_inherited_frame_targets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            command = root / "bin" / "frame"
            subject = benchmark.Subject(
                binary=command,
                command=command,
                skill=root / "skill" / "frame" / "SKILL.md",
                binary_sha256="0" * 64,
                skill_sha256="1" * 64,
            )
            inherited = {
                "FRAME_SESSION_ID": "9:9",
                "FRAME_SESSION_NAME": "user-session",
                "FRAME_TAB_ID": "9:8",
                "FRAME_PANE_ID": "9:7",
            }
            with mock.patch.dict(os.environ, inherited, clear=False):
                environment = benchmark.run_environment(subject, root / "runtime", None)

        for name in inherited:
            self.assertNotIn(name, environment)
        self.assertEqual(environment["FRAME_DEV_RUNTIME_DIR"], str(root / "runtime"))

    def test_shutdown_runtime_stops_a_live_isolated_daemon(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary)
            socket_path = runtime / "daemon.sock"
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(socket_path))
            listener.listen()
            shutdown_seen = threading.Event()

            def serve() -> None:
                try:
                    while True:
                        connection, _ = listener.accept()
                        with connection:
                            if connection.recv(1) == b"S":
                                shutdown_seen.set()
                                return
                finally:
                    listener.close()
                    socket_path.unlink(missing_ok=True)

            server = threading.Thread(target=serve)
            server.start()
            benchmark.shutdown_runtime(runtime)
            server.join(timeout=2.0)

        self.assertTrue(shutdown_seen.is_set())
        self.assertFalse(server.is_alive())

    def test_cold_job_scoring_requires_observed_output_and_bounded_wait(self) -> None:
        procedure = (
            "frame proc --stdin <<'EOF'\n"
            '{"commands":['
            '{"id":"job","command":"session.start"},'
            '{"command":"pane.wait","pane":{"result":"job"},'
            '"timeout_ms":30000},'
            '{"command":"pane.capture","pane":{"result":"job"}}]}\n'
            "EOF"
        )
        trace = benchmark.AgentTrace(
            returncode=0,
            elapsed_seconds=1.0,
            final_text="child exit code: 7",
            tool_calls=[{"name": "bash", "arguments": {"command": procedure}}],
            tool_results=[
                '{"state":"exited","code":7,'
                '"capture":{"text":"__FRAME_BENCH_COLD_FAILURE__"}}'
            ],
        )
        with tempfile.TemporaryDirectory() as temporary:
            checks = benchmark.score_positive(
                "cold-failure",
                trace,
                [],
                [],
                None,
                [],
                "",
                Path(temporary) / "missing",
            )

        self.assertTrue(all(checks.values()), checks)

    def test_external_adapter_uses_the_versioned_json_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workspace = root / "workspace"
            artifacts = root / "artifacts"
            workspace.mkdir()
            artifacts.mkdir()
            executable = root / "adapter"
            executable.write_text(
                f"#!{sys.executable}\n"
                "import json,sys\n"
                "request=json.load(open(sys.argv[1]))\n"
                f"assert request['schema']=={benchmark.REQUEST_SCHEMA!r}\n"
                "print(json.dumps({"
                f"'schema':{benchmark.RESULT_SCHEMA!r},"
                "'returncode':0,'final_text':'done','tool_calls':[],"
                "'tool_results':[],'skill_loaded':True}))\n",
                encoding="utf-8",
            )
            executable.chmod(0o755)
            request = benchmark.AgentRequest(
                run_id="external",
                prompt="test",
                workspace=workspace,
                artifact_directory=artifacts,
                skill=root / "skill",
                provider="provider",
                model="model",
                thinking="low",
                timeout_seconds=5,
            )

            trace = benchmark.ExternalAdapter(executable).run(
                request, os.environ.copy()
            )

        self.assertEqual(trace.final_text, "done")
        self.assertTrue(trace.skill_loaded)
        self.assertEqual(trace.returncode, 0)


if __name__ == "__main__":
    unittest.main()
