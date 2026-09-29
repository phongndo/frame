from __future__ import annotations

import os
import runpy
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load_runner() -> dict[str, Any]:
    return runpy.run_path(str(ROOT / "scripts" / "dev-run"), run_name="frame_dev_run")


class DevelopmentRunnerContractTest(unittest.TestCase):
    def test_just_and_dev_shell_frame_delegate_to_the_shared_runner(self) -> None:
        justfile = (ROOT / "justfile").read_text(encoding="utf-8")
        flake = (ROOT / "flake.nix").read_text(encoding="utf-8")
        self.assertIn('exec ./scripts/dev-run "$@"', justfile)
        self.assertIn('exec "$runner" "$@"', flake)

    def test_build_diagnostics_do_not_pollute_application_stdout(self) -> None:
        runner = load_runner()
        run_checked = runner["run_checked"]
        root = Path("/work/frame")
        environment = {"PATH": "/bin"}

        with mock.patch.object(subprocess, "run") as run:
            run_checked(["cmake", "--build", "build/dev"], root, environment)

        run.assert_called_once_with(
            ["cmake", "--build", "build/dev"],
            cwd=root,
            env=environment,
            check=True,
            stdout=sys.stderr,
        )

    def test_session_creation_defaults_to_the_invocation_directory(self) -> None:
        runner = load_runner()
        development_arguments = runner["development_arguments"]
        invocation_directory = Path("/work/frame")

        self.assertEqual(
            development_arguments([], invocation_directory),
            ["new", "--cwd", "/work/frame"],
        )
        self.assertEqual(
            development_arguments(["new", "work"], invocation_directory),
            ["new", "work", "--cwd", "/work/frame"],
        )
        self.assertEqual(
            development_arguments(
                ["start", "tests", "--", "just", "test"], invocation_directory
            ),
            [
                "start",
                "tests",
                "--cwd",
                "/work/frame",
                "--",
                "just",
                "test",
            ],
        )

    def test_explicit_cwd_and_non_creation_arguments_are_unchanged(self) -> None:
        runner = load_runner()
        development_arguments = runner["development_arguments"]
        invocation_directory = Path("/work/frame")

        explicit = ["new", "work", "--cwd", "/tmp"]
        command = ["proc", "session", "start"]
        self.assertEqual(
            development_arguments(explicit, invocation_directory), explicit
        )
        self.assertEqual(development_arguments(command, invocation_directory), command)

    def test_worktree_namespaces_are_stable_and_distinct(self) -> None:
        runner = load_runner()
        runtime_directory = runner["runtime_directory"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first"
            second = root / "second"
            first.mkdir()
            second.mkdir()
            self.assertEqual(
                runtime_directory(first, 1234), runtime_directory(first, 1234)
            )
            self.assertNotEqual(
                runtime_directory(first, 1234), runtime_directory(second, 1234)
            )
            self.assertIn("frame-dev-1234-", runtime_directory(first, 1234).name)

    def test_cached_configuration_builds_only_the_checkout_frame_target(self) -> None:
        runner = load_runner()
        prepare = runner["prepare"]
        fingerprint_for = runner["configuration_fingerprint"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            runtime = Path(temporary) / "runtime"
            build = root / "build" / "dev"
            for relative in runner["CONFIGURE_INPUTS"]:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative, encoding="utf-8")
            (build / "conan").mkdir(parents=True)
            (build / "build.ninja").touch()
            (build / "conan" / "conan_toolchain.cmake").touch()
            (build / "CMakeCache.txt").write_text(
                "CMAKE_BUILD_TYPE:STRING=Dev\n"
                "FRAME_BUILD_TESTS:BOOL=OFF\n"
                "FRAME_BUILD_BENCHMARKS:BOOL=OFF\n"
                "FRAME_VALIDATE_GHOSTTY_EVERY_BUILD:BOOL=OFF\n"
                f"CMAKE_HOME_DIRECTORY:INTERNAL={root.resolve()}\n",
                encoding="utf-8",
            )
            environment = {"PATH": os.environ.get("PATH", "")}
            fingerprint = fingerprint_for(root, environment)
            (build / ".frame-configure-signature").write_text(
                fingerprint + "\n", encoding="ascii"
            )
            (build / "frame").write_bytes(b"current checkout frame")

            calls: list[list[str]] = []
            prepare.__globals__["runtime_directory"] = lambda _root: runtime
            prepare.__globals__["run_checked"] = lambda arguments, _root, _environment: (
                calls.append(list(arguments))
            )
            prepare.__globals__["stop_stale_daemon"] = lambda _runtime, _build_id: None
            binary, arguments, execution_environment = prepare(
                root, ["pane", "split", "argument with spaces"], environment
            )

            self.assertEqual(
                calls,
                [["cmake", "--build", str(build), "--target", "frame"]],
            )
            self.assertEqual(binary, root / "build" / "dev" / "frame")
            self.assertEqual(
                arguments,
                [
                    str(root / "build" / "dev" / "frame"),
                    "pane",
                    "split",
                    "argument with spaces",
                ],
            )
            self.assertEqual(
                execution_environment["FRAME_DEV_RUNTIME_DIR"], str(runtime)
            )
            self.assertNotIn("/usr/bin/frame", arguments)

    def test_missing_configuration_runs_setup_once_before_incremental_build(
        self,
    ) -> None:
        runner = load_runner()
        prepare = runner["prepare"]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            runtime = Path(temporary) / "runtime"
            build = root / "build" / "dev"
            for relative in runner["CONFIGURE_INPUTS"]:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(relative, encoding="utf-8")
            environment = {"PATH": os.environ.get("PATH", "")}
            calls: list[list[str]] = []

            def run_checked(
                arguments: list[str], _root: Path, _environment: dict[str, str]
            ) -> None:
                calls.append(list(arguments))
                if arguments[0].endswith("scripts/ci/configure"):
                    (build / "conan").mkdir(parents=True)
                    (build / "build.ninja").touch()
                    (build / "conan" / "conan_toolchain.cmake").touch()
                    (build / "CMakeCache.txt").write_text(
                        "CMAKE_BUILD_TYPE:STRING=Dev\n"
                        "FRAME_BUILD_TESTS:BOOL=OFF\n"
                        "FRAME_BUILD_BENCHMARKS:BOOL=OFF\n"
                        "FRAME_VALIDATE_GHOSTTY_EVERY_BUILD:BOOL=OFF\n"
                        f"CMAKE_HOME_DIRECTORY:INTERNAL={root.resolve()}\n",
                        encoding="utf-8",
                    )
                else:
                    (build / "frame").write_bytes(b"configured frame")

            prepare.__globals__["runtime_directory"] = lambda _root: runtime
            prepare.__globals__["run_checked"] = run_checked
            prepare.__globals__["stop_stale_daemon"] = lambda _runtime, _build_id: None
            prepare(root, ["--version"], environment)
            prepare(root, ["--version"], environment)

            configure_calls = [
                call for call in calls if call[0].endswith("scripts/ci/configure")
            ]
            build_calls = [call for call in calls if call[0] == "cmake"]
            self.assertEqual(len(configure_calls), 1)
            self.assertEqual(len(build_calls), 2)
            self.assertTrue(
                all(call[-2:] == ["--target", "frame"] for call in build_calls)
            )

    def test_stale_daemon_is_shut_down_before_execution(self) -> None:
        runner = load_runner()
        stop_stale_daemon = runner["stop_stale_daemon"]
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary)
            socket_path = runtime / "daemon.sock"
            (runtime / "daemon.sock.build-id").write_text("0" * 64, encoding="ascii")
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(socket_path))
            listener.listen()
            shutdown_seen = threading.Event()

            def serve() -> None:
                try:
                    while True:
                        connection, _ = listener.accept()
                        with connection:
                            command = connection.recv(1)
                            if command == b"S":
                                shutdown_seen.set()
                                connection.sendall(b"frame daemon shut down\n")
                                break
                finally:
                    listener.close()
                    socket_path.unlink(missing_ok=True)

            server = threading.Thread(target=serve)
            server.start()
            stop_stale_daemon(runtime, "1" * 64)
            server.join(timeout=2.0)
            self.assertTrue(shutdown_seen.is_set())
            self.assertFalse(server.is_alive())


if __name__ == "__main__":
    unittest.main()
