#!/usr/bin/env python3
"""Run and evaluate paired extension-off/extension-on process workloads."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import random
import shlex
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from benchmarks.benchmark_manifest import load_manifest  # noqa: E402

CASES = {
    "idle-control": ("idle-resources", None),
    "idle-peers": ("idle-resources", "idle-peers"),
    "idle-surfaces": ("idle-resources", "idle-surfaces"),
    "interactive-control": ("interactive-open-loop", None),
    "changing-rows": ("interactive-open-loop", "changing-rows"),
    "storm-interactive": ("interactive-open-loop", "storm"),
    "slow-producer": ("interactive-open-loop", "slow-producer"),
    "storm-renderer": ("idle-resources", "storm"),
    "blocked-reader": ("extension-isolation", "blocked-reader"),
    "crash-focused": ("extension-isolation", "crash-focused"),
    "crash-docked": ("extension-isolation", "crash-docked"),
}


def nested(document: dict[str, Any], *path: str) -> Any:
    value: Any = document
    for field in path:
        if not isinstance(value, dict) or field not in value:
            raise RuntimeError(f"report is missing {'.'.join(path)}")
        value = value[field]
    return value


def ratio(loaded: int | float, baseline: int | float, floor: int = 100_000) -> float:
    return float(loaded) / float(max(baseline, floor))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def workload(report: dict[str, Any], case: str) -> dict[str, Any]:
    workload_id = CASES[case][0].replace("-", "_")
    return nested(report, "workloads", workload_id)


def idle_metrics(report: dict[str, Any], case: str) -> dict[str, Any]:
    result = workload(report, case)
    daemon = nested(result, "roles", "daemon")
    daemon_wakeups = nested(daemon, "wakeups")
    return {
        "daemon_cpu_p95_ns": nested(daemon, "cpu_time", "p95_ns"),
        "daemon_rss_p95_bytes": nested(daemon, "rss", "p95_bytes"),
        "daemon_wakeups_p95": (
            daemon_wakeups.get("p95_count")
            if daemon_wakeups.get("available") is True
            else None
        ),
        "daemon_wakeups_reason": daemon_wakeups.get("reason"),
        "total_cpu_p95_ns": nested(result, "cpu_time", "p95_ns"),
        "total_rss_p95_bytes": nested(result, "rss", "p95_bytes"),
        "outer_bytes_per_second_p50": nested(
            result, "outer_throughput", "p50_bytes_per_second"
        ),
        "extension_processes": nested(
            result,
            "resources_after_workload",
            "roles",
            "extension_fixture",
            "process_count",
        )
        if CASES[case][1] is not None
        else 0,
        "fixture_ready": result.get("extension_fixture_ready", []),
        "retained_surfaces": sum(
            int(item.get("surfaces", 0))
            for item in result.get("extension_fixture_ready", [])
        ),
    }


def interactive_metrics(report: dict[str, Any], case: str) -> dict[str, Any]:
    result = workload(report, case)
    daemon_cpu = nested(result, "workload_cpu", "roles", "daemon")
    return {
        "key_to_pty_p50_ns": nested(result, "key_to_pty", "p50_ns"),
        "key_to_pty_p95_ns": nested(result, "key_to_pty", "p95_ns"),
        "key_to_outer_bytes_p50_ns": nested(result, "key_to_outer_bytes", "p50_ns"),
        "key_to_outer_bytes_p95_ns": nested(result, "key_to_outer_bytes", "p95_ns"),
        "key_to_outer_bytes_p99_ns": nested(result, "key_to_outer_bytes", "p99_ns"),
        "daemon_cpu_ns_per_operation": (
            daemon_cpu.get("cpu_ns_per_operation")
            if daemon_cpu.get("available") is True
            else None
        ),
        "extension_processes": nested(
            result,
            "resources_after_workload",
            "roles",
            "extension_fixture",
            "process_count",
        )
        if CASES[case][1] is not None
        else 0,
        "fixture_ready": result.get("extension_fixture_ready", []),
        "retained_surfaces": sum(
            int(item.get("surfaces", 0))
            for item in result.get("extension_fixture_ready", [])
        ),
    }


def isolation_metrics(report: dict[str, Any], case: str) -> dict[str, Any]:
    result = workload(report, case)
    telemetry = result.get("fixture_telemetry", [])
    return {
        "paste_payload_bytes": result.get("paste_payload_bytes"),
        "paste_submit_ns": result.get("paste_submit_ns"),
        "cleanup_to_outer_bytes_ns": result["cleanup_to_outer_bytes_ns"],
        "other_peer_proc_completions": sum(
            int(item.get("proc_completions", 0)) for item in telemetry
        ),
        "extension_processes_before_cleanup": nested(
            result,
            "activity_resources",
            "before",
            "roles",
            "extension_fixture",
            "process_count",
        ),
    }


def evaluate(
    reports: dict[str, dict[str, Any]], policy: dict[str, Any]
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    metrics: dict[str, Any] = {}
    checks: list[dict[str, Any]] = []
    limits = policy["limits"]

    for case in ("idle-control", "idle-peers", "idle-surfaces", "storm-renderer"):
        metrics[case] = idle_metrics(reports[case], case)
    for case in (
        "interactive-control",
        "changing-rows",
        "storm-interactive",
        "slow-producer",
    ):
        metrics[case] = interactive_metrics(reports[case], case)
    for case in ("blocked-reader", "crash-focused", "crash-docked"):
        metrics[case] = isolation_metrics(reports[case], case)

    def check(identifier: str, actual: int | float, maximum: int | float) -> None:
        checks.append(
            {
                "id": identifier,
                "actual": actual,
                "maximum": maximum,
                "passed": actual <= maximum,
            }
        )

    def minimum(identifier: str, actual: int | float, required: int | float) -> None:
        checks.append(
            {
                "id": identifier,
                "actual": actual,
                "minimum": required,
                "passed": actual >= required,
            }
        )

    fixture_cases = {
        "idle-peers": "idle-peers",
        "idle-surfaces": "idle-surfaces",
        "changing-rows": "changing-rows",
        "storm-interactive": "storm",
        "slow-producer": "slow-producer",
        "storm-renderer": "storm",
        "blocked-reader": "blocked-reader",
        "crash-focused": "crash-focused",
        "crash-docked": "crash-docked",
    }
    for case, fixture in fixture_cases.items():
        process_field = (
            "extension_processes_before_cleanup"
            if case in {"blocked-reader", "crash-focused", "crash-docked"}
            else "extension_processes"
        )
        actual = metrics[case][process_field]
        expected = policy["fixtures"][fixture]["processes"]
        checks.append(
            {
                "id": f"{case}.external_processes",
                "actual": actual,
                "expected": expected,
                "passed": actual == expected,
            }
        )

    control_idle = metrics["idle-control"]
    for case in ("idle-peers", "idle-surfaces"):
        selected = metrics[case]
        check(
            f"{case}.daemon_cpu_p95_ns",
            selected["daemon_cpu_p95_ns"],
            limits["idle_daemon_cpu_p95_ns_per_second"],
        )
        if selected["daemon_wakeups_p95"] is not None:
            check(
                f"{case}.daemon_wakeups_p95",
                selected["daemon_wakeups_p95"],
                limits["idle_daemon_wakeups_p95_per_second"],
            )
        check(
            f"{case}.daemon_rss_increase_bytes",
            selected["daemon_rss_p95_bytes"] - control_idle["daemon_rss_p95_bytes"],
            limits["idle_daemon_rss_increase_bytes"],
        )

    control_interactive = metrics["interactive-control"]
    for case in ("changing-rows", "storm-interactive"):
        selected = metrics[case]
        selected["key_to_outer_bytes_p95_ratio"] = ratio(
            selected["key_to_outer_bytes_p95_ns"],
            control_interactive["key_to_outer_bytes_p95_ns"],
        )
        check(
            f"{case}.key_to_outer_bytes_p95_ratio",
            selected["key_to_outer_bytes_p95_ratio"],
            limits["interactive_key_to_outer_bytes_p95_maximum_ratio"],
        )
    slow = metrics["slow-producer"]
    slow["initial_retained_input_bytes"] = (
        policy["fixtures"]["slow-producer"]["processes"]
        * policy["fixtures"]["slow-producer"][
            "initial_retained_input_bytes_per_process"
        ]
    )
    slow["key_to_outer_bytes_p95_ratio"] = ratio(
        slow["key_to_outer_bytes_p95_ns"],
        control_interactive["key_to_outer_bytes_p95_ns"],
    )
    check(
        "slow-producer.key_to_outer_bytes_p95_ratio",
        slow["key_to_outer_bytes_p95_ratio"],
        limits["slow_producer_key_to_outer_bytes_p95_maximum_ratio"],
    )
    minimum(
        "storm-renderer.outer_bytes_per_second_p50",
        metrics["storm-renderer"]["outer_bytes_per_second_p50"],
        limits["minimum_storm_outer_bytes_per_second"],
    )

    blocked = metrics["blocked-reader"]
    check(
        "blocked-reader.paste_submit_ns",
        blocked["paste_submit_ns"],
        limits["maximum_paste_submit_ns"],
    )
    minimum(
        "blocked-reader.other_peer_proc_completions",
        blocked["other_peer_proc_completions"],
        limits["minimum_blocked_other_peer_proc_completions"],
    )
    for case in ("blocked-reader", "crash-focused", "crash-docked"):
        check(
            f"{case}.cleanup_to_outer_bytes_ns",
            metrics[case]["cleanup_to_outer_bytes_ns"],
            limits["maximum_cleanup_to_outer_bytes_ns"],
        )
    return metrics, checks


def write_markdown(summary: dict[str, Any], path: Path) -> None:
    lines = [
        "# Extension performance acceptance",
        "",
        f"- Host: `{summary['host']}`",
        f"- Commit: `{summary['commit']}`",
        f"- Repetitions: {summary['repetitions']}",
        f"- Randomization seed: `{summary['randomization_seed']}`",
        f"- Status: **{summary['status']}**",
        "",
        "| Check | Actual | Bound | Result |",
        "| --- | ---: | ---: | --- |",
    ]
    for check in summary["checks"]:
        bound = (
            f"≤ {check['maximum']}"
            if "maximum" in check
            else (
                f"≥ {check['minimum']}"
                if "minimum" in check
                else f"= {check['expected']}"
            )
        )
        lines.append(
            f"| `{check['id']}` | {check['actual']} | {bound} | "
            f"{'pass' if check['passed'] else 'FAIL'} |"
        )
    lines.extend(["", "## Raw captures", ""])
    for case, capture in summary["captures"].items():
        lines.append(f"- `{case}`: `{capture['path']}` (`sha256:{capture['sha256']}`)")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest", type=Path, default=ROOT / "benchmarks/workloads.json"
    )
    parser.add_argument(
        "--benchmark", type=Path, default=ROOT / "benchmarks/mux_benchmark.py"
    )
    parser.add_argument(
        "--server", type=Path, default=ROOT / "build/release/frame_test_server"
    )
    parser.add_argument(
        "--cli", type=Path, default=ROOT / "build/release/frame_test_cli"
    )
    parser.add_argument(
        "--peer", type=Path, default=ROOT / "build/release/frame_test_pty_peer"
    )
    parser.add_argument(
        "--probe", type=Path, default=ROOT / "build/release/frame_benchmark_probe"
    )
    parser.add_argument(
        "--fixture", type=Path, default=ROOT / "benchmarks/extension_fixture.py"
    )
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--repetitions", type=int)
    parser.add_argument("--randomization-seed", type=int, default=0x1E77E57)
    arguments = parser.parse_args()

    manifest = load_manifest(arguments.manifest)
    policy = manifest["extension_performance"]
    repetitions = arguments.repetitions or policy["minimum_repetitions"]
    if repetitions < policy["minimum_repetitions"]:
        parser.error("repetitions are below the reviewed extension performance policy")
    host = platform.node().split(".")[0]
    if host != policy["approved_host"]:
        parser.error(
            f"extension performance acceptance requires host {policy['approved_host']!r}, got {host!r}"
        )
    for executable in (
        arguments.server,
        arguments.cli,
        arguments.peer,
        arguments.probe,
    ):
        if not executable.is_file() or executable.parent.name != "release":
            parser.error(
                f"extension acceptance requires a Release executable: {executable}"
            )
    if arguments.output_directory.exists():
        parser.error("output directory already exists")
    arguments.output_directory.mkdir(parents=True)

    order = list(CASES)
    random.Random(arguments.randomization_seed).shuffle(order)
    reports: dict[str, dict[str, Any]] = {}
    captures: dict[str, Any] = {}
    for case in order:
        mode, fixture = CASES[case]
        output = arguments.output_directory / f"{case}.json"
        command = [
            sys.executable,
            str(arguments.benchmark),
            "--manifest",
            str(arguments.manifest),
            "--mode",
            mode,
            "--multiplexer",
            "frame",
            "--intent",
            "gate",
            "--repetitions",
            str(repetitions),
            "--server",
            str(arguments.server),
            "--cli",
            str(arguments.cli),
            "--peer",
            str(arguments.peer),
            "--probe",
            str(arguments.probe),
            "--extension-fixture-path",
            str(arguments.fixture),
            "--output",
            str(output),
        ]
        if fixture is not None:
            command.extend(["--extension-fixture", fixture])
        command.append("--allow-workload-failures")
        subprocess.run(command, cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        report = json.loads(output.read_text(encoding="utf-8"))
        reports[case] = report
        captures[case] = {
            "path": output.name,
            "sha256": sha256(output),
            "command": shlex.join(command),
            "fixture": fixture,
        }

    outcomes = {
        case: {
            "status": workload(report, case).get("status"),
            "error": workload(report, case).get("error"),
        }
        for case, report in reports.items()
    }
    outcome_checks = [
        {
            "id": f"{case}.completion",
            "actual": outcome["status"],
            "expected": "completed",
            "passed": outcome["status"] == "completed",
        }
        for case, outcome in outcomes.items()
    ]
    if all(check["passed"] for check in outcome_checks):
        metrics, acceptance_checks = evaluate(reports, policy)
    else:
        metrics, acceptance_checks = {}, []
    checks = outcome_checks + acceptance_checks
    commits = {report["commit"] for report in reports.values()}
    if len(commits) != 1:
        raise RuntimeError(
            f"extension captures used different commits: {sorted(commits)}"
        )
    summary = {
        "schema": "frame.extension-performance/v1",
        "generated_at": datetime.now(UTC).isoformat(),
        "host": host,
        "commit": commits.pop(),
        "repetitions": repetitions,
        "randomization_seed": arguments.randomization_seed,
        "execution_order": order,
        "policy": policy,
        "metrics": metrics,
        "outcomes": outcomes,
        "checks": checks,
        "status": "accepted"
        if all(check["passed"] for check in checks)
        else "rejected",
        "captures": captures,
    }
    summary_path = arguments.output_directory / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    write_markdown(summary, arguments.output_directory / "summary.md")
    print(json.dumps(summary, indent=2))
    return 0 if summary["status"] == "accepted" else 1


if __name__ == "__main__":
    raise SystemExit(main())
