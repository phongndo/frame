"""Git fixtures run inside hooks must not touch the hook caller's repository."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


class GitFixtureIsolationTest(unittest.TestCase):
    @patch.dict(os.environ)
    def test_hook_context_cannot_redirect_fixture_commits_or_configuration(
        self,
    ) -> None:
        for variable in subprocess.check_output(
            ["git", "rev-parse", "--local-env-vars"], text=True
        ).splitlines():
            os.environ.pop(variable, None)
        with tempfile.TemporaryDirectory() as temporary:
            caller = Path(temporary)

            def git(*arguments: str) -> str:
                return subprocess.check_output(
                    ["git", *arguments], cwd=caller, text=True, stderr=subprocess.PIPE
                )

            git("init", "--quiet")
            (caller / "sentinel").write_text("do not change\n")
            git("add", "sentinel")
            git(
                "-c",
                "user.name=Isolation Test",
                "-c",
                "user.email=isolation@example.invalid",
                "-c",
                "commit.gpgsign=false",
                "commit",
                "--quiet",
                "-m",
                "sentinel",
            )
            head = git("rev-parse", "HEAD")
            metadata = caller / ".git"
            original = {
                name: (metadata / name).read_bytes() for name in ("index", "config")
            }
            environment = dict(
                os.environ,
                GIT_DIR=str(metadata),
                GIT_WORK_TREE=str(caller),
                GIT_INDEX_FILE=str(metadata / "index"),
            )
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "unittest",
                    "tools.test_ci_ghostty_pin",
                    "tools.test_ci_hooks.FileSizeHookTest",
                ],
                cwd=ROOT,
                env=environment,
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(git("rev-parse", "HEAD"), head)
            for name, contents in original.items():
                self.assertEqual((metadata / name).read_bytes(), contents, name)


if __name__ == "__main__":
    unittest.main()
