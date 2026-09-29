#!/usr/bin/env python3
"""Prove selected regression detectors reject real source faults in an isolated worktree."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Fault:
    name: str
    path: str
    before: str
    after: str
    target: str
    detector: str


FAULTS = (
    Fault(
        "child-wakeup",
        "src/core/engine.cpp",
        ".fd = child_reaper.wake_descriptor, .events = POLLIN, .revents = 0",
        ".fd = child_reaper.wake_descriptor, .events = 0, .revents = 0",
        "frame_unit_tests",
        "ReactorEnvironmentTest.ChildWakeCanPrecedeAcceptAndFragmentedRequest",
    ),
    Fault(
        "partial-write",
        "src/core/pty_writer.cpp",
        "const bool consumed = queue.consume(size);",
        "const bool consumed = queue.consume(bytes.size());",
        "frame_terminal_boundary_tests",
        "PtyWriterTest.ConsumesOnlyPartialWritesAndRecoversAfterEagain",
    ),
    Fault(
        "resize-delivery",
        "src/platform/pty.cpp",
        "return ::ioctl(pty_descriptor, TIOCSWINSZ, &native_size) == 0;",
        "return ::ioctl(pty_descriptor, TIOCGWINSZ, &native_size) == 0;",
        "frame_component_integration_tests",
        "PlatformPtyTest.ResizeReachesSlaveGeometry",
    ),
    Fault(
        "stale-id",
        "include/frame/generational_store.hpp",
        "return slot.value != nullptr && slot.generation == id.generation();",
        "return slot.value != nullptr;",
        "frame_unit_tests",
        "BoundedGenerationalStoreTest.RejectsStaleIdsAndReportsCapacity",
    ),
    Fault(
        "steady-allocation",
        "src/core/client_frame_output.cpp",
        "  auto* const output = target.output;",
        "  void* (*volatile detection_allocate)(std::size_t) = &::operator new;\n"
        "  ::operator delete(detection_allocate(1));\n"
        "  auto* const output = target.output;",
        "frame_steady_state_allocation_audit",
        "general_allocation_calls",
    ),
)
SLOW_DISPATCH = Fault(
    "slow-dispatch",
    "src/core/command.cpp",
    "auto CommandDispatcher::dispatch(const Command& command) const noexcept -> CommandResult {",
    "auto CommandDispatcher::dispatch(const Command& command) const noexcept -> CommandResult {\n"
    "  for (std::uint32_t detection_index = 0; detection_index < 100'000; ++detection_index) {\n"
    "    const volatile auto detection_work = detection_index;\n"
    "    static_cast<void>(detection_work);\n"
    "  }",
    "frame_benchmarks",
    "command_dispatch_cpu_p95",
)


def replacement(source: str, fault: Fault) -> str:
    if source.count(fault.before) != 1:
        raise ValueError(f"{fault.name}: source anchor must match exactly once")
    return source.replace(fault.before, fault.after, 1)


@contextmanager
def mutated(tree: Path, fault: Fault) -> Iterator[None]:
    path = tree / fault.path
    original = path.read_bytes()
    changed = replacement(original.decode(), fault)
    try:
        path.write_text(changed, encoding="utf-8")
        yield
    finally:
        path.write_bytes(original)


def run(tree: Path, command: list[str], log: Path, timeout: int) -> int:
    """Bound the entire process group; a timeout is never a detected mutation."""
    print(f"+ {' '.join(command)} (log: {log})", flush=True)
    with log.open("w", encoding="utf-8") as output:
        output.write(json.dumps(command) + "\n")
        output.flush()
        process = subprocess.Popen(
            command,
            cwd=tree,
            stdout=output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            return process.wait(timeout=timeout)
        except BaseException:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise


def require_success(tree: Path, command: list[str], log: Path, timeout: int) -> None:
    if run(tree, command, log, timeout) != 0:
        raise RuntimeError(f"setup/control failed; see {log}")


@contextmanager
def snapshot(root: Path, output: Path) -> Iterator[Path]:
    """Copy the working diff and nonignored new files, never edit the source checkout."""
    tree = Path(tempfile.mkdtemp(prefix="frame-detection-"))
    tree.rmdir()
    subprocess.run(
        ["git", "worktree", "add", "--detach", str(tree), "HEAD"], cwd=root, check=True
    )
    try:
        patch = subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=root)
        (output / "working.patch").write_bytes(patch)
        if patch:
            subprocess.run(
                ["git", "apply", "--binary"], cwd=tree, input=patch, check=True
            )
        untracked = subprocess.check_output(
            ["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=root
        )
        for raw in untracked.split(b"\0"):
            if not raw:
                continue
            relative = Path(os.fsdecode(raw))
            destination = tree / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / relative, destination, follow_symlinks=False)
        yield tree
    finally:
        subprocess.run(
            ["git", "worktree", "remove", "--force", str(tree)], cwd=root, check=True
        )


def read_object(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"not a JSON object: {path}")
    return value


def gtest_outcome(report: dict, detector: str, failed: bool) -> bool:
    suite_name, test_name = detector.split(".", 1)
    cases = [
        case
        for suite in report.get("testsuites", [])
        if suite.get("name") == suite_name
        for case in suite.get("testsuite", [])
        if case.get("name") == test_name
    ]
    return (
        report.get("tests") == 1
        and report.get("errors") == 0
        and len(cases) == 1
        and cases[0].get("status") == "RUN"
        and cases[0].get("result") == "COMPLETED"
        and bool(cases[0].get("failures")) == failed
        and report.get("failures") == int(failed)
    )


def allocation_outcome(report: dict, failed: bool) -> bool:
    return (
        report.get("schema") == 2
        and report.get("suite") == "steady-state-allocation-audit"
        and report.get("status") == ("failed" if failed else "passed")
        and report.get("audited_iterations") == 10_000
        and (report.get("general_allocation_calls", 0) > 0) == failed
    )


def paired_outcome(report: dict, failed: bool) -> bool:
    checks = [
        check
        for check in report.get("comparisons", [])
        if check.get("id") == SLOW_DISPATCH.detector
    ]
    return (
        report.get("schema") == 1
        and report.get("suite") == "frame-paired-regression"
        and report.get("status") == ("failed" if failed else "passed")
        and len(checks) == 1
        and checks[0].get("status") == ("failed" if failed else "passed")
        and (not failed or checks[0]["candidate"] > checks[0]["maximum"])
    )


def native_check(
    tree: Path, fault: Fault, directory: Path, failed: bool, timeout: int
) -> None:
    directory.mkdir()
    targets = [fault.target]
    if fault.name == "steady-allocation":
        targets.append("frame_unit_tests")
    require_success(
        tree,
        ["cmake", "--build", "build/debug", "--target", *targets],
        directory / "build.log",
        timeout,
    )
    if fault.name == "steady-allocation":
        raw = tree / "build/debug/steady-state-work-audit.json"
        raw.unlink(missing_ok=True)
        code = run(
            tree,
            ["scripts/ci/deterministic-budgets", "debug"],
            directory / "test.log",
            timeout,
        )
        report = read_object(raw)
        shutil.copy2(raw, directory / "result.json")
        valid = allocation_outcome(report, failed)
    else:
        report_path = directory / "result.json"
        code = run(
            tree,
            [
                f"build/debug/{fault.target}",
                f"--gtest_filter={fault.detector}",
                f"--gtest_output=json:{report_path}",
            ],
            directory / "test.log",
            timeout,
        )
        valid = gtest_outcome(read_object(report_path), fault.detector, failed)
    if code != int(failed) or not valid:
        raise RuntimeError(
            f"{'fault survived or wrong failure' if failed else 'control failed'}: {fault.name}; see {directory}"
        )


def performance_check(tree: Path, directory: Path, failed: bool, timeout: int) -> None:
    directory.mkdir()
    capture = directory / "gate"
    code = run(
        tree,
        ["scripts/performance", "gate", "HEAD", str(capture)],
        directory / "gate.log",
        timeout,
    )
    report = read_object(capture / "paired-regression.json")
    if code != int(failed) or not paired_outcome(report, failed):
        raise RuntimeError(
            f"performance {'fault survived or wrong failure' if failed else 'control failed'}; see {directory}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", action="append", choices=[f.name for f in FAULTS])
    parser.add_argument(
        "--performance",
        action="store_true",
        help="run real A/A and slowed-candidate gates on the approved host (requires clean tracked sources)",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--timeout",
        type=int,
        default=1800,
        help="seconds per native build/check; performance gates allow at least 4 hours",
    )
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.performance and args.case:
        parser.error("--performance and --case are separate runs")
    if (
        args.performance
        and subprocess.run(["git", "diff", "--quiet", "HEAD"], cwd=ROOT).returncode
    ):
        parser.error("commit tracked changes before the performance A/A control")
    (ROOT / "build").mkdir(exist_ok=True)
    output = args.output or Path(
        tempfile.mkdtemp(prefix="detection-", dir=ROOT / "build")
    )
    if args.output:
        output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    selected = (
        [SLOW_DISPATCH]
        if args.performance
        else [f for f in FAULTS if not args.case or f.name in args.case]
    )
    results: dict = {
        "schema": 1,
        "suite": "frame-detection-checks",
        "status": "incomplete",
        "cases": [],
        "revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
    }
    try:
        with snapshot(ROOT, output) as tree:
            for fault in selected:
                source = (tree / fault.path).read_text(encoding="utf-8")
                replacement(source, fault)
                results["cases"].append(
                    {
                        "id": fault.name,
                        "detector": fault.detector,
                        "path": fault.path,
                        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                        "before": fault.before,
                        "after": fault.after,
                        "status": "pending",
                    }
                )
            if not args.performance:
                require_success(
                    tree,
                    [
                        "scripts/ci/configure",
                        "debug",
                        "-DFRAME_BUILD_TESTS=ON",
                        "-DFRAME_BUILD_BENCHMARKS=OFF",
                    ],
                    output / "configure.log",
                    args.timeout,
                )
            for fault, result in zip(selected, results["cases"], strict=True):
                control = output / f"{fault.name}-control"
                negative = output / f"{fault.name}-fault"
                if args.performance:
                    performance_check(tree, control, False, max(args.timeout, 14_400))
                else:
                    native_check(tree, fault, control, False, args.timeout)
                result["status"] = "control-passed"
                with mutated(tree, fault):
                    if args.performance:
                        performance_check(
                            tree, negative, True, max(args.timeout, 14_400)
                        )
                    else:
                        native_check(tree, fault, negative, True, args.timeout)
                result["status"] = "detected"
            results["status"] = "passed"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        results["status"] = "failed"
        results["error"] = str(error)
        print(error, file=sys.stderr)
    finally:
        (output / "results.json").write_text(
            json.dumps(results, indent=2) + "\n", encoding="utf-8"
        )
    print(f"detection checks {results['status']}: {output}")
    return 0 if results["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
