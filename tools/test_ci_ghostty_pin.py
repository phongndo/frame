import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
VALIDATOR = ROOT / "cmake" / "ValidateGhosttyPin.cmake"
PINNED_COMMIT = json.loads(
    (ROOT / "third_party" / "ghostty-metadata" / "PIN.json").read_text(encoding="utf-8")
)["commit"]


class GhosttyPinValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        # CMake also invokes Git, so isolate the entire fixture's environment.
        for variable in subprocess.check_output(
            ["git", "rev-parse", "--local-env-vars"], text=True
        ).splitlines():
            os.environ.pop(variable, None)
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.pin_file = self.root / "PIN.json"
        self.source = self.root / "ghostty"
        self.source.mkdir()
        self.write_pin(PINNED_COMMIT)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_pin(self, commit: str) -> None:
        self.pin_file.write_text(json.dumps({"commit": commit}), encoding="utf-8")

    def run_validator(self, **defines: str) -> subprocess.CompletedProcess[str]:
        command = ["cmake"]
        for name, value in defines.items():
            command.append(f"-D{name}={value}")
        command.extend(["-P", str(VALIDATOR)])
        return subprocess.run(
            command,
            cwd=self.root,
            text=True,
            capture_output=True,
            check=False,
        )

    def git(self, *args: str, cwd: Path | None = None) -> None:
        completed = subprocess.run(
            ["git", "-c", "commit.gpgsign=false", *args],
            cwd=cwd or self.source,
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0:
            raise AssertionError(completed.stderr)

    def git_head(self) -> str:
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.source,
            text=True,
            capture_output=True,
            check=True,
        )
        return completed.stdout.strip()

    def init_source_repo(self, *, dirty: bool = False) -> str:
        self.git("init")
        self.git("config", "user.email", "frame@example.com")
        self.git("config", "user.name", "Frame")
        (self.source / "build.zig").write_text("/* test */\n", encoding="utf-8")
        self.git("add", "build.zig")
        self.git("commit", "-m", "init")
        commit = self.git_head()
        self.write_pin(commit)
        if dirty:
            (self.source / "dirty.txt").write_text("dirty\n", encoding="utf-8")
        return commit

    def test_uninitialized_directory_does_not_use_parent_git(self) -> None:
        self.git("init", cwd=self.root)
        result = self.run_validator(
            GHOSTTY_PIN_FILE=str(self.pin_file),
            GHOSTTY_SOURCE_DIR=str(self.source),
            GHOSTTY_GIT_EXECUTABLE="git",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("submodule is not initialized", result.stderr)
        self.assertNotIn("submodule mismatch", result.stderr)

    def test_nix_attestation_rejects_a_non_store_path(self) -> None:
        result = self.run_validator(
            GHOSTTY_PIN_FILE=str(self.pin_file),
            GHOSTTY_SOURCE_DIR=str(self.source),
            GHOSTTY_NIX_SOURCE_REV=PINNED_COMMIT,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must reside in /nix/store", result.stderr)

    def test_nix_attestation_rejects_a_revision_mismatch(self) -> None:
        result = self.run_validator(
            GHOSTTY_PIN_FILE=str(self.pin_file),
            GHOSTTY_SOURCE_DIR=f"/nix/store/{'0' * 32}-ghostty",
            GHOSTTY_NIX_SOURCE_REV="deadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("flake input mismatch", result.stderr)

    def test_matching_clean_checkout_is_accepted(self) -> None:
        self.init_source_repo()
        result = self.run_validator(
            GHOSTTY_PIN_FILE=str(self.pin_file),
            GHOSTTY_SOURCE_DIR=str(self.source),
            GHOSTTY_GIT_EXECUTABLE="git",
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_flake_lock_matches_pin_json(self) -> None:
        pin = json.loads(
            (ROOT / "third_party" / "ghostty-metadata" / "PIN.json").read_text(
                encoding="utf-8"
            )
        )
        lock = json.loads((ROOT / "flake.lock").read_text(encoding="utf-8"))
        self.assertEqual(
            lock["nodes"]["ghosttySource"]["locked"]["rev"],
            pin["commit"],
        )

    def install_patch(self, content: str | None = None) -> Path:
        patch = self.root / "patches" / "test.patch"
        patch.parent.mkdir(exist_ok=True)
        patch.write_text(
            content
            if content is not None
            else "--- a/build.zig\n+++ b/build.zig\n@@ -1 +1 @@\n-/* test */\n+/* patched */\n",
            encoding="utf-8",
        )
        pin = json.loads(self.pin_file.read_text(encoding="utf-8"))
        pin["patches"] = [
            {
                "file": "patches/test.patch",
                "sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
            }
        ]
        self.pin_file.write_text(json.dumps(pin), encoding="utf-8")
        return patch

    def prepare_source(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "cmake",
                f"-DGHOSTTY_PIN_FILE={self.pin_file}",
                f"-DGHOSTTY_SOURCE_DIR={self.source}",
                "-DGHOSTTY_GIT_EXECUTABLE=git",
                f"-DGHOSTTY_PATCHED_SOURCE_DIR={self.root / 'prepared'}",
                "-P",
                str(ROOT / "cmake" / "PrepareGhosttySource.cmake"),
            ],
            cwd=self.root,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_patch_applies_only_to_private_copy_and_is_reusable(self) -> None:
        self.init_source_repo()
        self.install_patch()
        for _ in range(2):
            result = self.prepare_source()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                (self.source / "build.zig").read_text(encoding="utf-8"),
                "/* test */\n",
            )
            self.assertEqual(
                (self.root / "prepared" / "build.zig").read_text(encoding="utf-8"),
                "/* patched */\n",
            )
            self.assertFalse((self.root / "prepared" / ".git").exists())

    def test_changed_patch_is_rejected_even_with_prepared_cache(self) -> None:
        self.init_source_repo()
        patch = self.install_patch()
        self.assertEqual(self.prepare_source().returncode, 0)
        patch.write_text("tampered\n", encoding="utf-8")
        result = self.prepare_source()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("patch hash mismatch", result.stderr)

    def test_missing_patch_is_rejected(self) -> None:
        self.init_source_repo()
        self.install_patch().unlink()
        result = self.prepare_source()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing Ghostty patch", result.stderr)

    def test_failed_patch_does_not_publish_prepared_source(self) -> None:
        self.init_source_repo()
        self.install_patch(
            "--- a/build.zig\n+++ b/build.zig\n@@ -1 +1 @@\n-wrong base\n+patched\n"
        )
        result = self.prepare_source()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Ghostty patch failed", result.stderr)
        self.assertFalse((self.root / "prepared" / "build.zig").exists())
        self.assertFalse((self.root / "prepared.tmp").exists())

    def test_changed_pin_rebuilds_prepared_source(self) -> None:
        self.init_source_repo()
        self.install_patch()
        self.assertEqual(self.prepare_source().returncode, 0)
        self.install_patch(
            "--- a/build.zig\n+++ b/build.zig\n@@ -1 +1 @@\n-/* test */\n+/* replacement */\n"
        )
        result = self.prepare_source()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.root / "prepared" / "build.zig").read_text(encoding="utf-8"),
            "/* replacement */\n",
        )

    def test_non_array_patches_are_rejected(self) -> None:
        commit = self.init_source_repo()
        self.pin_file.write_text(
            json.dumps({"commit": commit, "patches": "patches/test.patch"}),
            encoding="utf-8",
        )
        result = self.prepare_source()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("patches must be an array", result.stderr)

    def test_patch_path_cannot_escape_metadata(self) -> None:
        self.init_source_repo()
        self.install_patch()
        pin = json.loads(self.pin_file.read_text(encoding="utf-8"))
        pin["patches"][0]["file"] = "../test.patch"
        self.pin_file.write_text(json.dumps(pin), encoding="utf-8")
        result = self.prepare_source()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid Ghostty patch path", result.stderr)

    def test_dirty_checkout_is_rejected(self) -> None:
        self.init_source_repo(dirty=True)
        result = self.run_validator(
            GHOSTTY_PIN_FILE=str(self.pin_file),
            GHOSTTY_SOURCE_DIR=str(self.source),
            GHOSTTY_GIT_EXECUTABLE="git",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be clean", result.stderr)


if __name__ == "__main__":
    unittest.main()
