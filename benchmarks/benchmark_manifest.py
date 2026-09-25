#!/usr/bin/env python3
"""Load the single versioned benchmark scenario and policy manifest."""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

MANIFEST_SCHEMA = 4
SUBJECTS = ("direct", "lemma", "tmux", "zellij", "herdr")
PROCESS_STATUSES = ("completed", "failed", "unsupported")


class ManifestError(ValueError):
    """The benchmark manifest does not describe a bounded executable suite."""


def _nonempty_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ManifestError(f"{label} must be a non-empty string")
    return value


def _string_list(value: Any, label: str) -> list[str]:
    if not isinstance(value, list) or not value:
        raise ManifestError(f"{label} must be a non-empty array")
    result = []
    for index, item in enumerate(value):
        result.append(_nonempty_string(item, f"{label}[{index}]"))
    if len(result) != len(set(result)):
        raise ManifestError(f"{label} contains duplicate values")
    return result


def _scaling_profiles(
    manifest: dict[str, Any],
    profiles_key: str,
    suites_key: str,
    count_key: str,
    maximum: int,
) -> list[str]:
    profiles = manifest.get(profiles_key)
    if not isinstance(profiles, list) or not profiles:
        raise ManifestError(f"{profiles_key} must be a non-empty array")
    identifiers: list[str] = []
    counts: list[int] = []
    for index, profile in enumerate(profiles):
        label = f"{profiles_key}[{index}]"
        if not isinstance(profile, dict):
            raise ManifestError(f"{label} must be an object")
        identifiers.append(_nonempty_string(profile.get("id"), f"{label}.id"))
        count = profile.get(count_key)
        if (
            not isinstance(count, int)
            or isinstance(count, bool)
            or not 1 <= count <= maximum
        ):
            raise ManifestError(f"{label}.{count_key} is outside the supported bounds")
        counts.append(count)
        _nonempty_string(profile.get("purpose"), f"{label}.purpose")
    if len(identifiers) != len(set(identifiers)) or counts != sorted(counts):
        raise ManifestError(
            f"{profiles_key} must have unique IDs and ascending {count_key} counts"
        )
    suites = manifest.get(suites_key)
    if not isinstance(suites, dict) or set(suites) != {"smoke", "extended", "gate"}:
        raise ManifestError(f"{suites_key} must contain smoke, extended, and gate")
    for name, members in suites.items():
        selected = _string_list(members, f"{suites_key}.{name}")
        unknown = sorted(set(selected).difference(identifiers))
        if unknown:
            raise ManifestError(
                f"{suites_key}.{name} contains unknown profiles: {unknown}"
            )
    if suites["extended"] != identifiers:
        raise ManifestError(f"{suites_key}.extended must cover every profile in order")
    return identifiers


def load_manifest(path: Path | None = None) -> dict[str, Any]:
    selected = path or Path(
        os.environ.get("LEMMA_BENCHMARK_MANIFEST", "benchmarks/workloads.json")
    )
    try:
        manifest = json.loads(selected.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(
            f"cannot read benchmark manifest {selected}: {error}"
        ) from error
    validate_manifest(manifest)
    return manifest


def validate_manifest(manifest: Any) -> None:
    if not isinstance(manifest, dict) or manifest.get("schema") != MANIFEST_SCHEMA:
        raise ManifestError(f"benchmark manifest must use schema {MANIFEST_SCHEMA}")

    terminal = manifest.get("terminal")
    if not isinstance(terminal, dict):
        raise ManifestError("terminal must be an object")
    for dimension in ("columns", "rows"):
        value = terminal.get(dimension)
        if (
            not isinstance(value, int)
            or isinstance(value, bool)
            or not 0 < value <= 1_000
        ):
            raise ManifestError(f"terminal.{dimension} is outside the supported bounds")
    _nonempty_string(terminal.get("term"), "terminal.term")

    _scaling_profiles(manifest, "pane_profiles", "profile_suites", "panes", 64)
    _scaling_profiles(
        manifest,
        "session_profiles",
        "session_profile_suites",
        "sessions",
        64,
    )
    _scaling_profiles(
        manifest,
        "workspace_profiles",
        "workspace_profile_suites",
        "workspaces",
        64,
    )

    workloads = manifest.get("process_workloads")
    if not isinstance(workloads, list) or not workloads:
        raise ManifestError("process_workloads must be a non-empty array")
    identifiers: list[str] = []
    cli_modes: list[str] = []
    for index, workload in enumerate(workloads):
        label = f"process_workloads[{index}]"
        if not isinstance(workload, dict):
            raise ManifestError(f"{label} must be an object")
        identifiers.append(_nonempty_string(workload.get("id"), f"{label}.id"))
        cli_modes.append(
            _nonempty_string(workload.get("cli_mode"), f"{label}.cli_mode")
        )
        _nonempty_string(workload.get("fixture"), f"{label}.fixture")
        _nonempty_string(
            workload.get("completion_endpoint"), f"{label}.completion_endpoint"
        )
        subjects = _string_list(workload.get("subjects"), f"{label}.subjects")
        unknown = sorted(set(subjects).difference(SUBJECTS))
        if unknown:
            raise ManifestError(
                f"{label}.subjects contains unknown subjects: {unknown}"
            )
        _string_list(workload.get("metrics"), f"{label}.metrics")
        unsupported = workload.get("unsupported_subjects", {})
        if not isinstance(unsupported, dict):
            raise ManifestError(f"{label}.unsupported_subjects must be an object")
        for subject, reason in unsupported.items():
            if subject not in SUBJECTS or subject in subjects:
                raise ManifestError(
                    f"{label}.unsupported_subjects must name only excluded subjects"
                )
            _nonempty_string(reason, f"{label}.unsupported_subjects.{subject}")
        if "comparison_sampling" in workload:
            sampling = workload.get("comparison_sampling")
            limits = {"blocks": 32, "repetition_scale": 100}
            if not isinstance(sampling, dict) or set(sampling) != set(limits):
                raise ManifestError(
                    f"{label}.comparison_sampling must define blocks and repetition_scale"
                )
            for key, value in sampling.items():
                if (
                    not isinstance(value, int)
                    or isinstance(value, bool)
                    or not 1 <= value <= limits[str(key)]
                ):
                    raise ManifestError(
                        f"{label}.comparison_sampling.{key} is outside the supported bounds"
                    )
    if len(identifiers) != len(set(identifiers)):
        raise ManifestError("process workload IDs must be unique")
    if len(cli_modes) != len(set(cli_modes)):
        raise ManifestError("process workload cli_mode values must be unique")

    suites = manifest.get("suites")
    if not isinstance(suites, dict) or not suites:
        raise ManifestError("suites must be a non-empty object")
    known = set(identifiers)
    for name, members in suites.items():
        _nonempty_string(name, "suite name")
        selected = _string_list(members, f"suites.{name}")
        unknown = sorted(set(selected).difference(known))
        if unknown:
            raise ManifestError(f"suites.{name} contains unknown workloads: {unknown}")
    # The paired gate captures only the regression suite: diagnostic comparisons stay out of it
    # until reviewed budgets reference them, so their failures cannot abort a gate.
    regression = suites.get("regression")
    comparison = suites.get("comparison")
    if not isinstance(regression, list) or not isinstance(comparison, list):
        raise ManifestError("suites must define comparison and regression")
    if not set(regression) <= set(comparison):
        raise ManifestError("suites.regression must be a subset of suites.comparison")
    budgets = manifest.get("regression_budgets")
    process_budgets = (
        budgets.get("process_workloads") if isinstance(budgets, dict) else None
    )
    if isinstance(process_budgets, dict):
        referenced = {
            path[1]
            for check in process_budgets.get("checks", [])
            for path in (check.get("samples_path"),)
            if isinstance(path, list) and len(path) > 1
        } | {
            check.get(key, [None, None])[1]
            for check in process_budgets.get("comparative_checks", [])
            for key in ("baseline_samples_path", "loaded_samples_path")
        }
        missing = sorted(str(name) for name in referenced - set(regression))
        if missing:
            raise ManifestError(
                f"suites.regression omits budgeted workloads: {missing}"
            )

    policies = manifest.get("sample_policies")
    if not isinstance(policies, dict) or set(policies) != {
        "smoke",
        "extended",
        "gate",
    }:
        raise ManifestError("sample_policies must contain smoke, extended, and gate")
    for name, policy in policies.items():
        if not isinstance(policy, dict):
            raise ManifestError(f"sample_policies.{name} must be an object")
        for field in (
            "micro_repetitions",
            "process_repetitions",
            "profile_repetitions",
        ):
            value = policy.get(field)
            if (
                not isinstance(value, int)
                or isinstance(value, bool)
                or not 1 <= value <= 10_000
            ):
                raise ManifestError(f"sample_policies.{name}.{field} is invalid")
        for field in ("micro_min_time_seconds", "micro_warmup_seconds"):
            value = policy.get(field)
            if (
                not isinstance(value, (int, float))
                or isinstance(value, bool)
                or not 0 <= value <= 60
            ):
                raise ManifestError(f"sample_policies.{name}.{field} is invalid")

    deterministic = manifest.get("deterministic_budgets")
    steady_state = (
        deterministic.get("steady_state") if isinstance(deterministic, dict) else None
    )
    reactor = deterministic.get("reactor") if isinstance(deterministic, dict) else None
    if (
        not isinstance(deterministic, dict)
        or deterministic.get("schema") != 1
        or deterministic.get("status") != "reviewed"
        or not isinstance(steady_state, dict)
        or not isinstance(reactor, dict)
    ):
        raise ManifestError("deterministic_budgets must be a reviewed schema-1 policy")
    deterministic_fields = {
        "audited_iterations",
        "maximum_general_allocation_calls",
        "maximum_general_allocation_bytes",
        "maximum_terminal_quota_allocation_calls",
        "maximum_frame_bytes",
        "maximum_queued_messages",
        "maximum_frame_messages_per_frame",
        "maximum_writer_attempts_per_frame",
        "maximum_wire_overhead_bytes_per_frame",
        "required_routed_input_bytes_per_iteration",
        "required_extension_accounting_scans_per_iteration",
        "required_active_extension_peers",
        "required_active_extension_surfaces",
        "required_frames_per_iteration",
        "required_flush_calls_per_iteration",
    }
    if set(steady_state) != deterministic_fields:
        raise ManifestError("deterministic_budgets.steady_state fields are incomplete")
    for field, value in steady_state.items():
        minimum = 1 if field == "audited_iterations" else 0
        if (
            not isinstance(value, int)
            or isinstance(value, bool)
            or not minimum <= value <= 1_000_000_000
        ):
            raise ManifestError(
                f"deterministic_budgets.steady_state.{field} is invalid"
            )
    extension = manifest.get("extension_performance")
    extension_fixtures = (
        extension.get("fixtures") if isinstance(extension, dict) else None
    )
    extension_limits = extension.get("limits") if isinstance(extension, dict) else None
    if (
        not isinstance(extension, dict)
        or extension.get("schema") != 1
        or extension.get("status") != "reviewed"
        or extension.get("approved_host") != "z"
        or not isinstance(extension.get("minimum_repetitions"), int)
        or extension["minimum_repetitions"] < 20
        or not isinstance(extension_fixtures, dict)
        or set(extension_fixtures)
        != {
            "idle-peers",
            "idle-surfaces",
            "changing-rows",
            "storm",
            "slow-producer",
            "blocked-reader",
            "crash-focused",
            "crash-docked",
        }
        or not isinstance(extension_limits, dict)
    ):
        raise ManifestError("extension_performance must be a reviewed schema-1 policy")
    for name, fixture in extension_fixtures.items():
        if not isinstance(fixture, dict) or not fixture:
            raise ManifestError(f"extension_performance.fixtures.{name} is invalid")
        for field, value in fixture.items():
            if (
                not isinstance(field, str)
                or not isinstance(value, int)
                or isinstance(value, bool)
                or value <= 0
            ):
                raise ManifestError(
                    f"extension_performance.fixtures.{name}.{field} is invalid"
                )
    expected_extension_limits = {
        "idle_daemon_cpu_p95_ns_per_second",
        "idle_daemon_wakeups_p95_per_second",
        "idle_daemon_rss_increase_bytes",
        "interactive_key_to_outer_bytes_p95_maximum_ratio",
        "slow_producer_key_to_outer_bytes_p95_maximum_ratio",
        "minimum_storm_outer_bytes_per_second",
        "maximum_paste_submit_ns",
        "minimum_blocked_other_peer_proc_completions",
        "maximum_cleanup_to_outer_bytes_ns",
    }
    if set(extension_limits) != expected_extension_limits:
        raise ManifestError("extension_performance.limits fields are incomplete")
    for field, value in extension_limits.items():
        if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
            raise ManifestError(f"extension_performance.limits.{field} is invalid")

    reactor_fields = {
        "test",
        "maximum_poll_calls",
        "maximum_readiness_events",
        "maximum_outbound_send_calls",
        "required_blocked_sends",
        "required_partial_sends",
        "required_child_wakeups",
    }
    if set(reactor) != reactor_fields or not isinstance(reactor["test"], str):
        raise ManifestError("deterministic_budgets.reactor fields are incomplete")
    if reactor["test"] != (
        "ReactorEnvironmentTest."
        "ScriptedWorldControlsFragmentationBackpressureChildExitAndOrdering"
    ):
        raise ManifestError("deterministic_budgets.reactor.test is invalid")
    for field in reactor_fields - {"test"}:
        value = reactor[field]
        if (
            not isinstance(value, int)
            or isinstance(value, bool)
            or not 1 <= value <= 1_000_000
        ):
            raise ManifestError(f"deterministic_budgets.reactor.{field} is invalid")

    comparison_policy = manifest.get("comparison_policy")
    practical = (
        comparison_policy.get("practical_effect_thresholds")
        if isinstance(comparison_policy, dict)
        else None
    )
    if not isinstance(practical, dict):
        raise ManifestError("comparison_policy has no practical effect thresholds")
    for name in ("minimum_relative_change", "minimum_latency_change_ns"):
        value = practical.get(name)
        if not isinstance(value, (int, float)) or isinstance(value, bool) or value <= 0:
            raise ManifestError(f"comparison_policy.{name} is invalid")
    expected_failures = comparison_policy.get("expected_failures")
    if not isinstance(expected_failures, list):
        raise ManifestError("comparison_policy.expected_failures must be an array")
    failure_signatures: list[tuple[str, str, str]] = []
    workload_subjects = {
        workload["id"]: set(workload["subjects"]) for workload in workloads
    }
    for index, failure in enumerate(expected_failures):
        label = f"comparison_policy.expected_failures[{index}]"
        if not isinstance(failure, dict):
            raise ManifestError(f"{label} must be an object")
        subject = _nonempty_string(failure.get("subject"), f"{label}.subject")
        workload = _nonempty_string(failure.get("workload"), f"{label}.workload")
        _nonempty_string(failure.get("classification"), f"{label}.classification")
        error_contains = _nonempty_string(
            failure.get("error_contains"), f"{label}.error_contains"
        )
        if subject not in SUBJECTS or workload not in workload_subjects:
            raise ManifestError(f"{label} identifies an unknown subject or workload")
        if subject not in workload_subjects[workload]:
            raise ManifestError(f"{label} identifies an unsupported workload")
        failure_signatures.append((subject, workload, error_contains))
    if len(failure_signatures) != len(set(failure_signatures)):
        raise ManifestError(
            "comparison_policy.expected_failures contains duplicate signatures"
        )

    budgets = manifest.get("regression_budgets")
    scope = budgets.get("scope") if isinstance(budgets, dict) else None
    maximum_load = (
        scope.get("maximum_load_average_1m") if isinstance(scope, dict) else None
    )
    if (
        not isinstance(maximum_load, (int, float))
        or isinstance(maximum_load, bool)
        or maximum_load <= 0
    ):
        raise ManifestError(
            "regression_budgets.scope.maximum_load_average_1m is invalid"
        )

    lab = manifest.get("terminal_lab")
    if not isinstance(lab, dict) or lab.get("schema") != 1:
        raise ManifestError("terminal_lab must use schema 1")
    terminals = _string_list(lab.get("terminals"), "terminal_lab.terminals")
    if terminals != ["ghostty", "kitty", "wezterm"]:
        raise ManifestError(
            "terminal_lab.terminals must be ghostty, kitty, and wezterm"
        )
    if _string_list(lab.get("subjects"), "terminal_lab.subjects") != list(SUBJECTS):
        raise ManifestError(
            "terminal_lab.subjects must contain the complete subject matrix"
        )
    endpoints = _string_list(lab.get("endpoints"), "terminal_lab.endpoints")
    if "input_to_photon_ns" not in endpoints:
        raise ManifestError("terminal_lab must define an input-to-photon endpoint")


def expected_failure(
    manifest: dict[str, Any], subject: str, workload: str, error: str
) -> dict[str, str] | None:
    for failure in manifest["comparison_policy"]["expected_failures"]:
        if (
            failure["subject"] == subject
            and failure["workload"] == workload
            and failure["error_contains"] in error
        ):
            return failure
    return None


def unsupported_result(workload: dict[str, Any], subject: str) -> dict[str, str]:
    reason = workload.get("unsupported_subjects", {}).get(subject)
    return {
        "status": "unsupported",
        "reason": reason
        if isinstance(reason, str)
        else f"{workload['id']} is not defined for the {subject} subject",
    }


def comparison_sampling(workload: dict[str, Any]) -> tuple[int, int]:
    """Return (blocks, repetition_scale) for one workload in the comparison suite.

    Only the cross-subject comparison uses this: gate and extension captures run the
    workload's plain repetition count.
    """
    sampling = workload.get("comparison_sampling", {})
    return int(sampling.get("blocks", 1)), int(sampling.get("repetition_scale", 1))


def workload_map(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {workload["id"]: workload for workload in manifest["process_workloads"]}


def workload_for_mode(manifest: dict[str, Any], mode: str) -> dict[str, Any] | None:
    return next(
        (
            workload
            for workload in manifest["process_workloads"]
            if workload["cli_mode"] == mode
        ),
        None,
    )


def suite_workloads(manifest: dict[str, Any], suite: str) -> list[dict[str, Any]]:
    scenarios = workload_map(manifest)
    try:
        return [scenarios[identifier] for identifier in manifest["suites"][suite]]
    except KeyError as error:
        raise ManifestError(f"unknown benchmark suite: {suite}") from error
