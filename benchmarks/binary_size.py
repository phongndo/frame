#!/usr/bin/env python3
"""Record release executable sizes, stripped sizes, and direct dependencies."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any


def binary_record(path: Path) -> dict[str, Any]:
    record: dict[str, Any] = {"path": str(path.resolve()), "bytes": path.stat().st_size}
    with tempfile.TemporaryDirectory(prefix="frame-stripped-") as temporary:
        stripped = Path(temporary) / path.name
        shutil.copy2(path, stripped)
        subprocess.run(["strip", "-x", str(stripped)], check=True)
        record["stripped_bytes"] = stripped.stat().st_size
    if platform.system() == "Darwin":
        dependencies = subprocess.run(
            ["otool", "-L", str(path)], check=True, capture_output=True, text=True
        ).stdout.splitlines()[1:]
        record["dependencies"] = [
            line.strip().split(" (", 1)[0] for line in dependencies
        ]
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-directory", type=Path, default=Path("build/release"))
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    binaries: dict[str, Any] = {}
    for name in (
        "frame",
        "frame_test_server",
        "frame_test_cli",
        "frame_test_pty_peer",
        "frame_benchmark_probe",
    ):
        path = arguments.build_directory / name
        binaries[name] = binary_record(path)
    for name in ("tmux", "zellij"):
        resolved = shutil.which(name)
        binaries[name] = (
            {"available": False}
            if resolved is None
            else {
                "available": True,
                "path": resolved,
                "bytes": os.stat(resolved).st_size,
            }
        )

    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"], check=True, capture_output=True, text=True
    ).stdout.strip()
    report = {
        "schema": 1,
        "commit": commit,
        "system": platform.platform(),
        "binaries": binaries,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    arguments.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
