"""Installation failures must not replace an existing command or working release."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "distribution", ROOT / "scripts/ci/distribution.py"
)
assert spec is not None and spec.loader is not None
distribution = importlib.util.module_from_spec(spec)
spec.loader.exec_module(distribution)


class InstallerTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="frame-installer-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.download = self.root / "downloads"
        self.download.mkdir()
        self.target = distribution.native_target()
        self.environment = dict(
            os.environ,
            FRAME_VERSION="v9.8.7",
            FRAME_INSTALL_DIR=str(self.root / "commands with spaces"),
            FRAME_DATA_DIR=str(self.root / "data with spaces"),
            FRAME_RELEASE_BASE_URL=self.download.as_uri(),
        )
        self.executable = Path(self.environment["FRAME_INSTALL_DIR"]) / "frame"

    def archive(self, *, missing_helper: bool = False) -> Path:
        name = f"frame-v9.8.7-{self.target}"
        prefix = self.root / name
        (prefix / "bin").mkdir(parents=True)
        (prefix / "share/terminfo").mkdir(parents=True)
        for name in distribution.BINARIES:
            if missing_helper and name == "frame-config-host":
                continue
            binary = prefix / "bin" / name
            binary.write_text("#!/bin/sh\nprintf 'frame 9.8.7\\n'\n")
            binary.chmod(0o755)
        archive = self.download / f"{prefix.name}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(prefix, arcname=prefix.name)
        archive.with_suffix(".gz.sha256").write_text(
            f"{distribution.checksum(archive)}  {archive.name}\n"
        )
        return archive

    def install(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["sh", str(ROOT / "scripts/install.sh")],
            env=self.environment,
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
        )

    def test_reinstall_preserves_old_release_for_running_processes(self) -> None:
        self.archive()
        first = self.install()
        self.assertEqual(first.returncode, 0, first.stderr)
        previous = self.executable.resolve()
        second = self.install()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertNotEqual(previous, self.executable.resolve())
        self.assertTrue(previous.is_file())
        self.assertTrue((previous.parent / "frame-config-host").is_file())

    def test_corrupt_update_leaves_installed_command_unchanged(self) -> None:
        archive = self.archive()
        installed = self.install()
        self.assertEqual(installed.returncode, 0, installed.stderr)
        previous = self.executable.readlink()
        archive.write_bytes(b"corrupted download")
        failed = self.install()
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("checksum verification failed", failed.stderr)
        self.assertEqual(previous, self.executable.readlink())

    def test_unrelated_command_is_not_replaced(self) -> None:
        self.executable.parent.mkdir()
        self.executable.write_text("user-owned command")
        failed = self.install()
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(self.executable.read_text(), "user-owned command")

    def test_incomplete_package_is_not_installed(self) -> None:
        self.archive(missing_helper=True)
        failed = self.install()
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("archive is missing frame-config-host", failed.stderr)
        self.assertFalse(self.executable.exists())


class TerminfoTest(unittest.TestCase):
    def test_compiled_entry_is_readable_by_legacy_ncurses(self) -> None:
        with tempfile.TemporaryDirectory(prefix="frame-terminfo-") as temporary:
            output = Path(temporary)
            subprocess.run(
                ["tic", "-x", "-o", str(output), str(ROOT / "terminfo/frame.terminfo")],
                check=True,
                capture_output=True,
            )
            entries = list(output.glob("*/frame"))
            self.assertEqual(len(entries), 1)
            compiled = entries[0].read_bytes()
            # macOS's system ncurses cannot read 32-bit numbers or large entries.
            self.assertEqual(compiled[:2], b"\x1a\x01")
            self.assertLessEqual(len(compiled), 4096)
            subprocess.run(
                ["infocmp", "-x", "-A", str(output), "frame"],
                check=True,
                capture_output=True,
            )


class FormulaTest(unittest.TestCase):
    def test_checksum_must_name_the_matching_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "frame.tar.gz.sha256"
            path.write_text(f"{'a' * 64}  unrelated.tar.gz\n")
            with self.assertRaisesRegex(ValueError, "invalid checksum"):
                distribution.read_checksum(path)


if __name__ == "__main__":
    unittest.main()
