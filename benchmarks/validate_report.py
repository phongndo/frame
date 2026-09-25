#!/usr/bin/env python3
"""Validate benchmark reports against the executable workload manifest."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from benchmark_manifest import (
    ManifestError,
    comparison_sampling,
    expected_failure,
    load_manifest,
    workload_map,
)

REPORT_SCHEMA = 5
COMPARISON_SCHEMA = 3


class ReportError(ValueError):
    """A report cannot support the comparison it claims to describe."""


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReportError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ReportError(f"{path} must contain an object")
    return value


def validate_binary_records(report: dict[str, Any]) -> None:
    binaries = report.get("binaries")
    if not isinstance(binaries, dict) or not binaries:
        raise ReportError("process report contains no binary provenance")
    for role, record in binaries.items():
        if not isinstance(record, dict):
            raise ReportError(f"binary role {role} is not a provenance record")
        digest = record.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64:
            raise ReportError(f"binary role {role} has no SHA-256 identity")
        if not isinstance(record.get("bytes"), int) or record["bytes"] <= 0:
            raise ReportError(f"binary role {role} has an invalid byte size")


def sample_distributions(value: Any, prefix: str = "") -> list[tuple[str, list[Any]]]:
    if not isinstance(value, dict):
        return []
    distributions: list[tuple[str, list[Any]]] = []
    for key, child in value.items():
        label = f"{prefix}.{key}" if prefix else key
        if key.startswith("samples_") and isinstance(child, list) and child:
            distributions.append((label, child))
        elif isinstance(child, dict):
            distributions.extend(sample_distributions(child, label))
    return distributions


def validate_process_report(
    report: dict[str, Any],
    manifest: dict[str, Any],
    *,
    allow_failures: bool,
) -> None:
    if report.get("schema") != REPORT_SCHEMA:
        raise ReportError(f"process report must use schema {REPORT_SCHEMA}")
    subject = report.get("multiplexer")
    if (
        not isinstance(subject, str)
        or subject not in manifest["terminal_lab"]["subjects"]
    ):
        raise ReportError(f"process report has unknown subject {subject!r}")
    repetitions = report.get("repetitions")
    if not isinstance(repetitions, int) or repetitions < 1:
        raise ReportError("process report has an invalid repetition count")
    validity = report.get("statistics_valid")
    if not isinstance(validity, dict) or validity.get("p95") != (repetitions >= 20):
        raise ReportError("process report has incorrect p95 validity metadata")
    if validity.get("p99") != (repetitions >= 100):
        raise ReportError("process report has incorrect p99 validity metadata")
    load = report.get("host_load_average")
    maximum_load = report.get("maximum_gate_load_average_1m")
    if (
        not isinstance(load, list)
        or not load
        or not isinstance(load[0], (int, float))
        or isinstance(load[0], bool)
        or not isinstance(maximum_load, (int, float))
        or isinstance(maximum_load, bool)
        or report.get("environment_valid") != (load[0] <= maximum_load)
    ):
        raise ReportError("process report has invalid environment metadata")
    if report.get("run_intent") == "gate" and not report["environment_valid"]:
        raise ReportError("gate report was captured above the reviewed host load")
    validate_binary_records(report)

    scenarios = workload_map(manifest)
    selected = report.get("scenario_ids")
    workloads = report.get("workloads")
    if not isinstance(selected, list) or not isinstance(workloads, dict):
        raise ReportError("process report has no scenario selection")
    if set(selected) != set(workloads):
        raise ReportError("scenario_ids and workload results differ")
    for identifier in selected:
        scenario = scenarios.get(identifier)
        result = workloads.get(identifier)
        if scenario is None or not isinstance(result, dict):
            raise ReportError(f"workload {identifier} is not defined")
        status = result.get("status")
        supported = subject in scenario["subjects"]
        if not supported:
            if status != "unsupported":
                raise ReportError(f"unsupported workload {identifier} was not explicit")
            continue
        if status == "failed":
            error = result.get("error")
            if not allow_failures:
                raise ReportError(f"workload {identifier} failed: {error}")
            reviewed = expected_failure(
                manifest,
                subject,
                identifier,
                error if isinstance(error, str) else "",
            )
            if reviewed is None:
                raise ReportError(
                    f"{subject} workload {identifier} has an unreviewed failure: {error}"
                )
            if (
                result.get("failure_expected") is not True
                or result.get("failure_classification") != reviewed["classification"]
            ):
                raise ReportError(
                    f"workload {identifier} omitted its reviewed failure classification"
                )
            continue
        if status != "completed":
            raise ReportError(f"workload {identifier} did not complete")
        distributions = sample_distributions(result)
        if not distributions:
            raise ReportError(f"workload {identifier} retained no raw distributions")
        # Comparison-sampled workloads pool scaled blocks and declare their own count.
        expected = result.get("repetitions", repetitions)
        if not isinstance(expected, int) or expected < repetitions:
            raise ReportError(f"workload {identifier} has an invalid repetition count")
        for label, samples in distributions:
            if label.endswith("samples_ns") or label.endswith("samples_bytes"):
                if len(samples) not in {1, expected}:
                    raise ReportError(
                        f"workload {identifier}.{label} has {len(samples)} samples, "
                        f"expected 1 or {expected}"
                    )

    profiles = report.get("pane_profiles")
    if isinstance(profiles, dict) and profiles:
        intent = report.get("run_intent")
        profile_suites = manifest.get("profile_suites")
        if not isinstance(intent, str) or not isinstance(profile_suites, dict):
            raise ReportError("pane profile report has no manifest-owned profile suite")
        expected_profiles = profile_suites.get(intent)
        if not isinstance(expected_profiles, list) or set(profiles) != set(
            expected_profiles
        ):
            raise ReportError("pane profile report has invalid profile IDs")
        for profile, conditions in profiles.items():
            if not isinstance(conditions, dict) or set(conditions) != {
                "idle",
                "active",
            }:
                raise ReportError(f"pane profile {profile} has invalid conditions")
            for condition, result in conditions.items():
                if not isinstance(result, dict) or result.get("status") != "completed":
                    raise ReportError(
                        f"pane profile {profile}.{condition} did not complete"
                    )
                interaction = result.get("interaction")
                resources = result.get("resources")
                if not isinstance(interaction, dict) or not isinstance(resources, dict):
                    raise ReportError(
                        f"pane profile {profile}.{condition} is incomplete"
                    )
                for endpoint in ("key_to_pty", "key_to_outer_bytes"):
                    samples = interaction.get(endpoint, {}).get("samples_ns")
                    if not isinstance(samples, list) or len(samples) != repetitions:
                        raise ReportError(
                            f"pane profile {profile}.{condition}.{endpoint} "
                            "has an invalid distribution"
                        )

    intent = report.get("run_intent")
    for report_key, suites_key in (
        ("session_profiles", "session_profile_suites"),
        ("workspace_profiles", "workspace_profile_suites"),
    ):
        scaling_profiles = report.get(report_key)
        if not isinstance(scaling_profiles, dict) or not scaling_profiles:
            continue
        suites = manifest.get(suites_key)
        expected_profiles = suites.get(intent) if isinstance(suites, dict) else None
        if not isinstance(expected_profiles, list) or set(scaling_profiles) != set(
            expected_profiles
        ):
            raise ReportError(f"{report_key} report has invalid profile IDs")
        for profile, result in scaling_profiles.items():
            if not isinstance(result, dict) or result.get("status") != "completed":
                raise ReportError(f"{report_key}.{profile} did not complete")
            distributions = sample_distributions(result)
            if not distributions:
                raise ReportError(f"{report_key}.{profile} retained no distributions")
            for label, samples in distributions:
                if (
                    label.endswith(("samples_ns", "samples_bytes"))
                    and len(samples) != repetitions
                ):
                    raise ReportError(
                        f"{report_key}.{profile}.{label} has an invalid distribution"
                    )


def validate_comparison_report(
    report: dict[str, Any], manifest: dict[str, Any], *, allow_failures: bool
) -> None:
    if report.get("schema") != COMPARISON_SCHEMA:
        raise ReportError(f"comparison report must use schema {COMPARISON_SCHEMA}")
    results = report.get("results")
    expected = manifest["terminal_lab"]["subjects"]
    if (
        not isinstance(results, list)
        or [r.get("multiplexer") for r in results] != expected
    ):
        raise ReportError(
            "comparison report does not contain the complete ordered subject matrix"
        )
    execution_order = report.get("execution_order")
    scenarios = workload_map(manifest)
    expected_tasks = {
        (
            subject,
            identifier,
            "before" if subject == "direct" else "subject",
            block,
        )
        for identifier in manifest["suites"]["comparison"]
        for block in range(comparison_sampling(scenarios[identifier])[0])
        for subject in expected
        if subject in scenarios[identifier]["subjects"]
    }
    expected_tasks.update(
        ("direct", identifier, "after", block)
        for identifier in manifest["suites"]["comparison"]
        for block in range(comparison_sampling(scenarios[identifier])[0])
        if "direct" in scenarios[identifier]["subjects"]
    )
    observed_tasks = (
        {
            (
                task.get("subject"),
                task.get("workload"),
                task.get("phase"),
                task.get("block"),
            )
            for task in execution_order
            if isinstance(task, dict)
        }
        if isinstance(execution_order, list)
        else set()
    )
    if observed_tasks != expected_tasks or len(execution_order or []) != len(
        expected_tasks
    ):
        raise ReportError(
            "comparison report has no complete randomized execution order"
        )
    if report.get("run_intent") == "gate" and not report.get("environment_valid"):
        raise ReportError("comparison gate was captured above the reviewed host load")
    controls = report.get("direct_after_controls")
    expected_controls = {
        identifier
        for identifier in manifest["suites"]["comparison"]
        if "direct" in scenarios[identifier]["subjects"]
    }
    if not isinstance(controls, dict) or set(controls) != expected_controls:
        raise ReportError("comparison report has no complete direct after-controls")
    if any(
        not isinstance(result, dict) or result.get("status") != "completed"
        for result in controls.values()
    ):
        raise ReportError("a direct after-control did not complete")
    for result in results:
        validate_process_report(result, manifest, allow_failures=allow_failures)
        for identifier, workload in result["workloads"].items():
            blocks, scale = comparison_sampling(scenarios[identifier])
            if workload.get("status") != "completed" or (blocks, scale) == (1, 1):
                continue
            # A pooled distribution must contain every scaled block, never a surviving subset.
            if workload.get("repetitions") != result["repetitions"] * scale * blocks:
                raise ReportError(
                    f"{result['multiplexer']} workload {identifier} did not pool every block"
                )


def validate_micro_report(report: dict[str, Any]) -> None:
    rows = report.get("benchmarks")
    context = report.get("context")
    if not isinstance(rows, list) or not rows or not isinstance(context, dict):
        raise ReportError("native microbenchmark report is incomplete")
    if not context.get("source_commit") or not context.get("executable_sha256"):
        raise ReportError(
            "native microbenchmark report has incomplete source provenance"
        )
    failures = [row for row in rows if row.get("error_occurred") is True]
    if failures:
        raise ReportError(f"native microbenchmarks failed: {failures}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest", type=Path, default=Path("benchmarks/workloads.json")
    )
    parser.add_argument("--process", type=Path)
    parser.add_argument("--comparison", type=Path)
    parser.add_argument("--micro", type=Path)
    parser.add_argument("--allow-failures", action="store_true")
    arguments = parser.parse_args()
    if (
        sum(
            value is not None
            for value in (arguments.process, arguments.comparison, arguments.micro)
        )
        != 1
    ):
        parser.error("provide exactly one of --process, --comparison, or --micro")
    try:
        manifest = load_manifest(arguments.manifest)
        if arguments.process is not None:
            validate_process_report(
                load_object(arguments.process),
                manifest,
                allow_failures=arguments.allow_failures,
            )
        elif arguments.comparison is not None:
            validate_comparison_report(
                load_object(arguments.comparison),
                manifest,
                allow_failures=arguments.allow_failures,
            )
        else:
            assert arguments.micro is not None
            validate_micro_report(load_object(arguments.micro))
    except (ManifestError, ReportError) as error:
        print(f"benchmark report error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
