#!/usr/bin/env python3
"""Measure repeated same-revision noise against the reviewed paired policy."""

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
    load_object,
    require_scope,
)
from compare_regression import (
    micro_value,
    noise_floor,
    policy,
    process_value,
    profile_values,
    reports,
    require_manifest_identity,
    require_same_capture_scope,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest", type=Path, default=Path("benchmarks/workloads.json")
    )
    parser.add_argument(
        "--capture", type=Path, action="append", required=True, dest="captures"
    )
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def calibration(
    identifier: str,
    values: list[float],
    unit: str,
    section: str,
    section_policy: dict[str, Any],
    *,
    diagnostic: bool = False,
) -> dict[str, Any]:
    low = min(values)
    high = max(values)
    maximum_ratio = float(section_policy["maximum_ratio"])
    absolute_floor = noise_floor(section_policy, identifier, unit)
    allowed = low * maximum_ratio + absolute_floor
    required_floor = max(high - low * maximum_ratio, 0.0)
    return {
        "id": identifier,
        "section": section,
        "unit": unit,
        "samples": values,
        "minimum": low,
        "maximum": high,
        "observed_spread": high - low,
        "observed_maximum_ratio": high / low if low > 0 else None,
        "policy_maximum_ratio": maximum_ratio,
        "policy_absolute_noise_floor": absolute_floor,
        "policy_maximum": allowed,
        "minimum_floor_required_by_observations": math.ceil(required_floor),
        "status": (
            "diagnostic" if diagnostic else "passed" if high <= allowed else "failed"
        ),
    }


def main() -> int:
    arguments = parse_args()
    if len(arguments.captures) < 2:
        print("calibration requires at least two captures", file=sys.stderr)
        return 2
    try:
        manifest = load_object(arguments.manifest)
        paired = policy(manifest)
        budgets = budgets_from_manifest(manifest)
        loaded = [reports(path) for path in arguments.captures]
        manifest_sha256 = hashlib.sha256(arguments.manifest.read_bytes()).hexdigest()
        for capture_reports in loaded:
            require_manifest_identity(capture_reports, manifest_sha256)
            require_scope(budgets, *capture_reports)
        base_micro, base_process, base_profile = loaded[0]
        for index, (micro, process, profile) in enumerate(loaded[1:], 1):
            require_same_capture_scope(
                base_process, process, f"process capture {index}"
            )
            require_same_capture_scope(
                base_profile, profile, f"profile capture {index}"
            )
            if base_micro.get("context", {}).get("host_name") != micro.get(
                "context", {}
            ).get("host_name"):
                raise BudgetError("calibration microbenchmarks used different hosts")
            if base_micro.get("context", {}).get("manifest_sha256") != micro.get(
                "context", {}
            ).get("manifest_sha256"):
                raise BudgetError(
                    "calibration microbenchmarks used different manifests"
                )

        results: list[dict[str, Any]] = []
        micro_budget = budgets["microbenchmarks"]
        for check in micro_budget["checks"]:
            values = [
                micro_value(report[0], check, micro_budget["minimum_repetitions"])
                for report in loaded
            ]
            results.append(
                calibration(
                    check["id"],
                    values,
                    check["unit"],
                    "microbenchmarks",
                    paired["microbenchmarks"],
                )
            )

        process_budget = budgets["process_workloads"]
        for check in process_budget["checks"]:
            values = [
                process_value(report[1], check, process_budget["minimum_repetitions"])
                for report in loaded
            ]
            supported_values = [value for value in values if value is not None]
            if not supported_values:
                results.append(
                    {
                        "id": check["id"],
                        "section": "process_workloads",
                        "unit": check["unit"],
                        "status": "unsupported",
                    }
                )
                continue
            if len(supported_values) != len(values):
                raise BudgetError(
                    f"{check['id']} support changed between calibration captures"
                )
            results.append(
                calibration(
                    check["id"],
                    supported_values,
                    check["unit"],
                    "process_workloads",
                    paired["process_workloads"],
                    diagnostic=check["id"]
                    in paired["process_workloads"].get("diagnostic_ids", []),
                )
            )

        profile_captures = [
            {
                identifier: (value, unit)
                for identifier, value, unit in profile_values(
                    report[2], budgets["pane_profiles"]["minimum_repetitions"]
                )
            }
            for report in loaded
        ]
        expected_profile_metrics = profile_captures[0].keys()
        if any(
            capture.keys() != expected_profile_metrics
            for capture in profile_captures[1:]
        ):
            raise BudgetError("profile captures contain different measured metrics")
        for identifier, (first_value, unit) in profile_captures[0].items():
            values = [first_value]
            for capture in profile_captures[1:]:
                value, observed_unit = capture[identifier]
                if observed_unit != unit:
                    raise BudgetError(f"profile {identifier} changed units")
                values.append(value)
            results.append(
                calibration(
                    identifier,
                    values,
                    unit,
                    "pane_profiles",
                    paired["pane_profiles"],
                )
            )
    except (BudgetError, KeyError) as error:
        print(f"calibration error: {error}", file=sys.stderr)
        return 2

    failures = [result for result in results if result["status"] == "failed"]
    report = {
        "schema": 1,
        "suite": "frame-performance-calibration",
        "status": "failed" if failures else "passed",
        "capture_count": len(arguments.captures),
        "captures": [str(path) for path in arguments.captures],
        "host_fingerprint": base_process["host_fingerprint"],
        "results": results,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"performance calibration {report['status']}; report: {arguments.output}")
    for failure in failures:
        print(
            f"uncalibrated noise: {failure['id']}: maximum={failure['maximum']} "
            f"policy_maximum={failure['policy_maximum']} {failure['unit']}",
            file=sys.stderr,
        )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
