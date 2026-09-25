#!/usr/bin/env python3
"""Summarize raw F3 profile, history, and lifecycle memory reports."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def profile_summary(report: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for profile, conditions in report["pane_profiles"].items():
        result[profile] = {}
        for condition, value in conditions.items():
            resources = value["resources"]
            roles = resources["roles"]
            result[profile][condition] = {
                "tree_rss_p50_bytes": resources["rss"]["p50_bytes"],
                "daemon_rss_p50_bytes": roles["daemon"]["rss"]["p50_bytes"],
                "client_rss_p50_bytes": roles["attached_client"]["rss"]["p50_bytes"],
                "children_rss_p50_bytes": roles["pane_or_mux_children"]["rss"][
                    "p50_bytes"
                ],
            }
    return result


def component_summary(report: dict[str, Any]) -> dict[str, Any]:
    workload = report["workloads"]["component_resources"]
    result: dict[str, Any] = {}
    for condition in (
        "baseline",
        "detached_session",
        "attached_session",
        "detached_after_attach",
    ):
        resources = workload[condition]
        result[condition] = {
            "tree_rss_p50_bytes": resources["rss"]["p50_bytes"],
            "daemon_rss_p50_bytes": resources["roles"]["daemon"]["rss"]["p50_bytes"],
        }
    result["detached_session_daemon_delta_bytes"] = (
        result["detached_session"]["daemon_rss_p50_bytes"]
        - result["baseline"]["daemon_rss_p50_bytes"]
    )
    result["attachment_daemon_delta_bytes"] = (
        result["attached_session"]["daemon_rss_p50_bytes"]
        - result["detached_session"]["daemon_rss_p50_bytes"]
    )
    result["post_detach_daemon_delta_bytes"] = (
        result["detached_after_attach"]["daemon_rss_p50_bytes"]
        - result["detached_session"]["daemon_rss_p50_bytes"]
    )
    return result


def history_summary(report: dict[str, Any]) -> dict[str, Any]:
    workload = report["workloads"]["history_resources"]
    result: dict[str, Any] = {
        "history_input_rows": workload["history_input_rows"],
    }
    for condition in ("empty", "populated"):
        resources = workload[condition]
        result[condition] = {
            "tree_rss_p50_bytes": resources["rss"]["p50_bytes"],
            "daemon_rss_p50_bytes": resources["roles"]["daemon"]["rss"]["p50_bytes"],
        }
    result["tree_rss_delta_bytes"] = (
        result["populated"]["tree_rss_p50_bytes"]
        - result["empty"]["tree_rss_p50_bytes"]
    )
    result["daemon_rss_delta_bytes"] = (
        result["populated"]["daemon_rss_p50_bytes"]
        - result["empty"]["daemon_rss_p50_bytes"]
    )
    return result


def lifecycle_summary(report: dict[str, Any]) -> dict[str, Any]:
    workload = report["workloads"]["lifecycle_churn"]
    samples = workload["daemon_rss"]["samples_bytes"]
    plateau_count = max(1, len(samples) // 4)
    plateau = samples[-plateau_count:]
    return {
        "cycles": workload["cycles"],
        "daemon_rss_samples_bytes": samples,
        "first_daemon_rss_bytes": samples[0],
        "last_daemon_rss_bytes": samples[-1],
        "plateau_sample_count": plateau_count,
        "plateau_min_bytes": min(plateau),
        "plateau_max_bytes": max(plateau),
        "plateau_range_bytes": max(plateau) - min(plateau),
    }


def load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lemma-profiles", type=Path, required=True)
    parser.add_argument("--tmux-profiles", type=Path, required=True)
    parser.add_argument("--zellij-profiles", type=Path)
    parser.add_argument("--herdr-profiles", type=Path)
    parser.add_argument("--components", type=Path, required=True)
    parser.add_argument("--history", type=Path, required=True)
    parser.add_argument("--lifecycle", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    lemma = profile_summary(load(arguments.lemma_profiles))
    tmux = profile_summary(load(arguments.tmux_profiles))
    lemma_p1 = lemma["P1"]["idle"]["tree_rss_p50_bytes"]
    tmux_p1 = tmux["P1"]["idle"]["tree_rss_p50_bytes"]
    report = {
        "schema": 1,
        "lemma_profiles": lemma,
        "tmux_profiles": tmux,
        "p1_idle_tree_rss_lemma_to_tmux_ratio": lemma_p1 / tmux_p1,
        "competitor_profiles": {
            subject: profile_summary(load(path))
            for subject, path in (
                ("zellij", arguments.zellij_profiles),
                ("herdr", arguments.herdr_profiles),
            )
            if path is not None
        },
        "components": component_summary(load(arguments.components)),
        "history": history_summary(load(arguments.history)),
        "lifecycle": lifecycle_summary(load(arguments.lifecycle)),
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    arguments.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
