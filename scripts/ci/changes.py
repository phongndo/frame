#!/usr/bin/env python3
"""Classify a Git diff into the Frame CI lanes that it can affect."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from collections.abc import Iterable

LANES = ("cpp", "python", "automation")
ZERO_SHA = "0" * 40


def _is(path: str, *names: str) -> bool:
    return path in names


def classify_paths(paths: Iterable[str]) -> dict[str, bool]:
    """Return the conservative set of lanes affected by repository paths."""
    result = {lane: False for lane in LANES}

    for raw_path in paths:
        path = raw_path.removeprefix("./")

        # Orchestration must exercise every conditional job. Otherwise a broken
        # classifier could appear green after selecting only its own lint lane.
        if path.startswith((".github/workflows/", ".github/actions/", "scripts/ci/")):
            return {lane: True for lane in LANES}

        if path.startswith("tools/test_ci"):
            result["automation"] = True

        if path.endswith((".py", ".pyi")) or _is(path, "pyproject.toml", "uv.lock"):
            result["python"] = True

        # Native contract inputs include the embedded schema and canonical runnable
        # examples. Markdown copies are checked separately by the unconditional docs job.
        # Select the independent C++ format, build/test, analysis, and sanitizer jobs.
        if path.startswith(
            (
                "apps/",
                "benchmarks/",
                "examples/",
                "extensions/",
                "schema/",
                "include/",
                "src/",
                "tests/",
                "third_party/ghostty",
            )
        ):
            result["cpp"] = True

        if path.startswith(("cmake/", "conan/")) or _is(
            path,
            "CMakeLists.txt",
            "CMakePresets.json",
            "conan.lock",
            "conanfile.py",
            ".gitmodules",
        ):
            result["cpp"] = True

        if _is(path, ".clang-format", ".clang-tidy", ".clangd", "hk.pkl"):
            result["cpp"] = True
        if _is(path, "hk.pkl"):
            result["python"] = True

        # These files define the environment or local entry points for all
        # merge-blocking suites, so validate their complete contract.
        if _is(path, "flake.nix", "flake.lock", "justfile"):
            result = {lane: True for lane in LANES}

    return result


def diff_revisions(base: str, head: str, event: str | None) -> list[str]:
    """Return Git revision arguments appropriate for the triggering event."""
    # Push validation must compare the two ref endpoints. In particular, a
    # force-push can remove source changes that are not present in the new
    # side's merge-base-to-head history. Pull requests and merge groups instead
    # validate the proposed branch changes relative to their merge base.
    if event == "push":
        return [base, head]
    return [f"{base}...{head}"]


def changed_paths(
    base: str | None, head: str, event: str | None = None
) -> list[str] | None:
    """Return affected paths, or None when a complete diff is unavailable."""
    if not base or base == ZERO_SHA:
        return None

    for revision in (base, head):
        check = subprocess.run(
            ["git", "cat-file", "-e", f"{revision}^{{commit}}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if check.returncode != 0:
            return None

    # Treat renames as a deletion plus an addition so the old component still
    # selects its lane when a source file moves into an otherwise unrelated path.
    revisions = diff_revisions(base, head, event)
    completed = subprocess.run(
        [
            "git",
            "diff",
            "--name-only",
            "--no-renames",
            "--diff-filter=ACDMRTUXB",
            *revisions,
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr, end="")
        return None
    return [line for line in completed.stdout.splitlines() if line]


def write_outputs(result: dict[str, bool], output_path: str | None) -> None:
    lines = [f"{lane}={'true' if result[lane] else 'false'}" for lane in LANES]
    if output_path:
        with open(output_path, "a", encoding="utf-8") as output:
            output.write("\n".join(lines) + "\n")
    else:
        print("\n".join(lines))


def write_summary(paths: list[str] | None, result: dict[str, bool]) -> None:
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return

    selected = (
        ", ".join(lane for lane in LANES if result[lane])
        or "none (documentation checks run unconditionally)"
    )
    path_summary = (
        "full validation fallback" if paths is None else f"{len(paths)} changed path(s)"
    )
    with open(summary_path, "a", encoding="utf-8") as summary:
        summary.write("## CI impact\n\n")
        summary.write(f"- Diff: {path_summary}\n")
        summary.write(f"- Selected lanes: {selected}\n")
        if paths:
            summary.write("\n<details><summary>Changed paths</summary>\n\n```text\n")
            summary.write("\n".join(paths))
            summary.write("\n```\n</details>\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default=os.environ.get("BASE_SHA"))
    parser.add_argument("--head", default=os.environ.get("HEAD_SHA", "HEAD"))
    parser.add_argument("--event", default=os.environ.get("GITHUB_EVENT_NAME"))
    parser.add_argument("--output", default=os.environ.get("GITHUB_OUTPUT"))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    paths = changed_paths(args.base, args.head, args.event)
    if paths is not None:
        subprocess.run(
            [
                "git",
                "diff",
                "--check",
                *diff_revisions(args.base, args.head, args.event),
                "--",
                ".",
            ],
            check=True,
        )

    result = {lane: True for lane in LANES} if paths is None else classify_paths(paths)
    if paths is None:
        print("Unable to establish a complete base diff; selecting every CI lane.")
    else:
        print(f"Classified {len(paths)} changed path(s).")
        for path in paths:
            print(f"  {path}")

    write_outputs(result, args.output)
    write_summary(paths, result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
