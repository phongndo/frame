"""Public executable contracts, without the test CLI's legacy pane/tab routing."""

from __future__ import annotations

import json
import os
import select
import subprocess
import unittest
from pathlib import Path
from typing import Any

from tests.support.mux_harness import (
    ALT_SCREEN,
    LEMMA_OUTER_TERMINAL_RESTORE,
    LemmaServer,
)
from tests.support.pty_process import PtyProcess


class ProductionCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.server = LemmaServer.from_environment()
        self.addCleanup(self.server.close)
        self.executable = Path(os.environ["LEMMA_TEST_EXECUTABLE"]).resolve()
        self.assertTrue(self.executable.is_file(), self.executable)
        # Route the real main() to this test's owned daemon, never the user's runtime.
        build_id = "0" * 64
        marker = self.server.root / "daemon.sock.build-id"
        marker.write_text(build_id)
        marker.chmod(0o600)
        self.environment = self.server.environment | {
            "LEMMA_DEV_RUNTIME_DIR": str(self.server.root),
            "LEMMA_DEV_BUILD_ID": build_id,
        }

    def command(
        self,
        *arguments: str,
        input: str | None = None,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.executable), *arguments],
            env=self.environment if environment is None else environment,
            input=input,
            capture_output=True,
            text=True,
            timeout=5.0,
            check=False,
        )

    def ok(self, *arguments: str) -> str:
        result = self.command(*arguments)
        self.assertEqual(
            result.returncode, 0, (arguments, result.stdout, result.stderr)
        )
        self.assertEqual(result.stderr, "", (arguments, result.stderr))
        return result.stdout

    def result(self, *arguments: str) -> dict[str, Any]:
        document = json.loads(self.ok(*arguments))
        self.assertEqual(document["schema"], "lemma.proc-result/v1")
        self.assertTrue(document["ok"], document)
        self.assertEqual(len(document["results"]), 1, document)
        result = document["results"][0]["result"]
        self.assertEqual(result["schema"], "lemma.command-result/v1")
        self.assertIn(result["status"], ("applied", "no_effect"))
        return result

    def start(self, name: str, script: str = "exec /bin/sh") -> dict[str, Any]:
        return self.result(
            "session", "start", name, "--hold", "--", "/bin/sh", "-c", script
        )

    def test_panes_can_resolve_their_shipped_terminal_description(self) -> None:
        started = self.start(
            "terminfo",
            'printf "TERM=%s\\n" "$TERM"; '
            'test -r "$TERMINFO/l/lemma" || test -r "$TERMINFO/6c/lemma"; '
            'infocmp -x "$TERM"; printf "COLORS="; tput colors',
        )
        target = ("--session", "terminfo", "--pane", started["pane"])
        self.ok("wait", *target, "--exit-code", "0", "--timeout", "2s")
        captured = self.ok("capture", *target, "--source", "recent", "--lines", "250")
        self.assertIn("TERM=lemma", captured)
        self.assertIn("Lemma terminal multiplexer", captured)
        self.assertIn("COLORS=256", captured)
        for capability in ("Ss=", "Se=", "Cs=", "Cr="):
            self.assertIn(capability, captured)
        self.assertNotIn("Ms=", captured)

    def test_capture_and_split_fail_when_stdout_is_not_writable(self) -> None:
        started = self.start("output-failure", "printf 'small-output\\n'")
        target = ("--session", "output-failure", "--pane", started["pane"])
        self.ok("wait", *target, "--exit-code", "0", "--timeout", "2s")
        self.assertIn("small-output", self.ok("capture", *target))
        destination = self.server.root / "read-only-output"
        destination.write_bytes(b"unchanged")
        for verb in ("capture", "split"):
            for flags in ((), ("--json",)):
                with self.subTest(verb=verb, flags=flags):
                    options = (
                        (
                            "--right",
                            "--focus",
                            "preserve",
                            "--hold",
                            "--",
                            "/bin/sh",
                            "-c",
                            "exit 0",
                        )
                        if verb == "split"
                        else ()
                    )
                    with destination.open("rb") as stdout:
                        result = subprocess.run(
                            [str(self.executable), verb, *target, *flags, *options],
                            env=self.environment,
                            stdin=subprocess.DEVNULL,
                            stdout=stdout,
                            stderr=subprocess.PIPE,
                            timeout=5.0,
                            check=False,
                        )
                    self.assertEqual(result.returncode, 1, (verb, flags, result.stderr))
        self.assertEqual(destination.read_bytes(), b"unchanged")

    def test_every_advertised_command_has_offline_help(self) -> None:
        root = self.ok("--help")
        self.assertEqual(root, self.ok("help"))
        offline = self.server.root / "offline"
        offline.mkdir()
        environment = self.environment | {"LEMMA_DEV_RUNTIME_DIR": str(offline)}
        names = [
            "new",
            "start",
            "attach",
            "ls",
            "list",
            "split",
            "send",
            "wait",
            "capture",
            "focus",
            "zoom",
            "swap",
            "resize",
            "rename",
            "kill",
            "session",
            "tab",
            "pane",
            "proc",
            "events",
            "api",
            "config",
            "skill",
            "version",
        ]
        for name in names:
            for arguments in ((name, "--help"), ("help", name)):
                with self.subTest(arguments=arguments):
                    help_result = self.command(*arguments, environment=environment)
                    self.assertEqual(help_result.returncode, 0, help_result.stderr)
                    self.assertEqual(help_result.stderr, "")
                    self.assertNotEqual(help_result.stdout, root, name)
                    self.assertIn(name, help_result.stdout.lower())
        self.assertEqual(list(offline.iterdir()), [])
        for name in ("session", "tab", "pane"):
            self.assertEqual(self.ok(name), self.ok(name, "--help"))

    def test_unknown_help_topics_fail_instead_of_silently_showing_root_help(
        self,
    ) -> None:
        for arguments in (
            ("help", "not-a-command"),
            ("not-a-command", "--help"),
            ("proc", "not-a-domain", "--help"),
            ("config", "not-a-command", "--help"),
            ("api", "not-a-command", "--help"),
        ):
            with self.subTest(arguments=arguments):
                result = self.command(*arguments)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("invalid lemma", result.stderr)

    def test_basic_session_lifecycle_and_resource_aliases(self) -> None:
        self.start("anchor")
        created = self.ok(
            "start", "lifecycle", "--cwd", str(self.server.root), "--", "/bin/sh"
        )
        self.assertIn("lifecycle", created)
        # Foreground titles can update between calls; compare stable session identities.
        for verb in ("ls", "list"):
            lines = self.ok(verb).splitlines()
            self.assertEqual(len(lines), 2)
            self.assertEqual(
                {line.split('":', 1)[0] for line in lines},
                {'lemma session "anchor', 'lemma session "lifecycle'},
            )
        before = self.result("session", "inspect", "lifecycle")
        self.assertEqual(
            Path(before["session_state"]["launch"]["cwd"]).resolve(),
            self.server.root.resolve(),
        )
        self.ok("rename", "lifecycle", "renamed")
        after = self.result("proc", "session", "inspect", "renamed")
        self.assertEqual(after["session"]["id"], before["session"]["id"])
        self.assertEqual(after["session"]["name"], "renamed")
        self.ok("kill", "renamed")
        names = [
            session["name"] for session in self.result("session", "list")["sessions"]
        ]
        self.assertEqual(names, ["anchor"])

    def test_option_like_tab_titles_preserve_selector_ordering(self) -> None:
        started = self.start("tab-titles")
        target = ("--session", "tab-titles", "--tab", started["tab"])
        for prefix in ((), ("proc",)):
            for title in (
                "--text",
                "--paste",
                "--cwd",
                "--contains",
                "--focus",
                "--file",
            ):
                for arguments in ((title, *target), (*target, title)):
                    with self.subTest(prefix=prefix, title=title, arguments=arguments):
                        result = self.result(*prefix, "tab", "rename", *arguments)
                        self.assertEqual(result["command"], "tab.rename")
                        state = self.result("tab", "inspect", *target)["tab_state"]
                        self.assertEqual(state["title"], title)

    def test_option_like_tab_titles_do_not_swallow_help(self) -> None:
        for prefix in ((), ("proc",)):
            with self.subTest(prefix=prefix):
                result = self.command(*prefix, "tab", "rename", "--text", "--help")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                self.assertIn("tab rename", result.stdout)

    def test_resource_tab_and_pane_commands_keep_canonical_json(self) -> None:
        started = self.start("resources", 'IFS= read -r command; eval "$command"')
        target = ("--session", "resources")
        tab = self.result("tab", "new", *target, "--title", "second", "--", "/bin/sh")
        tab_target = (*target, "--tab", tab["tab"])
        self.result("tab", "rename", *tab_target, "renamed")
        self.result("tab", "move", *tab_target, "1")
        state = self.result("tab", "inspect", *tab_target)["tab_state"]
        self.assertEqual((state["title"], state["position"]), ("renamed", 1))
        self.result("tab", "select", *target, "--tab", started["tab"])
        listed = self.result("tab", "list", *target)
        self.assertEqual(len(listed["tabs"]), 2)
        pane_target = (*target, "--pane", started["pane"])
        self.result("pane", "send", *pane_target, "--text", "printf '__RESOURCE__\\n'")
        self.result("pane", "input", *pane_target, "--key", "enter")
        self.result("pane", "wait", *pane_target, "--exit-code", "0", "--timeout", "2s")
        capture = self.result("pane", "capture", *pane_target)
        proc_capture = self.result("proc", "pane", "capture", *pane_target)
        self.assertIn("__RESOURCE__", capture["capture"]["text"])
        self.assertEqual(capture["capture"]["text"], proc_capture["capture"]["text"])
        self.result("tab", "kill", *tab_target)
        self.assertEqual(len(self.result("pane", "list", *target)["panes"]), 1)

    def test_each_basic_layout_verb_changes_the_requested_state_in_both_output_modes(
        self,
    ) -> None:
        for structured in (False, True):
            with self.subTest(json=structured):
                name = f"layout-{int(structured)}"
                started = self.start(name)
                target = ("--session", name, "--pane", started["pane"])
                tab_target = ("--session", name, "--tab", started["tab"])

                def apply(verb: str, *arguments: str) -> str | dict[str, Any]:
                    if structured:
                        result = self.result(verb, *target, "--json", *arguments)
                        self.assertEqual(result["command"], f"pane.{verb}")
                        return result
                    output = self.ok(verb, *target, *arguments)
                    if verb != "split":
                        self.assertEqual(output, "")
                    return output

                split = apply(
                    "split", "--right", "--focus", "preserve", "--", "/bin/sh"
                )
                pane = split["pane"] if isinstance(split, dict) else split.strip()
                state = self.result("tab", "inspect", *tab_target)["tab_state"]
                self.assertEqual(state["focused_pane"], started["pane"])
                self.ok("focus", "--session", name, "--pane", pane)
                apply("focus")
                self.assertEqual(
                    self.result("tab", "inspect", *tab_target)["tab_state"][
                        "focused_pane"
                    ],
                    started["pane"],
                )
                apply("zoom", "--on")
                self.assertTrue(
                    self.result("tab", "inspect", *tab_target)["tab_state"]["zoomed"]
                )
                apply("zoom", "--off")
                self.assertFalse(
                    self.result("tab", "inspect", *tab_target)["tab_state"]["zoomed"]
                )
                before = self.result("tab", "inspect", *tab_target)["tab_state"][
                    "layout"
                ]
                apply("resize", "right", "4")
                resized = self.result("tab", "inspect", *tab_target)["tab_state"][
                    "layout"
                ]
                self.assertNotEqual(before, resized)
                apply("swap", pane)
                swapped = self.result("tab", "inspect", *tab_target)["tab_state"][
                    "layout"
                ]
                self.assertNotEqual(resized, swapped)
                apply("swap", pane)
                self.assertEqual(
                    self.result("tab", "inspect", *tab_target)["tab_state"]["layout"],
                    resized,
                )

    def test_context_targets_and_explicit_overrides_do_not_follow_focus(self) -> None:
        started = self.start("context", "printf 'LEFT\\n'")
        session = started["session"]["id"]
        right = self.result(
            "split",
            "--json",
            "--session",
            session,
            "--pane",
            started["pane"],
            "--right",
            "--hold",
            "--",
            "/bin/sh",
            "-c",
            "printf 'RIGHT\\n'",
        )
        for pane in (started["pane"], right["pane"]):
            self.ok("wait", "--session", session, "--pane", pane, "--timeout", "2s")
        environment = self.environment | {
            "LEMMA_SESSION_ID": session,
            "LEMMA_TAB_ID": started["tab"],
            "LEMMA_PANE_ID": started["pane"],
        }
        for arguments, expected in (
            (("capture",), "LEFT"),
            (("capture", "--pane", right["pane"]), "RIGHT"),
            (("capture", right["pane"]), "RIGHT"),
        ):
            result = self.command(*arguments, environment=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(expected, result.stdout)

    def test_wait_conditions_generation_and_capture_formats(self) -> None:
        started = self.start(
            "conditions",
            "printf '\\033]133;A\\007$ '; IFS= read -r line; printf '\\033]133;B\\007\\033]133;C\\007\\033[31m%s\\033[0m\\n\\033]133;D;0\\007' \"$line\"",
        )
        target = ("--session", "conditions", "--pane", started["pane"])
        prompt = self.result(
            "wait", *target, "--json", "--until-prompt", "--timeout", "2s"
        )
        generation = str(prompt["terminal_generation"])
        result = self.command(
            "wait",
            *target,
            "--until-prompt",
            "--after-generation",
            generation,
            "--timeout",
            "10ms",
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("timeout", result.stderr)
        self.ok("send", *target, "--paste", "colored-output", "--key", "enter")
        self.ok("wait", *target, "--exit-code", "0", "--timeout", "2s")
        for source in ("visible", "recent", "last-command"):
            for wrap in ("rendered", "logical"):
                with self.subTest(source=source, wrap=wrap):
                    plain = self.ok(
                        "capture", *target, "--source", source, "--wrap", wrap
                    )
                    self.assertIn("colored-output", plain)
                    self.assertNotIn("\x1b", plain)
                    ansi = self.ok(
                        "capture",
                        *target,
                        "--source",
                        source,
                        "--wrap",
                        wrap,
                        "--format",
                        "ansi",
                    )
                    self.assertIn("colored-output", ansi)
                    self.assertIn("\x1b[", ansi)
                    structured = self.result(
                        "capture", *target, "--source", source, "--wrap", wrap, "--json"
                    )
                    self.assertEqual(plain, structured["capture"]["text"])
        signal = self.start("signal", "kill -TERM $$")
        self.ok(
            "wait",
            "--session",
            "signal",
            "--pane",
            signal["pane"],
            "--signal",
            "15",
            "--timeout",
            "2s",
        )

    def test_failure_outputs_and_parser_bounds_do_not_mutate_state(self) -> None:
        started = self.start("invalid")
        target = ("--session", "invalid", "--pane", started["pane"])
        before = self.result("session", "inspect", "invalid")["session"]["revision"]
        for verb, options in (
            ("split", ("--right", "--json", "--json")),
            ("split", ("--right", "--hold", "--hold")),
            ("split", ("--right", "--focus", "invalid")),
            ("send", ()),
            ("send", ("--text", "")),
            ("send", ("--paste", "x", "--key", "invalid")),
            ("wait", ("--timeout", "0ms")),
            ("wait", ("--exit-code", "256")),
            ("wait", ("--signal", "0")),
            ("wait", ("--contains", "x", "--until-prompt")),
            ("wait", ("--after-generation", "1")),
            ("wait", ("--if-session-revision", str(before))),
            ("capture", ("--lines", "0")),
            ("capture", ("--lines", "65536")),
            ("capture", ("--format", "jpeg")),
            ("capture", ("--source", "last-command", "--lines", "1")),
            ("capture", ("--tab", "0:1")),
            ("focus", ("--pane", "1:1")),
            ("resize", ("right", "0")),
            ("resize", ("right", "101")),
            ("zoom", ("--on", "--off")),
            ("swap", ("bad-id",)),
        ):
            with self.subTest(verb=verb, options=options):
                result = self.command(verb, *target, *options)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertTrue(result.stderr)
        self.assertEqual(
            self.result("session", "inspect", "invalid")["session"]["revision"], before
        )
        stale = self.command(
            "capture", "--session", "invalid", "--pane", "63:99", "--json"
        )
        self.assertEqual(stale.returncode, 1)
        self.assertEqual(stale.stderr, "")
        self.assertEqual(
            json.loads(stale.stdout)["results"][0]["result"]["status"], "stale"
        )
        conflict = self.command(
            "split", *target, "--right", "--if-session-revision", str(before + 1)
        )
        self.assertEqual(conflict.returncode, 1)
        self.assertIn("revision_mismatch", conflict.stderr)
        offline = self.server.root / "offline"
        offline.mkdir()
        environment = self.environment | {"LEMMA_DEV_RUNTIME_DIR": str(offline)}
        for flags in ((), ("--json",)):
            result = self.command("capture", *target, *flags, environment=environment)
            self.assertEqual(result.returncode, 1)
            if flags:
                self.assertEqual(
                    json.loads(result.stdout)["error"]["reason"], "unavailable"
                )
                self.assertEqual(result.stderr, "")
            else:
                self.assertEqual(result.stdout, "")
                self.assertIn("unavailable", result.stderr)

    def test_proc_file_stdin_and_event_stream(self) -> None:
        started = self.start("anchor")
        document = json.loads(
            (Path(__file__).resolve().parents[2] / "examples/job.json").read_text()
        )
        for source in ("file", "stdin"):
            document["commands"][0]["name"] = f"job-{source}"
            encoded = json.dumps(document)
            if source == "file":
                path = self.server.root / "job.json"
                path.write_text(encoded)
                result = self.command("proc", "--file", str(path))
            else:
                result = self.command("proc", "--stdin", input=encoded)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            results = json.loads(result.stdout)["results"]
            self.assertEqual(
                [entry["result"]["status"] for entry in results], ["applied"] * 4
            )
            self.assertIn("hello from Lemma", results[2]["result"]["capture"]["text"])
        invalid = self.command("proc", "--stdin", input="not JSON")
        self.assertEqual(invalid.returncode, 2)
        self.assertEqual(json.loads(invalid.stdout)["error"]["reason"], "invalid_json")
        process = subprocess.Popen(
            [
                str(self.executable),
                "events",
                "--session",
                "anchor",
                "--pane",
                started["pane"],
                "--screen",
            ],
            env=self.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            assert process.stdout is not None
            ready, _, _ = select.select([process.stdout], [], [], 2.0)
            self.assertTrue(ready, "event stream did not produce its initial snapshot")
            event = json.loads(process.stdout.readline())
            self.assertEqual(event["event"], "snapshot")
            self.assertTrue(event["present"], event)
            self.assertEqual(event["pane"], started["pane"])
            self.assertIn("capture", event)
        finally:
            process.kill()
            process.communicate(timeout=2.0)

    def test_config_schema_skill_version_and_noninteractive_launch_guards(self) -> None:
        self.assertEqual(self.ok("version"), self.ok("--version"))
        self.assertTrue(self.ok("skill").startswith("---\nname: lemma\n"))
        self.assertEqual(
            json.loads(self.ok("api", "schema", "--json"))["$schema"],
            "https://json-schema.org/draft/2020-12/schema",
        )
        self.assertIn("Proc", self.ok("api", "schema"))
        configuration = (
            Path(__file__).resolve().parents[2] / "examples/configuration.lua"
        )
        self.assertIn("config ok", self.ok("config", "check", str(configuration)))
        invalid = self.server.root / "invalid.lua"
        invalid.write_text("not valid Lua!")
        self.assertEqual(self.command("config", "check", str(invalid)).returncode, 1)
        for arguments in ((), ("new", "requires-tty")):
            result = self.command(*arguments)
            self.assertEqual(result.returncode, 1)
            self.assertIn("requires a terminal", result.stderr)
        self.assertEqual(self.result("session", "list")["sessions"], [])

    def test_interactive_new_bare_launch_detach_and_attach_restore_the_terminal(
        self,
    ) -> None:
        self.start("anchor")
        for arguments in ((), ("new", "interactive", "--", "/bin/sh")):
            previous = {
                session["name"]
                for session in self.result("session", "list")["sessions"]
            }
            process = PtyProcess(
                [str(self.executable), *arguments],
                self.environment,
                terminal_restore_sequence=LEMMA_OUTER_TERMINAL_RESTORE,
            )
            self.addCleanup(process.close)
            process.read_until(ALT_SCREEN, 3.0, preserve_suffix=True)
            sessions = self.result("session", "list")["sessions"]
            created = [
                session["name"]
                for session in sessions
                if session["name"] not in previous
            ]
            self.assertEqual(len(created), 1)
            process.write_all(b"\x02d", 2.0)
            process.wait_for_exit(3.0)
            self.assertTrue(process.terminal_state_restored)
            attached = PtyProcess(
                [str(self.executable), "attach", created[0]],
                self.environment,
                terminal_restore_sequence=LEMMA_OUTER_TERMINAL_RESTORE,
            )
            self.addCleanup(attached.close)
            attached.read_until(ALT_SCREEN, 3.0, preserve_suffix=True)
            attached.write_all(b"\x02d", 2.0)
            attached.wait_for_exit(3.0)
            self.assertTrue(attached.terminal_state_restored)
            self.ok("kill", created[0])


if __name__ == "__main__":
    unittest.main()
