#!/usr/bin/env python3
"""Validate and evaluate the pinned-host F0 regression budgets."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

from performance_host import load_policy, validate_fingerprint


class BudgetError(ValueError):
    """The manifest or a benchmark report cannot support a budget decision."""


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BudgetError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise BudgetError(f"{path} must contain a JSON object")
    return value


def require_int(value: Any, label: str, *, minimum: int = 0) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise BudgetError(f"{label} must be an integer >= {minimum}")
    return value


def require_number(value: Any, label: str) -> float:
    if (
        not isinstance(value, (int, float))
        or isinstance(value, bool)
        or not math.isfinite(value)
    ):
        raise BudgetError(f"{label} must be a finite number")
    return float(value)


def require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise BudgetError(f"{label} must be a non-empty string")
    return value


def require_decimal_int(value: Any, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, str) and value.isascii() and value.isdecimal():
        try:
            value = int(value)
        except ValueError as error:
            raise BudgetError(
                f"{label} is outside the supported integer range"
            ) from error
    return require_int(value, label, minimum=minimum)


def percentile(samples: list[float], quantile: float) -> float:
    ordered = sorted(samples)
    index = max(0, min(len(ordered) - 1, math.ceil(quantile * len(ordered)) - 1))
    return ordered[index]


def statistic(samples: list[float], name: str) -> float:
    if name == "p50":
        return percentile(samples, 0.50)
    if name == "p95":
        return percentile(samples, 0.95)
    if name == "p99":
        return percentile(samples, 0.99)
    if name == "max":
        return max(samples)
    raise BudgetError(f"unsupported statistic {name!r}")


def value_at_path(root: dict[str, Any], path: list[str], label: str) -> Any:
    value: Any = root
    for component in path:
        if not isinstance(value, dict) or component not in value:
            raise BudgetError(f"{label} cannot resolve {'.'.join(path)}")
        value = value[component]
    return value


def checked_samples(value: Any, label: str, minimum_samples: int) -> list[float]:
    if not isinstance(value, list) or len(value) < minimum_samples:
        actual = len(value) if isinstance(value, list) else 0
        raise BudgetError(
            f"{label} needs at least {minimum_samples} samples; found {actual}"
        )
    samples = [require_number(sample, f"{label} sample") for sample in value]
    if any(sample < 0 for sample in samples):
        raise BudgetError(f"{label} samples must be non-negative")
    return samples


def process_check_samples(
    report: dict[str, Any], check: dict[str, Any], minimum_samples: int
) -> list[float] | None:
    raw_samples = value_at_path(report, check["samples_path"], check["id"])
    if check.get("availability", "required") == "when_supported":
        capability = value_at_path(
            report, check["samples_path"][:-1], f"{check['id']} capability"
        )
        if not isinstance(capability, dict) or not isinstance(
            capability.get("available"), bool
        ):
            raise BudgetError(f"{check['id']} has no explicit availability status")
        if capability["available"] is False:
            if raw_samples != [] or not isinstance(capability.get("reason"), str):
                raise BudgetError(f"{check['id']} has malformed unsupported evidence")
            return None
    return checked_samples(raw_samples, check["id"], minimum_samples)


def require_completed_process_workloads(
    process_report: dict[str, Any],
    checks: list[dict[str, Any]],
    comparative_checks: list[dict[str, Any]] | None = None,
) -> None:
    workloads = process_report.get("workloads")
    if not isinstance(workloads, dict):
        raise BudgetError("process report has no workloads")
    required = {str(check["samples_path"][1]) for check in checks}
    for check in comparative_checks or []:
        required.add(str(check["baseline_samples_path"][1]))
        required.add(str(check["loaded_samples_path"][1]))
    for workload_name in sorted(required):
        workload = workloads.get(workload_name)
        if not isinstance(workload, dict) or workload.get("status") != "completed":
            raise BudgetError(f"process workload {workload_name} did not complete")


def validate_check(check: Any, label: str, *, micro: bool) -> dict[str, Any]:
    if not isinstance(check, dict):
        raise BudgetError(f"{label} must be an object")
    require_string(check.get("id"), f"{label}.id")
    require_string(check.get("statistic"), f"{label}.statistic")
    if check["statistic"] not in {"p50", "p95", "p99", "max"}:
        raise BudgetError(f"{label}.statistic is unsupported")
    maximum = require_number(check.get("maximum"), f"{label}.maximum")
    if maximum < 0:
        raise BudgetError(f"{label}.maximum must be non-negative")
    require_string(check.get("unit"), f"{label}.unit")
    availability = check.get("availability", "required")
    if availability not in {"required", "when_supported"}:
        raise BudgetError(f"{label}.availability is unsupported")
    if micro and availability != "required":
        raise BudgetError(f"{label}.availability is only valid for process checks")
    if micro:
        require_string(check.get("benchmark"), f"{label}.benchmark")
        if check.get("field") not in {"cpu_time", "real_time"}:
            raise BudgetError(f"{label}.field must be cpu_time or real_time")
    else:
        path = check.get("samples_path")
        if not isinstance(path, list) or len(path) < 3 or path[0] != "workloads":
            raise BudgetError(
                f"{label}.samples_path must identify a field under a process workload"
            )
        for index, component in enumerate(path):
            require_string(component, f"{label}.samples_path[{index}]")
    return check


def validate_comparative_check(check: Any, label: str) -> dict[str, Any]:
    if not isinstance(check, dict):
        raise BudgetError(f"{label} must be an object")
    require_string(check.get("id"), f"{label}.id")
    require_string(check.get("statistic"), f"{label}.statistic")
    if check["statistic"] not in {"p50", "p95", "p99", "max"}:
        raise BudgetError(f"{label}.statistic is unsupported")
    maximum = require_number(check.get("maximum_ratio"), f"{label}.maximum_ratio")
    if maximum < 1:
        raise BudgetError(f"{label}.maximum_ratio must be at least 1")
    for path_name in ("baseline_samples_path", "loaded_samples_path"):
        path = check.get(path_name)
        if not isinstance(path, list) or len(path) < 3 or path[0] != "workloads":
            raise BudgetError(
                f"{label}.{path_name} must identify a process workload field"
            )
        for index, component in enumerate(path):
            require_string(component, f"{label}.{path_name}[{index}]")
    return check


def budgets_from_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    if manifest.get("schema") != 4:
        raise BudgetError("workload manifest must use schema 4")
    budgets = manifest.get("regression_budgets")
    if not isinstance(budgets, dict) or budgets.get("schema") != 1:
        raise BudgetError("regression_budgets must use schema 1")
    if budgets.get("status") != "reviewed":
        raise BudgetError("regression_budgets.status must be reviewed")

    scope = budgets.get("scope")
    if not isinstance(scope, dict):
        raise BudgetError("regression_budgets.scope must be an object")
    process_requirements = scope.get("process_report_requirements")
    micro_requirements = scope.get("micro_context_requirements")
    if not isinstance(process_requirements, dict) or not process_requirements:
        raise BudgetError(
            "scope.process_report_requirements must be a non-empty object"
        )
    if not isinstance(micro_requirements, dict) or not micro_requirements:
        raise BudgetError("scope.micro_context_requirements must be a non-empty object")
    host_name = require_string(scope.get("approved_host"), "scope.approved_host")
    try:
        approved_host = load_policy(host_name)
    except ValueError as error:
        raise BudgetError(str(error)) from error
    maximum_load = require_number(
        scope.get("maximum_load_average_1m"), "scope.maximum_load_average_1m"
    )
    if maximum_load <= 0:
        raise BudgetError("scope.maximum_load_average_1m must be positive")

    micro = budgets.get("microbenchmarks")
    process = budgets.get("process_workloads")
    profiles = budgets.get("pane_profiles")
    for value, label in (
        (micro, "microbenchmarks"),
        (process, "process_workloads"),
        (profiles, "pane_profiles"),
    ):
        if not isinstance(value, dict):
            raise BudgetError(f"regression_budgets.{label} must be an object")
        require_int(
            value.get("minimum_repetitions"), f"{label}.minimum_repetitions", minimum=2
        )

    micro_checks = micro.get("checks")
    process_checks = process.get("checks")
    comparative_checks = process.get("comparative_checks", [])
    if not isinstance(micro_checks, list) or not micro_checks:
        raise BudgetError("microbenchmarks.checks must be a non-empty array")
    if not isinstance(process_checks, list) or not process_checks:
        raise BudgetError("process_workloads.checks must be a non-empty array")
    if not isinstance(comparative_checks, list):
        raise BudgetError("process_workloads.comparative_checks must be an array")
    for index, check in enumerate(micro_checks):
        validate_check(check, f"microbenchmarks.checks[{index}]", micro=True)
    for index, check in enumerate(process_checks):
        validate_check(check, f"process_workloads.checks[{index}]", micro=False)
    for index, check in enumerate(comparative_checks):
        validate_comparative_check(
            check, f"process_workloads.comparative_checks[{index}]"
        )
    identifiers = [
        check["id"] for check in [*micro_checks, *process_checks, *comparative_checks]
    ]
    if len(identifiers) != len(set(identifiers)):
        raise BudgetError("regression budget check IDs must be unique")

    limits = profiles.get("maximum_p95_rss_bytes")
    conditions = profiles.get("conditions")
    if not isinstance(limits, dict) or set(limits) != {"P1", "P4", "P16", "PMAX"}:
        raise BudgetError("pane_profiles.maximum_p95_rss_bytes has invalid profile IDs")
    for profile, maximum in limits.items():
        require_int(
            maximum, f"pane_profiles.maximum_p95_rss_bytes.{profile}", minimum=1
        )
    if not isinstance(conditions, dict) or set(conditions) != {"idle", "active"}:
        raise BudgetError("pane_profiles.conditions must contain idle and active")
    required_limits = {
        "maximum_p95_cpu_time_ns",
        "maximum_p95_key_to_pty_ns",
        "maximum_p50_key_to_outer_bytes_ns",
        "maximum_p95_key_to_outer_bytes_ns",
    }
    for condition, limits_for_condition in conditions.items():
        if not isinstance(limits_for_condition, dict) or not required_limits.issubset(
            limits_for_condition
        ):
            raise BudgetError(f"pane_profiles.conditions.{condition} is incomplete")
        for name in required_limits:
            require_int(
                limits_for_condition[name],
                f"pane_profiles.conditions.{condition}.{name}",
            )
    return {**budgets, "scope": {**scope, "approved_host": approved_host}}


def add_result(
    results: list[dict[str, Any]],
    identifier: str,
    samples: list[float],
    statistic_name: str,
    maximum: float,
    unit: str,
) -> None:
    observed = statistic(samples, statistic_name)
    results.append(
        {
            "id": identifier,
            "samples": len(samples),
            "statistic": statistic_name,
            "observed": observed,
            "maximum": maximum,
            "unit": unit,
            "status": "passed" if observed <= maximum else "failed",
        }
    )


def require_scope(
    budgets: dict[str, Any],
    micro_report: dict[str, Any],
    process_report: dict[str, Any],
    profile_report: dict[str, Any],
) -> None:
    scope = budgets["scope"]
    expected_host = scope["approved_host"]
    process_requirements = {
        **scope["process_report_requirements"],
        "system": expected_host["system"],
        "architecture": expected_host["architecture"],
    }
    for field, expected in process_requirements.items():
        if (
            process_report.get(field) != expected
            or profile_report.get(field) != expected
        ):
            raise BudgetError(
                f"reports are outside the reviewed scope: {field} must be {expected!r}"
            )
    context = micro_report.get("context")
    if not isinstance(context, dict):
        raise BudgetError("microbenchmark report has no context")
    micro_requirements = {
        **scope["micro_context_requirements"],
        "num_cpus": expected_host["logical_cpu_count"],
    }
    for field, expected in micro_requirements.items():
        if context.get(field) != expected:
            raise BudgetError(
                f"microbenchmark report is outside scope: {field} must be {expected!r}"
            )

    for report, label in ((process_report, "process"), (profile_report, "profile")):
        failures = validate_fingerprint(report.get("host_fingerprint"), expected_host)
        if failures:
            raise BudgetError(
                f"{label} report did not come from the approved pinned host: "
                + "; ".join(failures)
            )
    micro_host = {
        "host_name": context.get("host_name"),
        "model_identifier": context.get("host_model_identifier"),
        "cpu_model": context.get("host_cpu_model"),
        "physical_cpu_count": require_decimal_int(
            context.get("host_physical_cpu_count"),
            "microbenchmark context host_physical_cpu_count",
            minimum=1,
        ),
        "memory_bytes": require_decimal_int(
            context.get("host_memory_bytes"),
            "microbenchmark context host_memory_bytes",
            minimum=1,
        ),
    }
    failures = validate_fingerprint(micro_host, expected_host)
    if failures:
        raise BudgetError(
            "microbenchmark report did not come from the approved pinned host: "
            + "; ".join(failures)
        )
    if (
        micro_host != process_report["host_fingerprint"]
        or micro_host != profile_report["host_fingerprint"]
    ):
        raise BudgetError("benchmark reports came from different host fingerprints")
    maximum_load = float(scope["maximum_load_average_1m"])
    micro_load = context.get("load_avg")
    if (
        not isinstance(micro_load, list)
        or not micro_load
        or require_number(micro_load[0], "microbenchmark load average") > maximum_load
    ):
        raise BudgetError("native report was captured above the reviewed host load")
    for report, label in ((process_report, "process"), (profile_report, "profile")):
        load = report.get("host_load_average")
        if (
            not isinstance(load, list)
            or not load
            or require_number(load[0], f"{label} load average") > maximum_load
        ):
            raise BudgetError(
                f"{label} report was captured above the reviewed host load"
            )

    if process_report.get("host") != profile_report.get("host"):
        raise BudgetError("process and profile reports came from different hosts")
    if context.get("host_name") != process_report.get("host"):
        raise BudgetError(
            "microbenchmark and process reports came from different hosts"
        )
    if process_report.get("commit") != profile_report.get("commit"):
        raise BudgetError("process and profile reports came from different commits")
    if context.get("source_commit") != process_report.get("commit"):
        raise BudgetError("native and process reports came from different commits")
    executable_sha256 = context.get("executable_sha256")
    if not isinstance(executable_sha256, str) or len(executable_sha256) != 64:
        raise BudgetError("native report has no executable SHA-256 identity")
    process_manifest = process_report.get("manifest")
    profile_manifest = profile_report.get("manifest")
    if (
        not isinstance(process_manifest, dict)
        or not isinstance(profile_manifest, dict)
        or process_manifest.get("sha256") != profile_manifest.get("sha256")
        or context.get("manifest_sha256") != process_manifest.get("sha256")
    ):
        raise BudgetError("benchmark reports used different workload manifests")


def evaluate(
    budgets: dict[str, Any],
    micro_report: dict[str, Any],
    process_report: dict[str, Any],
    profile_report: dict[str, Any],
) -> list[dict[str, Any]]:
    require_scope(budgets, micro_report, process_report, profile_report)
    results: list[dict[str, Any]] = []

    micro_budget = budgets["microbenchmarks"]
    micro_minimum = micro_budget["minimum_repetitions"]
    benchmark_rows = micro_report.get("benchmarks")
    if not isinstance(benchmark_rows, list):
        raise BudgetError("microbenchmark report contains no benchmarks")
    for check in micro_budget["checks"]:
        rows = [
            row
            for row in benchmark_rows
            if isinstance(row, dict)
            and row.get("name") == check["benchmark"]
            and row.get("run_type") == "iteration"
            and isinstance(row.get("repetition_index"), int)
        ]
        samples = checked_samples(
            [row.get(check["field"]) for row in rows], check["id"], micro_minimum
        )
        if any(row.get("time_unit") != check["unit"] for row in rows):
            raise BudgetError(f"{check['id']} has an unexpected time unit")
        add_result(
            results,
            check["id"],
            samples,
            check["statistic"],
            float(check["maximum"]),
            check["unit"],
        )

    process_budget = budgets["process_workloads"]
    process_minimum = process_budget["minimum_repetitions"]
    if (
        process_report.get("schema") != 5
        or process_report.get("multiplexer") != "frame"
    ):
        raise BudgetError("process workload report must be a schema-5 Frame report")
    require_int(
        process_report.get("repetitions"),
        "process report repetitions",
        minimum=process_minimum,
    )
    require_completed_process_workloads(
        process_report,
        process_budget["checks"],
        process_budget.get("comparative_checks", []),
    )
    for check in process_budget["checks"]:
        samples = process_check_samples(process_report, check, process_minimum)
        if samples is None:
            results.append(
                {
                    "id": check["id"],
                    "samples": 0,
                    "statistic": check["statistic"],
                    "observed": None,
                    "maximum": float(check["maximum"]),
                    "unit": check["unit"],
                    "status": "unsupported",
                }
            )
            continue
        add_result(
            results,
            check["id"],
            samples,
            check["statistic"],
            float(check["maximum"]),
            check["unit"],
        )
    for check in process_budget.get("comparative_checks", []):
        baseline = checked_samples(
            value_at_path(process_report, check["baseline_samples_path"], check["id"]),
            f"{check['id']} baseline",
            process_minimum,
        )
        loaded = checked_samples(
            value_at_path(process_report, check["loaded_samples_path"], check["id"]),
            f"{check['id']} loaded",
            process_minimum,
        )
        baseline_statistic = statistic(baseline, check["statistic"])
        if baseline_statistic <= 0:
            raise BudgetError(f"{check['id']} baseline statistic must be positive")
        observed = statistic(loaded, check["statistic"]) / baseline_statistic
        maximum = float(check["maximum_ratio"])
        results.append(
            {
                "id": check["id"],
                "samples": min(len(baseline), len(loaded)),
                "statistic": check["statistic"],
                "observed": observed,
                "maximum": maximum,
                "unit": "ratio",
                "status": "passed" if observed <= maximum else "failed",
            }
        )

    profile_budget = budgets["pane_profiles"]
    profile_minimum = profile_budget["minimum_repetitions"]
    if (
        profile_report.get("schema") != 5
        or profile_report.get("multiplexer") != "frame"
    ):
        raise BudgetError("profile report must be a schema-5 Frame report")
    require_int(
        profile_report.get("repetitions"),
        "profile report repetitions",
        minimum=profile_minimum,
    )
    profiles = profile_report.get("pane_profiles")
    if not isinstance(profiles, dict):
        raise BudgetError("profile report has no pane profiles")
    for profile in ("P1", "P4", "P16", "PMAX"):
        conditions = profiles.get(profile)
        if not isinstance(conditions, dict):
            raise BudgetError(f"profile report is missing {profile}")
        for condition in ("idle", "active"):
            measured = conditions.get(condition)
            if not isinstance(measured, dict) or measured.get("status") != "completed":
                raise BudgetError(f"profile {profile}.{condition} did not complete")
            resources = measured.get("resources")
            interaction = measured.get("interaction")
            if not isinstance(resources, dict) or not isinstance(interaction, dict):
                raise BudgetError(f"profile {profile}.{condition} is incomplete")
            limits = profile_budget["conditions"][condition]
            checks = (
                (
                    "rss_p95",
                    resources.get("rss", {}).get("samples_bytes"),
                    "p95",
                    profile_budget["maximum_p95_rss_bytes"][profile],
                    "bytes",
                ),
                (
                    "cpu_time_p95",
                    resources.get("cpu_time", {}).get("samples_ns"),
                    "p95",
                    limits["maximum_p95_cpu_time_ns"],
                    "ns",
                ),
                (
                    "key_to_pty_p95",
                    interaction.get("key_to_pty", {}).get("samples_ns"),
                    "p95",
                    limits["maximum_p95_key_to_pty_ns"],
                    "ns",
                ),
                (
                    "key_to_outer_bytes_p50",
                    interaction.get("key_to_outer_bytes", {}).get("samples_ns"),
                    "p50",
                    limits["maximum_p50_key_to_outer_bytes_ns"],
                    "ns",
                ),
                (
                    "key_to_outer_bytes_p95",
                    interaction.get("key_to_outer_bytes", {}).get("samples_ns"),
                    "p95",
                    limits["maximum_p95_key_to_outer_bytes_ns"],
                    "ns",
                ),
            )
            for suffix, raw_samples, statistic_name, maximum, unit in checks:
                samples = checked_samples(
                    raw_samples, f"{profile}.{condition}.{suffix}", profile_minimum
                )
                add_result(
                    results,
                    f"{profile}.{condition}.{suffix}",
                    samples,
                    statistic_name,
                    float(maximum),
                    unit,
                )
    return results


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest", type=Path, default=Path("benchmarks/workloads.json")
    )
    parser.add_argument("--micro-report", type=Path)
    parser.add_argument("--process-report", type=Path)
    parser.add_argument("--profile-report", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--validate-manifest", action="store_true")
    arguments = parser.parse_args()
    supplied = (
        arguments.micro_report,
        arguments.process_report,
        arguments.profile_report,
    )
    if arguments.validate_manifest:
        if any(value is not None for value in supplied) or arguments.output is not None:
            parser.error("--validate-manifest does not accept reports or --output")
    elif any(value is None for value in supplied):
        parser.error(
            "--micro-report, --process-report, and --profile-report are required"
        )
    return arguments


def main() -> int:
    arguments = parse_arguments()
    try:
        manifest = load_object(arguments.manifest)
        budgets = budgets_from_manifest(manifest)
        if arguments.validate_manifest:
            return 0
        assert arguments.micro_report is not None
        assert arguments.process_report is not None
        assert arguments.profile_report is not None
        results = evaluate(
            budgets,
            load_object(arguments.micro_report),
            load_object(arguments.process_report),
            load_object(arguments.profile_report),
        )
    except BudgetError as error:
        print(f"regression budget error: {error}", file=sys.stderr)
        return 2

    failed = [result for result in results if result["status"] == "failed"]
    report = {
        "schema": 1,
        "suite": "frame-regression-budget",
        "scope": budgets["scope"],
        "status": "failed" if failed else "passed",
        "checks": results,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output is not None:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    for result in failed:
        print(
            f"regression budget failed: {result['id']}: "
            f"{result['observed']} > {result['maximum']} {result['unit']}",
            file=sys.stderr,
        )
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
