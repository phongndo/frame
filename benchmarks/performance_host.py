#!/usr/bin/env python3
"""Validate and record the approved local performance-host state."""

from __future__ import annotations

import argparse
import json
import os
import platform
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from mux_benchmark import host_fingerprint

POLICY_PATH = Path(__file__).with_name("performance_hosts.json")
IDENTITY_FIELDS = ("host_name", "model_identifier", "cpu_model", "physical_cpu_count")


def load_policy(name: str, path: Path = POLICY_PATH) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(
            f"cannot read performance host policy {path}: {error}"
        ) from error
    if (
        not isinstance(document, dict)
        or document.get("schema") != 1
        or not isinstance(document.get("hosts"), dict)
    ):
        raise ValueError("performance host policy must use schema 1")
    policy = document["hosts"].get(name)
    if not isinstance(policy, dict) or policy.get("host_name") != name:
        raise ValueError(f"host {name!r} is not approved")
    for field in (
        "host_name",
        "model_identifier",
        "cpu_model",
        "system",
        "architecture",
    ):
        if not isinstance(policy.get(field), str) or not policy[field]:
            raise ValueError(
                f"performance host policy {field} must be a non-empty string"
            )
    for field in ("physical_cpu_count", "logical_cpu_count", "minimum_memory_bytes"):
        value = policy.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value < 1:
            raise ValueError(
                f"performance host policy {field} must be a positive integer"
            )
    return policy


def validate_fingerprint(fingerprint: Any, policy: dict[str, Any]) -> list[str]:
    if not isinstance(fingerprint, dict) or set(fingerprint) != {
        *IDENTITY_FIELDS,
        "memory_bytes",
    }:
        return ["host fingerprint has invalid fields"]
    failures: list[str] = []
    for field in IDENTITY_FIELDS:
        observed = fingerprint[field]
        expected = policy[field]
        if type(observed) is not type(expected) or observed != expected:
            failures.append(
                f"{field} mismatch: observed {observed!r}, expected {expected!r}"
            )
    memory = fingerprint["memory_bytes"]
    if (
        not isinstance(memory, int)
        or isinstance(memory, bool)
        or memory < policy["minimum_memory_bytes"]
    ):
        failures.append("reported memory is below the approved-host minimum or invalid")
    return failures


def read_values(pattern: str) -> list[str]:
    values: set[str] = set()
    for path in Path("/").glob(pattern.lstrip("/")):
        try:
            value = path.read_text(encoding="utf-8").strip()
        except OSError:
            continue
        if value:
            values.add(value)
    return sorted(values)


def host_snapshot() -> dict[str, Any]:
    return {
        "captured_at": datetime.now(UTC).isoformat(),
        "fingerprint": host_fingerprint(),
        "system": platform.system(),
        "system_release": platform.release(),
        "architecture": platform.machine(),
        "logical_cpu_count": os.cpu_count(),
        "load_average": list(os.getloadavg()),
        "scaling_governors": read_values(
            "/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
        ),
        "energy_performance_preferences": read_values(
            "/sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference"
        ),
    }


def validate(snapshot: dict[str, Any], policy: dict[str, Any]) -> list[str]:
    failures = validate_fingerprint(snapshot.get("fingerprint"), policy)
    exact = {
        "system": snapshot.get("system"),
        "architecture": snapshot.get("architecture"),
        "logical_cpu_count": snapshot.get("logical_cpu_count"),
        "scaling_governors": snapshot.get("scaling_governors"),
        "energy_performance_preferences": snapshot.get(
            "energy_performance_preferences"
        ),
    }
    for field, observed in exact.items():
        if policy.get(field) != observed:
            failures.append(
                f"{field} mismatch: observed {observed!r}, expected {policy.get(field)!r}"
            )
    load = snapshot.get("load_average")
    if (
        not isinstance(load, list)
        or not load
        or load[0] > policy["maximum_load_average_1m"]
    ):
        failures.append("one-minute load average exceeds the approved-host maximum")
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", type=Path, default=POLICY_PATH)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    snapshot = host_snapshot()
    name = snapshot["fingerprint"]["host_name"]
    policy = None
    try:
        policy = load_policy(name, arguments.policy)
        failures = validate(snapshot, policy)
    except ValueError as error:
        failures = [str(error)]
    report = {
        "schema": 1,
        "suite": "frame-performance-host",
        "status": "failed" if failures else "passed",
        "policy": str(arguments.policy.resolve()),
        "cpu_affinity": policy.get("cpu_affinity")
        if isinstance(policy, dict)
        else None,
        "failures": failures,
        **snapshot,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if failures:
        for failure in failures:
            print(f"performance host error: {failure}", file=sys.stderr)
        return 1
    print(f"approved performance host: {name}; report: {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
