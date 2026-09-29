from __future__ import annotations

import copy
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

from validate_detection import (
    FAULTS,
    ROOT,
    SLOW_DISPATCH,
    Fault,
    allocation_outcome,
    gtest_outcome,
    mutated,
    paired_outcome,
    replacement,
    require_success,
    run,
    snapshot,
)


class DetectionContractTest(unittest.TestCase):
    def test_every_fault_has_one_current_production_anchor(self) -> None:
        for fault in (*FAULTS, SLOW_DISPATCH):
            with self.subTest(fault=fault.name):
                self.assertTrue(fault.path.startswith(("src/", "include/")))
                original = (ROOT / fault.path).read_text(encoding="utf-8")
                self.assertNotEqual(replacement(original, fault), original)

    def test_stale_or_ambiguous_anchor_is_not_a_detected_fault(self) -> None:
        fault = FAULTS[0]
        for source in ("missing", fault.before + fault.before):
            with self.assertRaises(ValueError):
                replacement(source, fault)

    def test_mutation_restores_exact_bytes_even_after_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            source = tree / "source.cpp"
            original = b"before\r\n"
            source.write_bytes(original)
            fault = Fault("test", "source.cpp", "before", "after", "unused", "unused")
            with self.assertRaises(RuntimeError), mutated(tree, fault):
                self.assertEqual(source.read_bytes(), b"after\r\n")
                raise RuntimeError("test failed")
            self.assertEqual(source.read_bytes(), original)

    @patch.dict(os.environ)
    def test_snapshot_includes_working_changes_without_mutating_checkout(self) -> None:
        # Commit hooks export repository-local settings such as GIT_INDEX_FILE. They must not
        # redirect this independent fixture repository or its detached worktree to the caller.
        local_variables = subprocess.check_output(
            ["git", "rev-parse", "--local-env-vars"], text=True
        ).splitlines()
        for variable in local_variables:
            os.environ.pop(variable, None)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "repo"
            root.mkdir()
            output = Path(directory) / "artifacts"
            output.mkdir()

            def git(*args: str) -> str:
                return subprocess.check_output(["git", *args], cwd=root, text=True)

            git("init", "--quiet")
            (root / "source.cpp").write_text("committed", encoding="utf-8")
            git("add", "source.cpp")
            git(
                "-c",
                "user.name=Detector Test",
                "-c",
                "user.email=detector@example.invalid",
                "-c",
                "commit.gpgsign=false",
                "commit",
                "--quiet",
                "-m",
                "fixture",
            )
            (root / "source.cpp").write_text("staged", encoding="utf-8")
            git("add", "source.cpp")
            (root / "source.cpp").write_text("working", encoding="utf-8")
            (root / "new.cpp").write_text("untracked", encoding="utf-8")
            (root / "link.cpp").symlink_to("new.cpp")
            original_status = git("status", "--porcelain")
            with self.assertRaises(RuntimeError), snapshot(root, output) as tree:
                self.assertEqual((tree / "source.cpp").read_text(), "working")
                self.assertEqual((tree / "new.cpp").read_text(), "untracked")
                self.assertTrue((tree / "link.cpp").is_symlink())
                (tree / "source.cpp").write_text("mutated", encoding="utf-8")
                raise RuntimeError("abort detector")
            self.assertFalse(tree.exists())
            self.assertEqual((root / "source.cpp").read_text(), "working")
            self.assertEqual(git("status", "--porcelain"), original_status)
            self.assertEqual(git("show", ":source.cpp"), "staged")
            self.assertEqual(
                git("worktree", "list", "--porcelain").count("worktree "), 1
            )

    def test_setup_failures_and_timeouts_are_errors_not_detection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tree = Path(directory)
            with self.assertRaises(RuntimeError):
                require_success(
                    tree,
                    [sys.executable, "-c", "raise SystemExit(1)"],
                    tree / "build.log",
                    10,
                )
            with self.assertRaises(subprocess.TimeoutExpired):
                run(
                    tree,
                    [sys.executable, "-c", "import time; time.sleep(60)"],
                    tree / "timeout.log",
                    1,
                )

    def test_gtest_requires_the_selected_completed_assertion_failure(self) -> None:
        report: dict[str, Any] = {
            "tests": 1,
            "errors": 0,
            "failures": 1,
            "testsuites": [
                {
                    "name": "Suite",
                    "testsuite": [
                        {
                            "name": "Case",
                            "status": "RUN",
                            "result": "COMPLETED",
                            "failures": [{"failure": "expected value"}],
                        }
                    ],
                }
            ],
        }
        self.assertTrue(gtest_outcome(report, "Suite.Case", True))
        self.assertFalse(gtest_outcome(report, "Suite.Other", True))
        self.assertFalse(gtest_outcome(report, "Suite.Case", False))
        for key, value in (("tests", 0), ("tests", 2), ("errors", 1), ("failures", 0)):
            invalid = copy.deepcopy(report)
            invalid[key] = value
            self.assertFalse(gtest_outcome(invalid, "Suite.Case", True))
        for key, value in (
            ("status", "NOTRUN"),
            ("result", "SKIPPED"),
            ("failures", []),
        ):
            invalid = copy.deepcopy(report)
            invalid["testsuites"][0]["testsuite"][0][key] = value
            self.assertFalse(gtest_outcome(invalid, "Suite.Case", True))
        report["failures"] = 0
        report["testsuites"][0]["testsuite"][0]["failures"] = []
        self.assertTrue(gtest_outcome(report, "Suite.Case", False))

    def test_resource_failure_must_be_an_observed_allocation(self) -> None:
        report = {
            "schema": 2,
            "suite": "steady-state-allocation-audit",
            "status": "failed",
            "audited_iterations": 10_000,
            "general_allocation_calls": 10_000,
        }
        self.assertTrue(allocation_outcome(report, True))
        report["general_allocation_calls"] = 0
        self.assertFalse(allocation_outcome(report, True))
        report["status"] = "passed"
        self.assertTrue(allocation_outcome(report, False))
        report["audited_iterations"] = 0
        self.assertFalse(allocation_outcome(report, False))

    def test_performance_requires_paired_rejection_not_absolute_target_failure(
        self,
    ) -> None:
        report = {
            "schema": 1,
            "suite": "frame-paired-regression",
            "status": "failed",
            "comparisons": [
                {
                    "id": SLOW_DISPATCH.detector,
                    "status": "failed",
                    "candidate": 1000,
                    "maximum": 10,
                }
            ],
        }
        self.assertTrue(paired_outcome(report, True))
        report["comparisons"][0]["candidate"] = 1
        self.assertFalse(paired_outcome(report, True))
        report["comparisons"][0]["status"] = "passed"
        report["target_status"] = "failed"
        self.assertFalse(paired_outcome(report, True))
        report["status"] = "passed"
        self.assertTrue(paired_outcome(report, False))
        report["comparisons"][0]["id"] = "unrelated"
        self.assertFalse(paired_outcome(report, False))


if __name__ == "__main__":
    unittest.main()
