#!/usr/bin/env python3
"""Decode and correlate Frame's bounded opt-in monotonic latency trace files."""

from __future__ import annotations

import argparse
import json
import math
import struct
from collections import Counter, defaultdict
from itertools import pairwise
from pathlib import Path
from typing import Any

TRACE_MAGIC = 0x3145_4341_5254_4D46
TRACE_VERSION = 2
EVENTS_MAX = 524_288
HEADER = struct.Struct("<QIHHIIQQ24x")
EVENT = struct.Struct("<QQQQIHH")
ROLES = {1: "daemon", 2: "attached_client"}
STAGES = {
    1: "client_physical_input_read",
    2: "daemon_input_message_received",
    3: "daemon_pty_write_progress",
    4: "daemon_pty_output_read",
    5: "ghostty_damage_reported",
    6: "frame_composition_started",
    7: "frame_composition_finished",
    8: "daemon_socket_write_progress",
    9: "client_socket_read",
    10: "client_outer_terminal_write_started",
    11: "client_outer_terminal_write_finished",
}
PATH_STAGES = (
    "client_physical_input_read",
    "daemon_input_message_received",
    "daemon_pty_write_progress",
    "daemon_pty_output_read",
    "frame_composition_started",
    "ghostty_damage_reported",
    "frame_composition_finished",
    "daemon_socket_write_progress",
    "client_socket_read",
    "client_outer_terminal_write_started",
    "client_outer_terminal_write_finished",
)
CLIENT_STAGES = {
    "client_physical_input_read",
    "client_socket_read",
    "client_outer_terminal_write_started",
    "client_outer_terminal_write_finished",
}
ROLE_STAGES = {
    "daemon": set(PATH_STAGES) - CLIENT_STAGES,
    "attached_client": CLIENT_STAGES,
}
PTY_STAGES = {
    "daemon_input_message_received",
    "daemon_pty_write_progress",
    "daemon_pty_output_read",
}


def percentile(samples: list[int], quantile: float) -> int:
    ordered = sorted(samples)
    index = max(0, min(len(ordered) - 1, math.ceil(quantile * len(ordered)) - 1))
    return ordered[index]


def summary(samples: list[int]) -> dict[str, Any]:
    return {
        "samples_ns": samples,
        "p50_ns": percentile(samples, 0.50),
        "p95_ns": percentile(samples, 0.95),
        "p99_ns": percentile(samples, 0.99),
    }


def read_trace(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    encoded = path.read_bytes()
    if len(encoded) < HEADER.size:
        raise RuntimeError(f"trace header is truncated: {path}")
    magic, version, role, event_size, capacity, process, count, dropped = (
        HEADER.unpack_from(encoded)
    )
    if magic != TRACE_MAGIC or version != TRACE_VERSION:
        raise RuntimeError(f"trace magic/version mismatch: {path}")
    if role not in ROLES or process == 0:
        raise RuntimeError(f"trace role/process is invalid: {path}")
    if encoded[40 : HEADER.size] != bytes(HEADER.size - 40):
        raise RuntimeError(f"trace header reserved bytes are invalid: {path}")
    if event_size != EVENT.size or capacity != EVENTS_MAX or count > capacity:
        raise RuntimeError(f"trace bounds are invalid: {path}")
    expected_size = HEADER.size + (capacity * event_size)
    if len(encoded) != expected_size:
        raise RuntimeError(
            f"trace size mismatch: {path}: expected={expected_size} actual={len(encoded)}"
        )

    events = []
    previous_timestamp = 0
    for index in range(count):
        offset = HEADER.size + (index * event_size)
        (
            timestamp_ns,
            sequence,
            correlation,
            value,
            subject,
            stage,
            reserved,
        ) = EVENT.unpack_from(encoded, offset)
        stage_name = STAGES.get(stage)
        if (
            sequence != index + 1
            or stage_name not in ROLE_STAGES[ROLES[role]]
            or reserved != 0
            or timestamp_ns == 0
            or timestamp_ns < previous_timestamp
        ):
            raise RuntimeError(f"trace event {index} is invalid: {path}")
        previous_timestamp = timestamp_ns
        events.append(
            {
                "timestamp_ns": timestamp_ns,
                "sequence": sequence,
                "correlation": correlation,
                "role": ROLES[role],
                "process": process,
                "stage": stage_name,
                "subject": subject,
                "value": value,
                "source": str(path),
            }
        )
    return (
        {
            "path": str(path),
            "role": ROLES[role],
            "process": process,
            "capacity": capacity,
            "events": count,
            "dropped": dropped,
        },
        events,
    )


def rejected_input(event: dict[str, Any], reason: str) -> dict[str, Any]:
    return {
        "correlation": event["correlation"],
        "input_process": event["process"],
        "input_sequence": event["sequence"],
        "timestamp_ns": event["timestamp_ns"],
        "subject": event["subject"],
        "value": event["value"],
        "source": event.get("source"),
        "reason": reason,
    }


def input_paths(
    events: list[dict[str, Any]], input_bytes: set[int] | None = None
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Build paths only from an exact marker-derived token observed at every boundary."""
    by_correlation: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for event in events:
        correlation = int(event.get("correlation", 0))
        if correlation != 0:
            by_correlation[correlation].append(event)
    for correlated in by_correlation.values():
        correlated.sort(
            key=lambda event: (
                event["timestamp_ns"],
                event["process"],
                event["sequence"],
            )
        )

    selected_inputs = [
        event
        for event in events
        if event["stage"] == "client_physical_input_read"
        and (not input_bytes or int(event["value"]) in input_bytes)
    ]
    input_token_counts = Counter(
        int(event.get("correlation", 0))
        for event in selected_inputs
        if int(event.get("correlation", 0)) != 0
    )

    paths = []
    rejected = []
    for input_event in selected_inputs:
        correlation = int(input_event.get("correlation", 0))
        if correlation == 0:
            rejected.append(
                rejected_input(input_event, "physical input has no marker token")
            )
            continue
        if input_token_counts[correlation] != 1:
            rejected.append(
                rejected_input(input_event, "physical input marker token is reused")
            )
            continue

        stages: dict[str, dict[str, Any]] = {PATH_STAGES[0]: input_event}
        previous_timestamp = int(input_event["timestamp_ns"])
        daemon_process: int | None = None
        pty_subject: int | None = None
        candidates = by_correlation[correlation]
        missing: str | None = None
        for stage in PATH_STAGES[1:]:
            matched = next(
                (
                    event
                    for event in candidates
                    if event["stage"] == stage
                    and int(event["timestamp_ns"]) >= previous_timestamp
                    and (
                        (
                            stage in CLIENT_STAGES
                            and event["process"] == input_event["process"]
                        )
                        or (
                            stage not in CLIENT_STAGES
                            and (
                                daemon_process is None
                                or event["process"] == daemon_process
                            )
                        )
                    )
                    and (
                        stage not in PTY_STAGES
                        or pty_subject is None
                        or event["subject"] == pty_subject
                    )
                ),
                None,
            )
            if matched is None:
                missing = stage
                break
            stages[stage] = matched
            previous_timestamp = int(matched["timestamp_ns"])
            if stage not in CLIENT_STAGES and daemon_process is None:
                daemon_process = int(matched["process"])
            if stage in PTY_STAGES and pty_subject is None:
                pty_subject = int(matched["subject"])
        if missing is not None:
            rejected.append(
                rejected_input(
                    input_event, f"marker token is missing ordered stage {missing}"
                )
            )
            continue

        paths.append(
            {
                "correlation": correlation,
                "input_process": input_event["process"],
                "input_sequence": input_event["sequence"],
                "stages": stages,
                "total_ns": previous_timestamp - int(input_event["timestamp_ns"]),
            }
        )
    return paths, rejected


def build_report(
    directory: Path, input_bytes: set[int] | None = None
) -> dict[str, Any]:
    files = []
    events = []
    for path in sorted(directory.glob("*.ltrace")):
        metadata, decoded = read_trace(path)
        files.append(metadata)
        events.extend(decoded)
    if not files:
        raise RuntimeError(f"no .ltrace files in {directory}")
    events.sort(
        key=lambda event: (event["timestamp_ns"], event["process"], event["sequence"])
    )
    paths, rejected_paths = input_paths(events, input_bytes)

    stage_distributions: dict[str, Any] = {}
    for stage in PATH_STAGES[1:]:
        samples = [
            int(path["stages"][stage]["timestamp_ns"])
            - int(path["stages"][PATH_STAGES[0]]["timestamp_ns"])
            for path in paths
        ]
        if samples:
            stage_distributions[stage] = summary(samples)

    stage_intervals: dict[str, Any] = {}
    for first, second in pairwise(PATH_STAGES):
        samples = [
            int(path["stages"][second]["timestamp_ns"])
            - int(path["stages"][first]["timestamp_ns"])
            for path in paths
        ]
        if samples:
            stage_intervals[f"{first}_to_{second}"] = summary(samples)

    input_processes = sorted(
        {int(path["input_process"]) for path in [*paths, *rejected_paths]}
    )
    return {
        "schema": 3,
        "suite": "frame-latency-trace",
        "trace_version": TRACE_VERSION,
        "clock": "CLOCK_MONOTONIC",
        "files": files,
        "dropped": sum(int(file["dropped"]) for file in files),
        "input_bytes_filter": sorted(input_bytes) if input_bytes else [],
        "correlation": {
            "status": "correlated" if paths else "no_complete_paths",
            "method": (
                "exact bounded _XXXXXX__ fixture token observed at each byte boundary; "
                "client receive uses the exact decoder-completing socket-read event"
            ),
            "input_processes": input_processes,
            "correlated_paths": len(paths),
            "rejected_paths": len(rejected_paths),
        },
        "stage_distributions_from_physical_input": stage_distributions,
        "stage_intervals": stage_intervals,
        "input_paths": paths,
        "rejected_input_paths": rejected_paths,
        "events": events,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--input-bytes",
        type=int,
        action="append",
        default=[],
        help="retain physical-input observations with this byte count (repeatable)",
    )
    arguments = parser.parse_args()
    if any(value < 1 or value > 8_192 for value in arguments.input_bytes):
        parser.error("--input-bytes values must be between 1 and 8192")
    try:
        report = build_report(arguments.directory, set(arguments.input_bytes))
    except (OSError, RuntimeError, struct.error) as error:
        parser.error(str(error))
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output is not None:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
