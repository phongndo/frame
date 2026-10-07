#!/usr/bin/env python3
"""Package, exercise, and describe Frame's relocatable native installation."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGETS = {
    ("Darwin", "arm64"): "aarch64-apple-darwin",
    ("Linux", "aarch64"): "aarch64-unknown-linux-gnu",
    ("Linux", "x86_64"): "x86_64-unknown-linux-gnu",
}
BINARIES = ("frame", "frame-ui", "frame-config-host", "frame-clipboard-host")
TAG_PATTERN = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?")


def release_tag() -> str:
    header = (ROOT / "include/frame/version.hpp").read_text()
    match = re.search(r'\bversion = "([^"]+)";', header)
    if match is None or TAG_PATTERN.fullmatch(f"v{match[1]}") is None:
        raise ValueError("include/frame/version.hpp has no release version")
    return f"v{match[1]}"


def native_target() -> str:
    return TARGETS[(platform.system(), platform.machine())]


def checksum(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_checksum(path: Path) -> str:
    match = re.fullmatch(r"([0-9a-f]{64})  ([^\r\n]+)\n?", path.read_text())
    if match is None or match[2] != path.name.removesuffix(".sha256"):
        raise ValueError(f"invalid checksum file: {path}")
    return match[1]


def check_linkage(binary: Path) -> None:
    if platform.system() == "Darwin":
        output = subprocess.check_output(["otool", "-L", str(binary)], text=True)
        for line in output.splitlines()[1:]:
            dependency = line.strip().split(" (", 1)[0]
            if not dependency.startswith(("/usr/lib/", "/System/Library/")):
                raise ValueError(
                    f"{binary} depends on a non-system library: {dependency}"
                )
    else:
        output = subprocess.check_output(["readelf", "-d", str(binary)], text=True)
        for dependency in re.findall(r"\(NEEDED\).*\[([^]]+)\]", output):
            if not re.fullmatch(
                r"lib(?:c|m|dl|pthread|rt)\.so\.[0-9]+|ld-linux[^/]*\.so\.[0-9]+",
                dependency,
            ):
                raise ValueError(
                    f"{binary} depends on a non-system library: {dependency}"
                )
        headers = subprocess.check_output(["readelf", "-l", str(binary)], text=True)
        if "/nix/store/" in output + headers:
            raise ValueError(
                f"{binary} requires the Nix store; build with native Clang"
            )


def copy_licenses(build: Path, prefix: Path) -> None:
    destination = prefix / "share/doc/frame/licenses"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "LICENSE", destination / "Frame.txt")
    packages = {}
    for metadata in (build / "conan").glob("*.cmake"):
        packages.update(
            re.findall(
                r'set\((\w+)_PACKAGE_FOLDER_RELEASE "([^"$]+)"\)', metadata.read_text()
            )
        )
    for name in ("lua", "zstd", "libpng", "zlib"):
        shutil.copytree(Path(packages[name]) / "licenses", destination / name)

    # Ghostty's optional components vary with its pin. Preserve upstream notices from
    # the pinned source and dependency cache instead of maintaining a copied inventory.
    sources = {
        "ghostty": Path(os.environ["FRAME_GHOSTTY_SOURCE_DIR"]),
        "ghostty-dependencies": Path(os.environ["FRAME_GHOSTTY_ZIG_SYSTEM_DIR"]),
    }
    for name, source in sources.items():
        count = 0
        for path in source.rglob("*"):
            if path.is_file() and path.name.upper().startswith(
                ("LICENSE", "COPYING", "COPYRIGHT", "NOTICE")
            ):
                target = destination / name / path.relative_to(source)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
                count += 1
        if count == 0:
            raise ValueError(f"no license notices in {source}")
    if platform.system() == "Linux":
        for pattern in ("libgcc-*-dev/copyright", "libstdc++-*-dev/copyright"):
            notices = list(Path("/usr/share/doc").glob(pattern))
            if not notices:
                raise ValueError(f"missing native runtime notice: {pattern}")
            for notice in notices:
                shutil.copy2(notice, destination / f"{notice.parent.name}.txt")


def package(build: Path, output: Path) -> Path:
    tag = release_tag()
    name = f"frame-{tag}-{native_target()}"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="frame-package-") as temporary:
        prefix = Path(temporary) / name
        subprocess.run(
            ["cmake", "--install", str(build), "--prefix", str(prefix)], check=True
        )
        for name in BINARIES:
            check_linkage(prefix / "bin" / name)
        version = subprocess.check_output(
            [str(prefix / "bin/frame"), "--version"], text=True
        )
        if not version.startswith(f"frame {tag[1:]} "):
            raise ValueError(f"binary version differs from {tag}: {version}")
        copy_licenses(build, prefix)
        shutil.copy2(ROOT / "README.md", prefix / "README.md")
        shutil.copy2(ROOT / "LICENSE", prefix / "LICENSE")
        revision = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        )
        (prefix / "REVISION").write_text(revision)
        archive = output / f"{prefix.name}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(prefix, arcname=prefix.name)
    archive.with_suffix(".gz.sha256").write_text(
        f"{checksum(archive)}  {archive.name}\n"
    )
    return archive


def smoke(executable: Path) -> None:
    # A short runtime path also fits Darwin's sockaddr_un. No user's daemon or
    # configuration is involved, including when run through a Homebrew symlink.
    with tempfile.TemporaryDirectory(prefix="frame-dist-", dir="/tmp") as temporary:
        root = Path(temporary)
        home = root / "home"
        home.mkdir()
        environment = {
            "PATH": "/usr/bin:/bin",
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(home / ".config"),
            "TERM": "xterm-256color",
            "FRAME_DEV_RUNTIME_DIR": str(root),
            "FRAME_DEV_BUILD_ID": "0" * 64,
        }

        def command(*arguments: str) -> str:
            result = subprocess.run(
                [str(executable), *arguments],
                cwd=home,
                env=environment,
                capture_output=True,
                text=True,
                timeout=15,
                check=False,
            )
            if result.returncode:
                raise RuntimeError(f"{arguments}: {result.stdout}\n{result.stderr}")
            return result.stdout

        command("--version")
        command("config", "check")
        try:
            started = json.loads(
                command(
                    "session",
                    "start",
                    "install-test",
                    "--cwd",
                    str(home),
                    "--hold",
                    "--",
                    "/bin/sh",
                    "-c",
                    'test "$TERM" = frame && infocmp -x "$TERM" && '
                    'printf "FRAME_INSTALL_OK\\n"',
                )
            )["results"][0]["result"]
            target = ("--session", "install-test", "--pane", started["pane"])
            try:
                command("wait", *target, "--exit-code", "0", "--timeout", "10s")
            except RuntimeError as error:
                captured = command(
                    "capture", *target, "--source", "recent", "--lines", "250"
                )
                raise RuntimeError(f"{error}\nTerminal output:\n{captured}") from error
            captured = command(
                "capture", *target, "--source", "recent", "--lines", "250"
            )
            if (
                "FRAME_INSTALL_OK" not in captured
                or "Frame terminal multiplexer" not in captured
            ):
                raise RuntimeError(
                    f"installed terminal did not run correctly: {captured}"
                )
            command("kill", "install-test")
            deadline = time.monotonic() + 5
            while (root / "daemon.sock").exists():
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        "installed daemon did not shut down after its last session"
                    )
                time.sleep(0.01)
        finally:
            if (root / "daemon.sock").exists():
                subprocess.run(
                    [str(executable), "shutdown", "--confirm"],
                    env=environment,
                    capture_output=True,
                    timeout=10,
                    check=False,
                )


def check_archive(archive: Path) -> None:
    if checksum(archive) != read_checksum(archive.with_suffix(".gz.sha256")):
        raise ValueError("archive checksum mismatch")
    with tempfile.TemporaryDirectory(prefix="frame-install-") as temporary:
        root = Path(temporary)
        environment = dict(
            os.environ,
            FRAME_VERSION=release_tag(),
            FRAME_INSTALL_DIR=str(root / "command directory"),
            FRAME_DATA_DIR=str(root / "release directory"),
            FRAME_RELEASE_BASE_URL=archive.parent.resolve().as_uri(),
        )
        executable = Path(environment["FRAME_INSTALL_DIR"]) / "frame"
        for _ in range(2):
            subprocess.run(
                ["sh", str(ROOT / "scripts/install.sh")], env=environment, check=True
            )
            smoke(executable)
        # A failed update must preserve both the entry point and the running release.
        installed = executable.readlink()
        broken = root / "broken release"
        broken.mkdir()
        shutil.copy2(archive, broken / archive.name)
        (broken / f"{archive.name}.sha256").write_text(f"{'0' * 64}  {archive.name}\n")
        environment["FRAME_RELEASE_BASE_URL"] = broken.as_uri()
        result = subprocess.run(
            ["sh", str(ROOT / "scripts/install.sh")],
            env=environment,
            capture_output=True,
        )
        if result.returncode == 0 or executable.readlink() != installed:
            raise RuntimeError("corrupt update changed the installation")
        smoke(executable)


def formula(tag: str, checksums: Path) -> str:
    if TAG_PATTERN.fullmatch(tag) is None:
        raise ValueError(f"invalid release tag: {tag}")
    lines = [
        "class Frame < Formula",
        '  desc "Fast, extensible terminal multiplexer"',
        '  homepage "https://github.com/phongndo/frame"',
        f'  version "{tag[1:]}"',
        '  license "MIT"',
        "",
    ]
    for system in ("Darwin", "Linux"):
        lines.append(f"  on_{'macos' if system == 'Darwin' else 'linux'} do")
        if system == "Darwin":
            lines.extend(
                ["    depends_on arch: :arm64", "    depends_on macos: :sonoma", ""]
            )
        for (target_system, arch), target in TARGETS.items():
            if target_system != system:
                continue
            asset = f"frame-{tag}-{target}.tar.gz"
            digest = read_checksum(checksums / f"{asset}.sha256")
            lines.extend(
                [
                    f"    on_{'intel' if arch == 'x86_64' else 'arm'} do",
                    f'      url "https://github.com/phongndo/frame/releases/download/{tag}/{asset}"',
                    f'      sha256 "{digest}"',
                    "    end",
                    "",
                ]
            )
        lines[-1] = "  end"
        lines.append("")
    lines.extend(
        [
            "  def install",
            '    bin.install Dir["bin/*"]',
            '    share.install Dir["share/*"]',
            "  end",
            "",
            "  test do",
            '    ENV["XDG_CONFIG_HOME"] = testpath/"config"',
            '    ENV["FRAME_DEV_RUNTIME_DIR"] = testpath',
            '    ENV["FRAME_DEV_BUILD_ID"] = "0" * 64',
            '    assert_match "frame #{version} ", shell_output("#{bin}/frame --version")',
            '    system bin/"frame", "config", "check"',
            "    started = shell_output(\"#{bin}/frame session start brew-test --hold -- /bin/sh -c 'printf FRAME_BREW_OK'\")",
            '    pane = JSON.parse(started).fetch("results").first.fetch("result").fetch("pane")',
            "    begin",
            '      system bin/"frame", "wait", "--session", "brew-test", "--pane", pane, "--exit-code", "0", "--timeout", "10s"',
            '      assert_match "FRAME_BREW_OK", shell_output("#{bin}/frame capture --session brew-test --pane #{pane}")',
            "    ensure",
            '      system bin/"frame", "kill", "brew-test"',
            "    end",
            "  end",
            "end",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("version")
    packaging = commands.add_parser("package")
    packaging.add_argument("--build", type=Path, default=ROOT / "build/release")
    packaging.add_argument("--output", type=Path, default=ROOT / "build/distribution")
    checking = commands.add_parser("check")
    checking.add_argument("archive", type=Path)
    smoking = commands.add_parser("smoke")
    smoking.add_argument("executable", type=Path)
    generating = commands.add_parser("formula")
    generating.add_argument("--tag", required=True)
    generating.add_argument("--checksums", required=True, type=Path)
    generating.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "version":
        print(release_tag())
    elif args.command == "package":
        print(package(args.build, args.output))
    elif args.command == "check":
        check_archive(args.archive.resolve())
        print(f"Installation checks passed: {args.archive.name}")
    elif args.command == "smoke":
        smoke(args.executable.absolute())
        print("Installed session check passed")
    elif args.command == "formula":
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(formula(args.tag, args.checksums))


if __name__ == "__main__":
    main()
