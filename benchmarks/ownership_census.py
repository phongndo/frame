#!/usr/bin/env python3
"""Reproduce compiler-record byte sizes for F3 memory owners."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
from pathlib import Path
from typing import Any

TARGETS = {
    "pane_semantic": "struct frame::core::Pane",
    "pane_runtime": "struct frame::core::engine_detail::PaneRuntime",
    "pane_runtime_store": "class frame::core::engine_detail::PaneRuntimeStore",
    "tab_inline": "struct frame::core::Tab",
    "session_inline": "struct frame::core::Session",
    "attachment_semantic": "struct frame::core::Attachment",
    "attachment_runtime": "struct frame::core::engine_detail::AttachmentRuntime",
    "session_record": "struct frame::core::engine_detail::SessionRecord",
    "copy_mode_semantic": "struct frame::core::CopyModeState",
    "copy_mode_runtime": "struct frame::core::engine_detail::CopyModeRuntimeState",
    "pending_connection": "struct frame::core::engine_detail::PendingConnection",
    "descriptor_owner": "struct frame::core::engine_detail::DescriptorOwner",
    "pty_write_queue_inline": "class frame::core::PanePtyWriteQueue",
    "client_decoder_inline": "class frame::protocol::ClientDecoder",
    "connection_output_inline": "class frame::core::ConnectionOutput",
    "client_frame_output_inline": "class frame::core::ClientFrameOutput",
    "frame_buffer_inline": "class frame::render::FrameBuffer",
    "terminal_impl_inline": "struct frame::vt::Terminal::Impl",
    "terminal_quota_allocator_inline": "class frame::vt::detail::QuotaAllocator",
}


def layout_command(entry: dict[str, Any]) -> list[str]:
    arguments = shlex.split(str(entry["command"]))
    command: list[str] = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        if argument == "-o":
            skip = True
            continue
        if argument == "-c":
            continue
        command.append(argument)
    return [*command, "-Xclang", "-fdump-record-layouts", "-fsyntax-only"]


def record_sizes(raw: str) -> dict[str, int]:
    lines = raw.splitlines()
    sizes: dict[str, int] = {}
    size_pattern = re.compile(r"\[sizeof=(\d+),")
    for label, target in TARGETS.items():
        for index, line in enumerate(lines):
            if "|" not in line or line.split("|", 1)[1].strip() != target:
                continue
            for following in lines[index + 1 : index + 1_000]:
                match = size_pattern.search(following)
                if match is not None:
                    sizes[label] = int(match.group(1))
                    break
                if "*** Dumping AST Record Layout" in following:
                    break
            break
    missing = sorted(set(TARGETS).difference(sizes))
    if missing:
        raise RuntimeError(f"record layout dump omitted targets: {', '.join(missing)}")
    return sizes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--compile-commands",
        type=Path,
        default=Path("build/release/compile_commands.json"),
    )
    parser.add_argument("--raw-output", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()

    entries = json.loads(arguments.compile_commands.read_text(encoding="utf-8"))
    selected = [
        entry
        for entry in entries
        if str(entry.get("file", "")).endswith(
            (
                "/src/core/engine.cpp",
                "/src/terminal/terminal_core.cpp",
            )
        )
    ]
    if len(selected) != 2:
        raise RuntimeError(
            "compile_commands must contain engine.cpp and terminal_core.cpp exactly once"
        )

    chunks: list[str] = []
    for entry in selected:
        result = subprocess.run(
            layout_command(entry),
            cwd=entry["directory"],
            check=True,
            capture_output=True,
            text=True,
        )
        chunks.append(f"===== {entry['file']} =====\n{result.stdout}{result.stderr}")
    raw = "\n".join(chunks)
    arguments.raw_output.parent.mkdir(parents=True, exist_ok=True)
    arguments.raw_output.write_text(raw, encoding="utf-8")

    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"], check=True, capture_output=True, text=True
    ).stdout.strip()
    status = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=normal"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    report = {
        "schema": 1,
        "commit": commit,
        "worktree_dirty": bool(status),
        "compile_commands": str(arguments.compile_commands),
        "raw_layouts": str(arguments.raw_output),
        "sizes_bytes": record_sizes(raw),
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
