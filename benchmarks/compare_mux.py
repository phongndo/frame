#!/usr/bin/env python3
"""Run one manifest-defined workload suite against the complete mux subject matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from benchmark_manifest import comparison_sampling, load_manifest, unsupported_result
from validate_report import validate_process_report


def resolve_executable(value: str) -> Path:
    resolved = shutil.which(value)
    path = Path(resolved or value)
    if not path.is_file():
        raise RuntimeError(f"missing executable: {value}")
    return path.resolve()


def percentile(samples: list[int], quantile: float) -> int:
    ordered = sorted(samples)
    index = max(0, min(len(ordered) - 1, math.ceil(quantile * len(ordered)) - 1))
    return ordered[index]


def distribution_summary(samples: list[int], block_p50: list[int]) -> dict[str, Any]:
    """Summarize samples pooled from independently launched, interleaved blocks."""
    return {
        "samples_ns": samples,
        "p50_ns": percentile(samples, 0.50),
        "p90_ns": percentile(samples, 0.90),
        "p95_ns": percentile(samples, 0.95),
        "p99_ns": percentile(samples, 0.99),
        "p95_valid": len(samples) >= 20,
        "p99_valid": len(samples) >= 100,
        # Per-block medians expose host-state regimes that pooling would otherwise hide.
        "block_p50_ns": block_p50,
    }


def merge_blocks(blocks: list[dict[str, Any]]) -> dict[str, Any]:
    """Pool block distributions; any failed block fails the workload with every block retained."""
    if len(blocks) == 1:
        return blocks[0]
    completed = [block for block in blocks if block.get("status") == "completed"]
    if len(completed) != len(blocks):
        failed = next(block for block in blocks if block.get("status") != "completed")
        return {**failed, "completed_blocks": len(completed), "blocks": blocks}
    merged: dict[str, Any] = {"status": "completed"}
    for key, value in blocks[0].items():
        values = [block.get(key) for block in blocks]
        if isinstance(value, dict) and isinstance(value.get("samples_ns"), list):
            summaries = [item for item in values if isinstance(item, dict)]
            if len(summaries) != len(blocks) or not all(
                isinstance(item.get("samples_ns"), list) for item in summaries
            ):
                raise ValueError(f"comparison blocks disagree on distribution {key}")
            merged[key] = distribution_summary(
                [sample for item in summaries for sample in item["samples_ns"]],
                [item["p50_ns"] for item in summaries],
            )
        elif all(item == value for item in values):
            merged[key] = value
    # Block-varying values such as batch CPU and resource snapshots remain per block.
    merged["blocks"] = blocks
    return merged


def distributions(
    value: Any, prefix: tuple[str, ...] = ()
) -> dict[tuple[str, ...], list[int]]:
    if not isinstance(value, dict):
        return {}
    result: dict[tuple[str, ...], list[int]] = {}
    for name, child in value.items():
        path = (*prefix, name)
        if (
            (name.startswith("samples_") or name == "outer_bytes")
            and isinstance(child, list)
            and all(
                isinstance(sample, int) and not isinstance(sample, bool)
                for sample in child
            )
        ):
            result[path] = child
        elif isinstance(child, dict) and name != "resources_after_workload":
            result.update(distributions(child, path))
    return result


def bootstrap_median_delta(
    baseline: list[int], contender: list[int], seed: int, resamples: int = 2_000
) -> tuple[int, int]:
    generator = random.Random(seed)
    deltas = []
    for _ in range(resamples):
        baseline_sample = [generator.choice(baseline) for _ in baseline]
        contender_sample = [generator.choice(contender) for _ in contender]
        deltas.append(
            percentile(contender_sample, 0.50) - percentile(baseline_sample, 0.50)
        )
    return percentile(deltas, 0.025), percentile(deltas, 0.975)


def direct_deltas(
    reports: dict[str, dict[str, Any]], thresholds: dict[str, float]
) -> list[dict[str, Any]]:
    direct = reports["direct"].get("workloads", {})
    analysis: list[dict[str, Any]] = []
    for subject, report in reports.items():
        if subject == "direct":
            continue
        for workload, result in report.get("workloads", {}).items():
            baseline = direct.get(workload)
            if (
                not isinstance(result, dict)
                or not isinstance(baseline, dict)
                or result.get("status") != "completed"
                or baseline.get("status") != "completed"
            ):
                continue
            baseline_distributions = distributions(baseline)
            for path, samples in distributions(result).items():
                baseline_samples = baseline_distributions.get(path)
                if not baseline_samples or not samples:
                    continue
                direct_p50 = percentile(baseline_samples, 0.50)
                subject_p50 = percentile(samples, 0.50)
                identity = f"{subject}:{workload}:{'.'.join(path)}"
                seed = int.from_bytes(
                    hashlib.sha256(identity.encode("utf-8")).digest()[:8], "big"
                )
                confidence_low, confidence_high = bootstrap_median_delta(
                    baseline_samples, samples, seed
                )
                ratio = subject_p50 / direct_p50 if direct_p50 > 0 else None
                practical_change = (
                    abs(subject_p50 - direct_p50)
                    >= thresholds["minimum_latency_change_ns"]
                    and ratio is not None
                    and abs(ratio - 1.0) >= thresholds["minimum_relative_change"]
                    if path[-1] == "samples_ns"
                    else None
                )
                analysis.append(
                    {
                        "subject": subject,
                        "workload": workload,
                        "metric_path": list(path),
                        "direct_p50": direct_p50,
                        "subject_p50": subject_p50,
                        "added_p50": subject_p50 - direct_p50,
                        "added_p50_confidence_95": [confidence_low, confidence_high],
                        "p50_ratio": ratio,
                        "practical_change": practical_change,
                    }
                )
    return analysis


def direct_control_drift(
    direct_report: dict[str, Any], after_controls: dict[str, dict[str, Any]]
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for workload, after in after_controls.items():
        before = direct_report["workloads"].get(workload)
        if (
            not isinstance(before, dict)
            or before.get("status") != "completed"
            or after.get("status") != "completed"
        ):
            continue
        before_distributions = distributions(before)
        for path, after_samples in distributions(after).items():
            before_samples = before_distributions.get(path)
            if not before_samples or not after_samples:
                continue
            before_p50 = percentile(before_samples, 0.50)
            after_p50 = percentile(after_samples, 0.50)
            result.append(
                {
                    "workload": workload,
                    "metric_path": list(path),
                    "before_p50": before_p50,
                    "after_p50": after_p50,
                    "drift_p50": after_p50 - before_p50,
                    "drift_ratio": after_p50 / before_p50 if before_p50 > 0 else None,
                }
            )
    return result


def execution_plan(
    blocks: list[tuple[dict[str, Any], int]],
    subjects: list[str],
    generator: random.Random,
) -> list[dict[str, Any]]:
    """Bracket each shuffled block with direct controls around shuffled subjects."""
    execution_order: list[dict[str, Any]] = []
    for workload, block in blocks:
        supported = [
            subject
            for subject in subjects
            if subject != "direct" and subject in workload["subjects"]
        ]
        generator.shuffle(supported)
        phases = [("subject", subject) for subject in supported]
        if "direct" in workload["subjects"]:
            phases = [("before", "direct"), *phases, ("after", "direct")]
        execution_order.extend(
            {
                "subject": subject,
                "workload": workload["id"],
                "phase": phase,
                "block": block,
            }
            for phase, subject in phases
        )
    return execution_order


def run_subject_workload(
    harness: Path,
    subject: str,
    workload: dict[str, Any],
    arguments: argparse.Namespace,
    executables: dict[str, Path],
    destination: Path,
) -> dict[str, Any]:
    command = [
        sys.executable,
        str(harness),
        "--multiplexer",
        subject,
        "--mode",
        workload["cli_mode"],
        "--intent",
        arguments.intent,
        "--manifest",
        str(arguments.manifest),
        "--peer",
        str(arguments.peer.resolve()),
        "--probe",
        str(arguments.probe.resolve()),
        "--repetitions",
        str(arguments.repetitions * comparison_sampling(workload)[1]),
        "--output",
        str(destination),
    ]
    if subject == "lemma":
        command.extend(
            [
                "--server",
                str(arguments.server.resolve()),
                "--cli",
                str(arguments.cli.resolve()),
            ]
        )
    elif subject != "direct":
        command.extend([f"--{subject}", str(executables[subject])])
    if subject not in {"direct", "lemma"}:
        command.append("--allow-workload-failures")
    with destination.with_suffix(".stderr.log").open("w", encoding="utf-8") as stderr:
        subprocess.run(
            command,
            check=True,
            timeout=1_200.0,
            stdout=subprocess.DEVNULL,
            stderr=stderr,
        )
    report = json.loads(destination.read_text(encoding="utf-8"))
    validate_process_report(
        report,
        load_manifest(arguments.manifest),
        allow_failures=subject not in {"direct", "lemma"},
    )
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest", type=Path, default=Path("benchmarks/workloads.json")
    )
    parser.add_argument(
        "--server", type=Path, default=Path("build/release/lemma_test_server")
    )
    parser.add_argument(
        "--cli", type=Path, default=Path("build/release/lemma_test_cli")
    )
    parser.add_argument(
        "--peer", type=Path, default=Path("build/release/lemma_test_pty_peer")
    )
    parser.add_argument(
        "--probe", type=Path, default=Path("build/release/lemma_benchmark_probe")
    )
    parser.add_argument("--tmux", default="tmux")
    parser.add_argument("--zellij", default="zellij")
    parser.add_argument("--herdr", default="herdr")
    parser.add_argument("--repetitions", type=int)
    parser.add_argument(
        "--intent", choices=("smoke", "extended", "gate", "manual"), default="manual"
    )
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    for path in (arguments.server, arguments.cli, arguments.peer, arguments.probe):
        if not path.is_file():
            parser.error(f"missing executable: {path}")

    fragment_root: Path | None = None
    try:
        manifest = load_manifest(arguments.manifest)
        if arguments.repetitions is None:
            policy_name = arguments.intent if arguments.intent != "manual" else "smoke"
            arguments.repetitions = manifest["sample_policies"][policy_name][
                "process_repetitions"
            ]
        if arguments.repetitions < 1 or arguments.repetitions > 10_000:
            parser.error("--repetitions must be between 1 and 10000")
        if any(
            arguments.repetitions * comparison_sampling(workload)[1] > 10_000
            for workload in manifest["process_workloads"]
        ):
            parser.error("scaled comparison repetitions must not exceed 10000")
        subjects = list(manifest["terminal_lab"]["subjects"])
        workloads = [
            workload
            for workload in manifest["process_workloads"]
            if workload["id"] in manifest["suites"]["comparison"]
        ]
        generator = random.Random(arguments.seed)
        blocks = [
            (workload, block)
            for workload in workloads
            for block in range(comparison_sampling(workload)[0])
        ]
        generator.shuffle(blocks)
        execution_order = execution_plan(blocks, subjects, generator)
        executables = {
            "tmux": resolve_executable(arguments.tmux),
            "zellij": resolve_executable(arguments.zellij),
            "herdr": resolve_executable(arguments.herdr),
        }
        harness = Path(__file__).with_name("mux_benchmark.py").resolve()
        block_results: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
        base_reports: dict[str, dict[str, Any]] = {}
        environment_valid = True
        workload_by_id = {workload["id"]: workload for workload in workloads}
        evidence_directory = arguments.output.with_suffix(".fragments")
        evidence_directory.mkdir(parents=True, exist_ok=True)
        # Never overwrite an earlier attempt or delete a rejected fragment.
        fragment_root = Path(tempfile.mkdtemp(prefix="run-", dir=evidence_directory))
        print(f"comparison fragments: {fragment_root}", file=sys.stderr)
        (fragment_root / "execution.json").write_text(
            json.dumps(
                {
                    "schema": 1,
                    "run_intent": arguments.intent,
                    "seed": arguments.seed,
                    "repetitions": arguments.repetitions,
                    "manifest_sha256": hashlib.sha256(
                        arguments.manifest.read_bytes()
                    ).hexdigest(),
                    "execution_order": execution_order,
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        for index, task in enumerate(execution_order):
            subject = task["subject"]
            workload = workload_by_id[task["workload"]]
            fragment = run_subject_workload(
                harness,
                subject,
                workload,
                arguments,
                executables,
                fragment_root / f"{index:03d}-{subject}-{workload['id']}.json",
            )
            environment_valid = environment_valid and bool(
                fragment.get("environment_valid")
            )
            base_reports.setdefault(subject, fragment)
            phase = "after" if task["phase"] == "after" else "subject"
            block_results.setdefault((subject, workload["id"], phase), []).append(
                fragment["workloads"][workload["id"]]
            )
        reports: dict[str, dict[str, Any]] = {}
        direct_after_controls: dict[str, dict[str, Any]] = {}
        for (subject, identifier, phase), results in block_results.items():
            merged = merge_blocks(results)
            blocks_declared, scale = comparison_sampling(workload_by_id[identifier])
            if blocks_declared > 1 or scale > 1:
                # Validation checks pooled distributions against this per-workload count.
                merged["repetitions"] = arguments.repetitions * scale * len(results)
            if phase == "after":
                direct_after_controls[identifier] = merged
                continue
            if subject not in reports:
                reports[subject] = {
                    **base_reports[subject],
                    "repetitions": arguments.repetitions,
                    "statistics_valid": {
                        "p50": True,
                        "p95": arguments.repetitions >= 20,
                        "p99": arguments.repetitions >= 100,
                    },
                    "scenario_ids": [],
                    "workloads": {},
                }
            reports[subject]["scenario_ids"].append(identifier)
            reports[subject]["workloads"][identifier] = merged
        scenario_order = [workload["id"] for workload in workloads]
        for subject in subjects:
            report = reports[subject]
            for workload in workloads:
                if subject not in workload["subjects"]:
                    report["workloads"][workload["id"]] = unsupported_result(
                        workload, subject
                    )
            report["scenario_ids"] = scenario_order
            report["workloads"] = {
                identifier: report["workloads"][identifier]
                for identifier in scenario_order
            }
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        location = (
            f"; comparison fragments retained in {fragment_root}"
            if fragment_root
            else ""
        )
        parser.error(f"{error}{location}")

    report = {
        "schema": 3,
        "suite": "core-mux-comparison",
        "run_intent": arguments.intent,
        "statistics_valid": {
            "p50": True,
            "p95": arguments.repetitions >= 20,
            "p99": arguments.repetitions >= 100,
        },
        "environment_valid": environment_valid,
        "manifest_sha256": hashlib.sha256(arguments.manifest.read_bytes()).hexdigest(),
        "seed": arguments.seed,
        "execution_order": execution_order,
        "fragments_directory": str(fragment_root),
        "policy": (
            "Workload blocks are randomized, non-direct subjects are randomized within "
            "each block, and direct controls bracket every supported block; all use an "
            "identical native probe, fixture, dimensions, and completion endpoint. "
            "Workloads with comparison_sampling run several independently launched "
            "blocks interleaved across the run and pool their raw distributions."
        ),
        "results": [reports[subject] for subject in subjects],
        "direct_after_controls": direct_after_controls,
        "direct_control_drift": direct_control_drift(
            reports["direct"], direct_after_controls
        ),
        "direct_baseline_deltas": direct_deltas(
            reports,
            manifest["comparison_policy"]["practical_effect_thresholds"],
        ),
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
