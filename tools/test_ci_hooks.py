from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
CHECK_FILE_SIZE = ROOT / "scripts" / "ci" / "check-file-size.py"
LIMIT = 500 * 1024


class FileSizeHookTest(unittest.TestCase):
    def setUp(self) -> None:
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        # A hook's Git environment belongs to its caller, never this fixture.
        for variable in subprocess.check_output(
            ["git", "rev-parse", "--local-env-vars"], text=True
        ).splitlines():
            os.environ.pop(variable, None)
        self.temporary = tempfile.TemporaryDirectory()
        self.repository = Path(self.temporary.name)
        self.git("init", "--quiet")
        self.git("config", "user.name", "Hook Test")
        self.git("config", "user.email", "hook-test@example.invalid")
        tracked = self.repository / "tracked.bin"
        tracked.write_bytes(b"small\n")
        self.git("add", "tracked.bin")
        self.git("commit", "--quiet", "-m", "fixture")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def git(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", *arguments],
            cwd=self.repository,
            check=True,
            capture_output=True,
            text=True,
        )

    def check(self, *paths: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["python3", str(CHECK_FILE_SIZE), *paths],
            cwd=self.repository,
            check=False,
            capture_output=True,
            text=True,
        )

    @staticmethod
    def write_size(path: Path, size: int) -> None:
        path.write_bytes((b"x" * (size - 1)) + b"\n")

    def test_staged_blob_owns_boundary_decision_for_space_name(self) -> None:
        path = self.repository / "exact boundary.bin"
        self.write_size(path, LIMIT)
        self.git("add", path.name)
        self.write_size(path, LIMIT + 1)

        accepted = self.check(path.name)

        self.assertEqual(accepted.returncode, 0, accepted.stderr)

        self.write_size(path, LIMIT + 1)
        self.git("add", path.name)
        self.write_size(path, LIMIT)

        rejected = self.check(path.name)

        self.assertEqual(rejected.returncode, 1)
        self.assertIn(f"{LIMIT + 1} bytes (maximum {LIMIT})", rejected.stderr)

    def test_modified_oversized_staged_file_is_rejected(self) -> None:
        path = self.repository / "tracked.bin"
        self.write_size(path, LIMIT + 1)
        self.git("add", path.name)
        self.write_size(path, LIMIT)

        result = self.check(path.name)

        self.assertEqual(result.returncode, 1)
        self.assertIn("tracked.bin", result.stderr)

    def test_worktree_fallback_rejects_oversized_regular_file(self) -> None:
        path = self.repository / "worktree only.bin"
        self.write_size(path, LIMIT + 1)

        result = self.check(path.name)

        self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
