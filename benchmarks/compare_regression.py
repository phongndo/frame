#!/usr/bin/env python3
"""Compare baseline and candidate distributions captured on one dedicated host."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any

from check_regression import (
    BudgetError,
    budgets_from_manifest,
    checked_samples,
    evaluate,
    load_object,
    process_check_samples,
    require_scope,
    statistic,
)


def policy(manifest: dict[str, Any]) -> dict[str, Any]:
    value = manifest.get("paired_regression")
    if not isinstance(value, dict) or value.get("schema") != 1:
        raise BudgetError("paired_regression must use schema 1")
    if value.get("status") != "reviewed":
        raise BudgetError("paired_regression.status must be reviewed")
    for section in ("microbenchmarks", "process_workloads", "pane_profiles"):
        section_value = value.get(section)
        if not isinstance(section_value, dict):
            raise BudgetError(f"paired_regression.{section} must be an object")
        ratio = section_value.get("maximum_ratio")
        if (
            not isinstance(ratio, (int, float))
            or isinstance(ratio, bool)
            or not math.isfinite(ratio)
            or ratio < 1
        ):
            raise BudgetError(f"paired_regression.{section}.maximum_ratio is invalid")
        floors = section_value.get("absolute_noise_floor")
        if not isinstance(floors, dict):
            raise BudgetError(
                f"paired_regression.{section}.absolute_noise_floor must be an object"
            )
        for unit, floor in floors.items():
            if (
                not isinstance(unit, str)
                or not isinstance(floor, (int, float))
                or isinstance(floor, bool)
                or not math.isfinite(floor)
                or floor < 0
            ):
                raise BudgetError(
                    f"paired_regression.{section}.absolute_noise_floor is invalid"
                )
        diagnostics = section_value.get("diagnostic_ids", [])
        if (
            not isinstance(diagnostics, list)
            or any(
                not isinstance(identifier, str) or not identifier
                for identifier in diagnostics
            )
            or len(diagnostics) != len(set(diagnostics))
        ):
            raise BudgetError(f"paired_regression.{section}.diagnostic_ids is invalid")
        overrides = section_value.get("absolute_noise_floor_by_id", {})
        if not isinstance(overrides, dict):
            raise BudgetError(
                f"paired_regression.{section}.absolute_noise_floor_by_id is invalid"
            )
        for identifier, floor in overrides.items():
            if (
                not isinstance(identifier, str)
                or not identifier
                or not isinstance(floor, (int, float))
                or isinstance(floor, bool)
                or not math.isfinite(floor)
                or floor < 0
            ):
                raise BudgetError(
                    f"paired_regression.{section}.absolute_noise_floor_by_id is invalid"
                )
    budgets = manifest.get("regression_budgets", {})
    for section in ("microbenchmarks", "process_workloads"):
        checks = budgets.get(section, {}).get("checks", [])
        known = {check.get("id") for check in checks if isinstance(check, dict)}
        unknown = set(value[section].get("diagnostic_ids", [])).difference(known)
        if unknown:
            raise BudgetError(
                f"paired_regression.{section}.diagnostic_ids contains unknown IDs"
            )
    return value


def noise_floor(section_policy: dict[str, Any], identifier: str, unit: str) -> float:
    overrides = section_policy.get("absolute_noise_floor_by_id", {})
    return float(
        overrides.get(identifier, section_policy["absolute_noise_floor"].get(unit, 0))
    )


def micro_value(report: dict[str, Any], check: dict[str, Any], minimum: int) -> float:
    rows = [
        row
        for row in report.get("benchmarks", [])
        if isinstance(row, dict)
        and row.get("name") == check["benchmark"]
        and row.get("run_type") == "iteration"
        and isinstance(row.get("repetition_index"), int)
    ]
    samples = checked_samples(
        [row.get(check["field"]) for row in rows], check["id"], minimum
    )
    if any(row.get("time_unit") != check["unit"] for row in rows):
        raise BudgetError(f"{check['id']} has an unexpected time unit")
    return statistic(samples, check["statistic"])


def process_value(
    report: dict[str, Any], check: dict[str, Any], minimum: int
) -> float | None:
    samples = process_check_samples(report, check, minimum)
    return statistic(samples, check["statistic"]) if samples is not None else None


def profile_values(
    report: dict[str, Any], minimum: int
) -> list[tuple[str, float, str]]:
    values: list[tuple[str, float, str]] = []
    profiles = report.get("pane_profiles")
    if not isinstance(profiles, dict):
        raise BudgetError("profile report has no pane_profiles")
    for profile in sorted(profiles):
        for condition in ("idle", "active"):
            measured = profiles.get(profile, {}).get(condition, {})
            if measured.get("status") != "completed":
                raise BudgetError(f"profile {profile}.{condition} did not complete")
            resources = measured.get("resources", {})
            interaction = measured.get("interaction", {})
            fields = (
                (
                    "rss_p95",
                    resources.get("rss", {}).get("samples_bytes"),
                    "p95",
                    "bytes",
                ),
                (
                    "cpu_time_p95",
                    resources.get("cpu_time", {}).get("samples_ns"),
                    "p95",
                    "ns",
                ),
                (
                    "key_to_pty_p95",
                    interaction.get("key_to_pty", {}).get("samples_ns"),
                    "p95",
                    "ns",
                ),
                (
                    "key_to_outer_bytes_p50",
                    interaction.get("key_to_outer_bytes", {}).get("samples_ns"),
                    "p50",
                    "ns",
                ),
                (
                    "key_to_outer_bytes_p95",
                    interaction.get("key_to_outer_bytes", {}).get("samples_ns"),
                    "p95",
                    "ns",
                ),
            )
            for suffix, raw, statistic_name, unit in fields:
                identifier = f"{profile}.{condition}.{suffix}"
                samples = checked_samples(raw, identifier, minimum)
                values.append((identifier, statistic(samples, statistic_name), unit))
    return values


def require_manifest_identity(
    reports: tuple[dict[str, Any], dict[str, Any], dict[str, Any]], expected: str
) -> None:
    micro, process, profile = reports
    identities = (
        micro.get("context", {}).get("manifest_sha256"),
        process.get("manifest", {}).get("sha256"),
        profile.get("manifest", {}).get("sha256"),
    )
    if any(identity != expected for identity in identities):
        raise BudgetError("capture reports do not use the selected workload manifest")


def require_same_capture_scope(
    baseline: dict[str, Any], candidate: dict[str, Any], label: str
) -> None:
    if (
        baseline.get("environment_valid") is not True
        or candidate.get("environment_valid") is not True
    ):
        raise BudgetError(
            f"{label} capture environment was outside its reviewed bounds"
        )
    for field in ("host", "host_fingerprint", "build_profile", "run_intent"):
        if baseline.get(field) != candidate.get(field):
            raise BudgetError(f"{label} reports differ in {field}")
    baseline_manifest = baseline.get("manifest", {})
    candidate_manifest = candidate.get("manifest", {})
    if baseline_manifest.get("sha256") != candidate_manifest.get("sha256"):
        raise BudgetError(f"{label} reports used different workload manifests")


def add_comparison(
    results: list[dict[str, Any]],
    identifier: str,
    baseline: float,
    candidate: float,
    unit: str,
    section_policy: dict[str, Any],
    *,
    diagnostic: bool = False,
) -> None:
    ratio = float(section_policy["maximum_ratio"])
    floor = noise_floor(section_policy, identifier, unit)
    maximum = baseline * ratio + floor
    results.append(
        {
            "id": identifier,
            "baseline": baseline,
            "candidate": candidate,
            "maximum": maximum,
            "maximum_ratio": ratio,
            "absolute_noise_floor": floor,
            "unit": unit,
            "status": (
                "diagnostic"
                if diagnostic
                else "passed"
                if candidate <= maximum
                else "failed"
            ),
        }
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest", type=Path, default=Path("benchmarks/workloads.json")
    )
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def reports(directory: Path) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    return (
        load_object(directory / "microbenchmarks.json"),
        load_object(directory / "process-workloads.json"),
        load_object(directory / "pane-profiles.json"),
    )


def main() -> int:
    arguments = parse_args()
    try:
        manifest = load_object(arguments.manifest)
        paired = policy(manifest)
        budgets = budgets_from_manifest(manifest)
        manifest_sha256 = hashlib.sha256(arguments.manifest.read_bytes()).hexdigest()
        baseline_reports = reports(arguments.baseline)
        candidate_reports = reports(arguments.candidate)
        require_manifest_identity(baseline_reports, manifest_sha256)
        require_manifest_identity(candidate_reports, manifest_sha256)
        base_micro, base_process, base_profile = baseline_reports
        cand_micro, cand_process, cand_profile = candidate_reports
        require_scope(budgets, *baseline_reports)
        require_same_capture_scope(base_process, cand_process, "process")
        require_same_capture_scope(base_profile, cand_profile, "profile")
        base_context = base_micro.get("context", {})
        candidate_context = cand_micro.get("context", {})
        for field in (
            "host_name",
            "num_cpus",
            "cpu_scaling_enabled",
            "host_cpu_model",
            "host_memory_bytes",
            "host_model_identifier",
            "host_physical_cpu_count",
            "manifest_sha256",
        ):
            if base_context.get(field) != candidate_context.get(field):
                raise BudgetError(f"microbenchmark reports differ in {field}")

        results: list[dict[str, Any]] = []
        micro_budget = budgets["microbenchmarks"]
        for check in micro_budget["checks"]:
            add_comparison(
                results,
                check["id"],
                micro_value(base_micro, check, micro_budget["minimum_repetitions"]),
                micro_value(cand_micro, check, micro_budget["minimum_repetitions"]),
                check["unit"],
                paired["microbenchmarks"],
            )

        process_budget = budgets["process_workloads"]
        for check in process_budget["checks"]:
            baseline_value = process_value(
                base_process, check, process_budget["minimum_repetitions"]
            )
            candidate_value = process_value(
                cand_process, check, process_budget["minimum_repetitions"]
            )
            if baseline_value is None or candidate_value is None:
                if baseline_value is not None or candidate_value is not None:
                    raise BudgetError(
                        f"{check['id']} support differs between baseline and candidate"
                    )
                results.append(
                    {
                        "id": check["id"],
                        "unit": check["unit"],
                        "status": "unsupported",
                    }
                )
                continue
            add_comparison(
                results,
                check["id"],
                baseline_value,
                candidate_value,
                check["unit"],
                paired["process_workloads"],
                diagnostic=check["id"]
                in paired["process_workloads"].get("diagnostic_ids", []),
            )

        base_profiles = {
            identifier: (value, unit)
            for identifier, value, unit in profile_values(
                base_profile, budgets["pane_profiles"]["minimum_repetitions"]
            )
        }
        candidate_profiles = {
            identifier: (value, unit)
            for identifier, value, unit in profile_values(
                cand_profile, budgets["pane_profiles"]["minimum_repetitions"]
            )
        }
        if base_profiles.keys() != candidate_profiles.keys():
            raise BudgetError("profile reports contain different measured metrics")
        for identifier, (candidate, unit) in candidate_profiles.items():
            baseline, baseline_unit = base_profiles[identifier]
            if baseline_unit != unit:
                raise BudgetError(f"profile {identifier} changed units")
            add_comparison(
                results,
                identifier,
                baseline,
                candidate,
                unit,
                paired["pane_profiles"],
            )

        target_results = evaluate(budgets, cand_micro, cand_process, cand_profile)
    except BudgetError as error:
        print(f"paired regression error: {error}", file=sys.stderr)
        return 2

    failed = [result for result in results if result["status"] == "failed"]
    target_failed = [
        result for result in target_results if result["status"] == "failed"
    ]
    report = {
        "schema": 1,
        "suite": "frame-paired-regression",
        "status": "failed" if failed else "passed",
        "target_status": "failed" if target_failed else "passed",
        "comparisons": results,
        "target_checks": target_results,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    for result in failed:
        print(
            f"paired regression failed: {result['id']}: "
            f"{result['candidate']} > {result['maximum']} {result['unit']}",
            file=sys.stderr,
        )
    for result in target_failed:
        print(
            f"product target remains unmet: {result['id']}: "
            f"{result['observed']} > {result['maximum']} {result['unit']}",
            file=sys.stderr,
        )
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
