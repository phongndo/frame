#!/usr/bin/env python3
"""Bounded process-level core-mux workloads with identical completion semantics."""

from __future__ import annotations

import argparse
import concurrent.futures
import ctypes
import hashlib
import json
import math
import os
import platform
import pwd
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Protocol

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from benchmarks.benchmark_manifest import (  # noqa: E402
    ManifestError,
    expected_failure,
    load_manifest,
    suite_workloads,
    unsupported_result,
    workload_for_mode,
)
from tests.support.mux_harness import LEMMA_OUTER_TERMINAL_RESTORE  # noqa: E402
from tests.support.pty_process import PtyOutputMonitor, PtyProcess  # noqa: E402

# Repository scans include the Ghostty submodule and can exceed two seconds on
# cold hosted runners. Metadata is outside the measured interaction.
GIT_METADATA_TIMEOUT_SECONDS = 30.0

ALT_SCREEN = b"\x1b[?1049h"
# The paired harness also runs pre-2.11 baselines, which never save/enable native geometry reports.
LEGACY_LEMMA_OUTER_TERMINAL_RESTORE = (
    b"\x1b[0m\x1b[?2026l\x1b[?1l\x1b[?9l\x1b[?1000l\x1b[?1002l\x1b[?1003l"
    b"\x1b[?1004l\x1b[?1005l\x1b[?1006l\x1b[?1007l\x1b[?1015l\x1b[?1016l"
    b"\x1b[?2004l\x1b]112\x1b\\\x1b[0 q\x1b[?25h\x1b[?7h\x1b[<u\x1b[?1049l"
)
# Baselines before outer-title support neither push nor restore the outer window title.
PRE_TITLE_LEMMA_OUTER_TERMINAL_RESTORE = (
    b"\x18\x1b_Gq=2,m=0;\x1b\\\x1b_Ga=d,d=A,q=2\x1b\\"
    b"\x1b[0m\x1b[?2026l\x1b[?1l\x1b[?9l\x1b[?1000l\x1b[?1002l\x1b[?1003l"
    b"\x1b[?1004l\x1b[?1005l\x1b[?1006l\x1b[?1007l\x1b[?1015l\x1b[?1016l"
    b"\x1b[?2004l\x1b]112\x1b\\\x1b[0 q\x1b[?25h\x1b[?7h\x1b[<u\x1b[?2048r\x1b[?1049l"
)
WARM_MARKER = b"__LEMMA_WARM_SCROLL_DONE__"
WARM_READY_MARKER = b"__LEMMA_WARM_SCROLL_READY__"
BLOCK_READY = b"__LEMMA_PTY_READY__"
BLOCK_DONE = b"__LEMMA_PTY_DONE__ bytes=2097152 digest=d939b04ca2c22325"
LATENCY_READY = b"__LEMMA_LATENCY_READY__"
LATENCY_OUTPUT_READY = b"__LEMMA_LATENCY_OUTPUT_READY__"
LATENCY_VISIBLE_ACK = b"__LEMMA_LATENCY_VISIBLE__"
LATENCY_NEXT_READY = b"__LEMMA_LATENCY_NEXT__"
TUI_REDRAW_READY = b"__LEMMA_TUI_REDRAW_READY__"
TUI_WHEEL_READY = b"__LEMMA_TUI_WHEEL_READY__"
IDLE_READY = b"__LEMMA_IDLE_READY__"
PAINT_READY = b"__LEMMA_PAINT_READY__"
RESIZE_READY = b"__LEMMA_RESIZE_READY__"
RESIZE_ARMED = b"__LEMMA_RESIZE_ARMED__"
# Alternate across the 80x24 baseline so every sample changes both pane dimensions.
RESIZE_GEOMETRIES = ((100, 30), (80, 24))
RESIZE_QUIET_SECONDS = 0.3
# Each navigation trigger is one terminal write through the attached client. Bindings are the
# subject defaults except where noted: Zellij's default new-tab path needs a mode round trip, so its
# benchmark config binds Alt t directly; Herdr leaves workspace cycling unbound by default, so its
# benchmark config binds prefix+( and prefix+). Lemma switches Sessions through its native command
# line (`switch NAME`). Zellij has no in-client Session switch trigger; see the workload manifest.
NAVIGATION_KEYS: dict[str, dict[str, bytes]] = {
    "lemma": {
        "new_pane": b"\x02%",
        "new_tab": b"\x02c",
        "next_tab": b"\x02n",
        "previous_tab": b"\x02p",
    },
    "tmux": {
        "new_pane": b"\x02%",
        "new_tab": b"\x02c",
        "next_tab": b"\x02n",
        "previous_tab": b"\x02p",
        "next_session": b"\x02)",
        "previous_session": b"\x02(",
    },
    "zellij": {
        "new_pane": b"\x1bn",
        "new_tab": b"\x1bt",
        "next_tab": b"\x1bl",
        "previous_tab": b"\x1bh",
    },
    "herdr": {
        "new_pane": b"\x02v",
        "new_tab": b"\x02c",
        "next_tab": b"\x02n",
        "previous_tab": b"\x02p",
        "next_session": b"\x02)",
        "previous_session": b"\x02(",
    },
}
ATTACH_VISIBLE_MARKER = b"__LEMMA_ATTACH_VISIBLE__"
# Keep the first byte distinct from fixture markers. Differential terminal renderers can retain a
# shared prefix on screen without retransmitting it to an attached outer client.
SHELL_READY_MARKER = b"LEMMA-SHELL-READY"
ATTACH_MAGIC = b"\x89LMA"
ATTACH_PROTOCOL_MAJOR = 2
ATTACH_PROTOCOL_MINOR = 11
# These known versions share the hello/input wire layouts used by the paired harness.
ATTACH_SUPPORTED_MINORS = (9, 10, 11)
ATTACH_HEADER_BYTES = 16
ATTACH_KIND_HELLO = 1
ATTACH_KIND_INPUT = 2
PAYLOAD_SIZE = 2 * 1024 * 1024
TUI_WHEEL_BURST_SIZE = 64
BLOCKED_CLIENT_NO_PROGRESS_TIMEOUT_NS = 5_000_000_000
# The workload starts its clock at the flood command rather than at the first queued daemon frame.
# Allow a bounded startup and CLI polling margin without weakening the five-second contract.
BLOCKED_CLIENT_DISCONNECT_TOLERANCE_NS = 500_000_000
BLOCKED_CLIENT_DISCONNECT_DEADLINE_NS = (
    BLOCKED_CLIENT_NO_PROGRESS_TIMEOUT_NS + BLOCKED_CLIENT_DISCONNECT_TOLERANCE_NS
)
ATTACH_STARTUP_SHELLS = {
    "sh",
    "dash",
    "ksh",
    "mksh",
    "bash",
    "zsh",
    "fish",
    "nu",
    "nushell",
}
EXTENSION_FIXTURE_MODES = (
    "idle-peers",
    "idle-surfaces",
    "changing-rows",
    "storm",
    "blocked-reader",
    "slow-producer",
    "crash-focused",
    "crash-docked",
)
# Zellij's list-panes action took about 3 s per call at 40-64 panes on the approved host.
ZELLIJ_PANE_LISTING_SECONDS = 30.0
HERDR_BENCHMARK_CONFIG = """onboarding = false

[update]
version_check = false
manifest_check = false

[ui]
sidebar_start_collapsed = true
sidebar_collapsed_mode = "hidden"
pane_scrollbars = false
mouse_capture = false
redraw_on_focus_gained = false
confirm_close = false
prompt_new_tab_name = false
prompt_new_workspace_name = false

[ui.sound]
enabled = false

[experimental]
pane_history = false

[terminal]
shell_mode = "login"

[keys]
next_workspace = "prefix+)"
previous_workspace = "prefix+("
"""


def darwin_sysctl(name: str) -> str | None:
    if platform.system() != "Darwin":
        return None
    try:
        value = subprocess.run(
            ["/usr/sbin/sysctl", "-n", name],
            check=True,
            capture_output=True,
            text=True,
            timeout=2.0,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return value or None


def linux_host_metadata(
    cpuinfo: str, meminfo: str, model_identifier: str | None
) -> dict[str, Any]:
    cpu_model: str | None = None
    physical_cores: set[tuple[str, str]] = set()
    for record in cpuinfo.split("\n\n"):
        fields = {
            key.strip(): value.strip()
            for line in record.splitlines()
            if ":" in line
            for key, value in (line.split(":", 1),)
        }
        cpu_model = cpu_model or fields.get("model name")
        if "physical id" in fields and "core id" in fields:
            physical_cores.add((fields["physical id"], fields["core id"]))
    memory_bytes: int | None = None
    for line in meminfo.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0] == "MemTotal:" and fields[2] == "kB":
            try:
                memory_bytes = int(fields[1]) * 1024
            except ValueError:
                pass
            break
    return {
        "model_identifier": model_identifier.strip() if model_identifier else None,
        "cpu_model": cpu_model,
        "physical_cpu_count": len(physical_cores) or None,
        "memory_bytes": memory_bytes,
    }


def host_fingerprint() -> dict[str, Any]:
    def integer_sysctl(name: str) -> int | None:
        value = darwin_sysctl(name)
        try:
            return int(value) if value is not None else None
        except ValueError:
            return None

    metadata: dict[str, Any]
    if platform.system() == "Linux":
        try:
            cpuinfo = Path("/proc/cpuinfo").read_text(encoding="utf-8")
            meminfo = Path("/proc/meminfo").read_text(encoding="utf-8")
        except OSError:
            cpuinfo = ""
            meminfo = ""
        try:
            model_identifier = Path("/sys/class/dmi/id/product_name").read_text(
                encoding="utf-8"
            )
        except OSError:
            model_identifier = None
        metadata = linux_host_metadata(cpuinfo, meminfo, model_identifier)
    else:
        metadata = {
            "model_identifier": darwin_sysctl("hw.model"),
            "cpu_model": darwin_sysctl("machdep.cpu.brand_string"),
            "physical_cpu_count": integer_sysctl("hw.physicalcpu"),
            "memory_bytes": integer_sysctl("hw.memsize"),
        }
    return {"host_name": platform.node(), **metadata}


def local_socket_peer_pid(path: Path) -> int:
    peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        peer.connect(str(path))
        if platform.system() == "Darwin":
            # Darwin's LOCAL_PEERPID is option 2 at SOL_LOCAL (level 0).
            encoded = peer.getsockopt(0, 2, struct.calcsize("i"))
            process = struct.unpack("i", encoded)[0]
        elif platform.system() == "Linux":
            # Linux exposes pid, uid, and gid through SO_PEERCRED.
            encoded = peer.getsockopt(
                socket.SOL_SOCKET,
                getattr(socket, "SO_PEERCRED", 17),
                struct.calcsize("3i"),
            )
            process, _, _ = struct.unpack("3i", encoded)
        else:
            raise RuntimeError("local socket peer PID is unsupported on this platform")
    finally:
        peer.close()
    if process <= 0:
        raise RuntimeError("local socket reported an invalid peer PID")
    return process


def attach_frame(
    kind: int,
    payload: bytes,
    sequence: int,
    flags: int = 0,
    *,
    minor: int = ATTACH_PROTOCOL_MINOR,
) -> bytes:
    if not 0 < sequence <= 0xFFFF_FFFF or len(payload) > 0xFFFF_FFFF:
        raise ValueError("private attach frame exceeds its wire bounds")
    return (
        struct.pack(
            "!4sBBBBII",
            ATTACH_MAGIC,
            ATTACH_PROTOCOL_MAJOR,
            minor,
            kind,
            flags,
            len(payload),
            sequence,
        )
        + payload
    )


def receive_exact(peer: socket.socket, size: int) -> bytes:
    received = bytearray()
    while len(received) < size:
        fragment = peer.recv(size - len(received))
        if not fragment:
            raise RuntimeError("private attach peer closed during a framed message")
        received.extend(fragment)
    return bytes(received)


class AttachVersionMismatch(RuntimeError):
    def __init__(self, minor: int) -> None:
        super().__init__(f"daemon requires known private attach version 2.{minor}")
        self.minor = minor


def receive_attach_hello(
    peer: socket.socket, minor: int = ATTACH_PROTOCOL_MINOR
) -> None:
    header = receive_exact(peer, ATTACH_HEADER_BYTES)
    magic, major, received_minor, kind, flags, payload_bytes, sequence = struct.unpack(
        "!4sBBBBII", header
    )
    if (
        magic == ATTACH_MAGIC
        and major == ATTACH_PROTOCOL_MAJOR
        and received_minor in ATTACH_SUPPORTED_MINORS
        and received_minor != minor
        and kind == 7
        and flags == 0
        and sequence == 1
        and 1 <= payload_bytes <= 256
    ):
        payload = receive_exact(peer, payload_bytes)
        if (
            payload[0] == 3
        ):  # DisconnectReason::version_mismatch; never classify diagnostic prose.
            raise AttachVersionMismatch(received_minor)
    if (
        magic != ATTACH_MAGIC
        or (major, received_minor) != (ATTACH_PROTOCOL_MAJOR, minor)
        or kind != ATTACH_KIND_HELLO
        or flags != 0
        or payload_bytes != 4
        or sequence != 1
    ):
        raise RuntimeError("blocked client received an invalid daemon hello")
    receive_exact(peer, payload_bytes)


def connect_blocked_client(
    path: Path, hello_payload: bytes
) -> tuple[socket.socket, int]:
    minor = ATTACH_PROTOCOL_MINOR
    for attempt in range(2):
        peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024)
            peer.settimeout(5.0)
            peer.connect(str(path))
            peer.sendall(attach_frame(ATTACH_KIND_HELLO, hello_payload, 1, minor=minor))
            receive_attach_hello(peer, minor)
            return peer, minor
        except AttachVersionMismatch as error:
            peer.close()
            if attempt != 0:
                raise
            minor = error.minor
        except BaseException:
            peer.close()
            raise
    raise AssertionError("bounded attach negotiation exhausted")


def percentile(samples: list[int], quantile: float) -> int:
    ordered = sorted(samples)
    index = max(0, min(len(ordered) - 1, math.ceil(quantile * len(ordered)) - 1))
    return ordered[index]


def metric_summary(samples: list[int], unit: str) -> dict[str, Any]:
    return {
        f"samples_{unit}": samples,
        f"p50_{unit}": percentile(samples, 0.50),
        f"p95_{unit}": percentile(samples, 0.95),
        f"p99_{unit}": percentile(samples, 0.99),
    }


def summary(samples: list[int]) -> dict[str, Any]:
    result = metric_summary(samples, "ns")
    result["p95_valid"] = len(samples) >= 20
    result["p99_valid"] = len(samples) >= 100
    return result


def executable_provenance(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    digest = hashlib.sha256()
    with resolved.open("rb") as executable:
        while chunk := executable.read(1024 * 1024):
            digest.update(chunk)
    status = resolved.stat()
    return {
        "path": str(resolved),
        "sha256": digest.hexdigest(),
        "bytes": status.st_size,
        "mtime_ns": status.st_mtime_ns,
    }


def interaction_label_codes() -> dict[str, bytes]:
    labels = [
        "OUTPUT",
        "OPEN",
        "TUI",
        "WHEEL",
        "IDLE",
        "BLOCKED",
        "CLIENT_IDLE",
        "CLIENT_BLOCKED",
    ]
    for profile in load_manifest()["pane_profiles"]:
        labels.extend((f"{profile['id']}_IDLE", f"{profile['id']}_ACTIVE"))
    # Append-only: earlier label indices determine existing interaction tokens.
    labels.extend(("SESSION", "TAB", "SPLIT", "NEWTAB", "RESIZE"))
    if len(labels) > 26 * 26:
        raise RuntimeError("interaction labels exceed the native probe token space")
    return {
        label: bytes((ord("L"), ord("A") + index // 26, ord("A") + index % 26))
        for index, label in enumerate(labels)
    }


INTERACTION_LABEL_CODES = interaction_label_codes()


def interaction_visible_token(label: str, index: int) -> bytes:
    if not 0 <= index < 10_000 or label not in INTERACTION_LABEL_CODES:
        raise ValueError("interaction marker is outside its bounded token space")
    label_index = tuple(INTERACTION_LABEL_CODES).index(label)
    radix = 26
    token_space = radix**6
    redraw_multiplier = sum(radix**position for position in range(6))
    encoded_index = bytearray(6)
    remaining = ((label_index * 10_000 + index) * redraw_multiplier) % token_space
    for position in range(len(encoded_index) - 1, -1, -1):
        encoded_index[position] = ord("A") + (remaining % radix)
        remaining //= radix
    return bytes(encoded_index)


def interaction_marker(label: str, index: int) -> bytes:
    visible_token = interaction_visible_token(label, index)
    return f"__LEMMA_{label}_{index:04d}_".encode() + visible_token + b"__"


def account_login_shell() -> str:
    try:
        account_shell = pwd.getpwuid(os.getuid()).pw_shell
    except (KeyError, OSError):
        account_shell = "/bin/sh"
    if (
        not account_shell
        or not Path(account_shell).is_absolute()
        or not os.access(account_shell, os.X_OK)
    ):
        return "/bin/sh"
    return account_shell


def install_shell_startup(
    environment: dict[str, str], command: str, *, nushell_command: str | None = None
) -> None:
    shell = Path(environment["SHELL"]).name
    home = Path(environment["HOME"])
    config = Path(environment["XDG_CONFIG_HOME"])
    zdot = Path(environment["ZDOTDIR"])
    if shell in {"sh", "dash", "ksh", "mksh"}:
        interactive_startup = config / "lemma" / "shell-startup.sh"
        environment["ENV"] = str(interactive_startup)
        paths = (home / ".profile", interactive_startup)
    elif shell == "bash":
        paths = (home / ".bash_profile", home / ".bashrc")
    elif shell == "zsh":
        paths = (zdot / ".zprofile", zdot / ".zshrc")
    elif shell == "fish":
        paths = (config / "fish" / "config.fish",)
    elif shell in {"nu", "nushell"}:
        paths = (config / "nushell" / "config.nu",)
        command = nushell_command if nushell_command is not None else command
    else:
        raise RuntimeError(
            f"benchmark startup does not support account login shell {environment['SHELL']!r}"
        )
    for path in paths:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(command, encoding="utf-8")


def benchmark_environment(root: Path) -> dict[str, str]:
    for name in ("home", "config", "zdot", "data"):
        (root / name).mkdir(mode=0o700)
    account_shell = account_login_shell()
    environment = {
        "HOME": str(root / "home"),
        "XDG_CONFIG_HOME": str(root / "config"),
        "ZDOTDIR": str(root / "zdot"),
        "PATH": os.environ.get("PATH", os.defpath),
        "SHELL": account_shell,
        "TERM": "xterm-256color",
        "COLORTERM": "truecolor",
        "LANG": "C",
        "LC_ALL": "C",
        "TMPDIR": str(root),
    }
    install_shell_startup(
        environment, f"printf '{SHELL_READY_MARKER.decode('ascii')}\\n'\n"
    )
    return environment


def install_attach_shell_startup(environment: dict[str, str], peer: Path) -> None:
    command = f"exec {shlex.quote(str(peer))} attach-visible\n"
    install_shell_startup(
        environment,
        command,
        nushell_command=f"exec {json.dumps(str(peer))} attach-visible\n",
    )


def parse_cpu_time(value: str) -> int:
    days = 0
    clock = value
    if "-" in clock:
        day_text, clock = clock.split("-", 1)
        days = int(day_text)
    parts = clock.split(":")
    if len(parts) == 3:
        hours, minutes, seconds = int(parts[0]), int(parts[1]), float(parts[2])
    elif len(parts) == 2:
        hours, minutes, seconds = 0, int(parts[0]), float(parts[1])
    else:
        raise ValueError(f"invalid process CPU time: {value!r}")
    return int((((days * 24 + hours) * 60 + minutes) * 60 + seconds) * 1_000_000_000)


def darwin_resource_snapshot(pids: set[int]) -> dict[str, Any]:
    if platform.system() != "Darwin":
        return {"available": False, "reason": "not Darwin"}
    if not pids:
        return {"available": False, "reason": "no processes to sample"}

    class RusageInfoV0(ctypes.Structure):
        _fields_ = [
            ("uuid", ctypes.c_uint8 * 16),
            ("user_time", ctypes.c_uint64),
            ("system_time", ctypes.c_uint64),
            ("package_idle_wakeups", ctypes.c_uint64),
            ("interrupt_wakeups", ctypes.c_uint64),
            ("pageins", ctypes.c_uint64),
            ("wired_size", ctypes.c_uint64),
            ("resident_size", ctypes.c_uint64),
            ("physical_footprint", ctypes.c_uint64),
            ("process_start_time", ctypes.c_uint64),
            ("process_exit_time", ctypes.c_uint64),
            ("child_user_time", ctypes.c_uint64),
            ("child_system_time", ctypes.c_uint64),
            ("child_package_idle_wakeups", ctypes.c_uint64),
            ("child_interrupt_wakeups", ctypes.c_uint64),
            ("child_pageins", ctypes.c_uint64),
            ("child_elapsed_time", ctypes.c_uint64),
            ("disk_bytes_read", ctypes.c_uint64),
            ("disk_bytes_written", ctypes.c_uint64),
        ]

    try:
        libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
        proc_pid_rusage = libproc.proc_pid_rusage
        proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        proc_pid_rusage.restype = ctypes.c_int
        package_idle = 0
        interrupts = 0
        resident_bytes = 0
        physical_footprint_bytes = 0
        # rusage_info_v0 user/system fields are nanoseconds, not Mach absolute-time ticks.
        cpu_time_ns = 0
        sampled = 0
        for pid in pids:
            usage = RusageInfoV0()
            if proc_pid_rusage(pid, 0, ctypes.byref(usage)) != 0:
                continue
            package_idle += usage.package_idle_wakeups
            interrupts += usage.interrupt_wakeups
            cpu_time_ns += usage.user_time + usage.system_time
            resident_bytes += usage.resident_size
            physical_footprint_bytes += usage.physical_footprint
            sampled += 1
    except (AttributeError, OSError) as error:
        return {"available": False, "reason": str(error)}
    return {
        "available": sampled == len(pids),
        "source": "proc_pid_rusage RUSAGE_INFO_V0",
        "sampled_processes": sampled,
        "cpu_time_ns": cpu_time_ns,
        "resident_bytes": resident_bytes,
        "physical_footprint_bytes": physical_footprint_bytes,
        "package_idle": package_idle,
        "interrupt": interrupts,
        "total": package_idle + interrupts,
    }


def parse_linux_schedstat(value: str) -> int:
    fields = value.split()
    if len(fields) != 3 or any(
        not field.isascii() or not field.isdecimal() for field in fields
    ):
        raise ValueError("invalid /proc/PID/schedstat record")
    return int(fields[0])


def linux_cpu_snapshot(pids: set[int]) -> dict[str, Any]:
    if platform.system() != "Linux":
        return {"available": False, "reason": "not Linux"}
    if not pids:
        return {"available": False, "reason": "no processes to sample"}
    try:
        cpu_time_ns = 0
        sampled = 0
        for pid in pids:
            # schedstat is per-thread, unlike /proc/PID/stat's process CPU fields.
            # Include workers (notably Zellij's); these snapshots cover live threads only.
            for task in Path(f"/proc/{pid}/task").iterdir():
                cpu_time_ns += parse_linux_schedstat(
                    (task / "schedstat").read_text(encoding="ascii")
                )
            sampled += 1
    except (OSError, ValueError) as error:
        return {"available": False, "reason": str(error)}
    return {
        "available": sampled == len(pids),
        "source": "/proc/PID/task/TID/schedstat live-thread CPU runtime",
        "sampled_processes": sampled,
        "cpu_time_ns": cpu_time_ns,
    }


def parse_linux_context_switches(value: str) -> tuple[int, int]:
    counts: dict[str, int] = {}
    for line in value.splitlines():
        name, separator, count = line.partition(":")
        if name in ("voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"):
            count = count.strip()
            if not separator or not count.isascii() or not count.isdecimal():
                raise ValueError("invalid /proc/PID/task/TID/status context switches")
            counts[name] = int(count)
    if len(counts) != 2:
        raise ValueError("incomplete /proc/PID/task/TID/status context switches")
    return counts["voluntary_ctxt_switches"], counts["nonvoluntary_ctxt_switches"]


def linux_context_switch_snapshot(pids: set[int]) -> dict[str, Any]:
    """Sum per-thread context switches as a Linux wakeup proxy.

    Voluntary switches count blocking waits (each later needs a wakeup); nonvoluntary switches
    count preemption. Neither equals Darwin's package-idle or interrupt wakeups. Live-thread
    snapshots lose counts from threads that exit, so each snapshot identifies its exact thread set
    and intervals whose set changed are rejected rather than undercounted.
    """
    if platform.system() != "Linux":
        return {"available": False, "reason": "not Linux"}
    if not pids:
        return {"available": False, "reason": "no processes to sample"}
    try:
        voluntary = 0
        nonvoluntary = 0
        sampled = 0
        tasks: list[str] = []
        for pid in pids:
            for task in Path(f"/proc/{pid}/task").iterdir():
                try:
                    # The Name field is arbitrary thread-name bytes.
                    status = (task / "status").read_text(
                        encoding="utf-8", errors="replace"
                    )
                except (FileNotFoundError, ProcessLookupError):
                    continue  # The thread exited after enumeration; the task set records it.
                thread_voluntary, thread_nonvoluntary = parse_linux_context_switches(
                    status
                )
                voluntary += thread_voluntary
                nonvoluntary += thread_nonvoluntary
                tasks.append(f"{pid}/{task.name}")
            sampled += 1
    except (OSError, ValueError) as error:
        return {"available": False, "reason": str(error)}
    return {
        "available": sampled == len(pids),
        "source": "/proc/PID/task/TID/status live-thread context switches",
        "sampled_processes": sampled,
        "sampled_threads": len(tasks),
        "task_set_sha256": hashlib.sha256(
            "\n".join(sorted(tasks)).encode("ascii")
        ).hexdigest(),
        "voluntary": voluntary,
        "nonvoluntary": nonvoluntary,
        "total": voluntary + nonvoluntary,
    }


def linux_memory_snapshot(pids: set[int]) -> dict[str, Any]:
    if platform.system() != "Linux":
        return {"available": False, "reason": "not Linux"}
    if not pids:
        return {"available": False, "reason": "no processes to sample"}
    try:
        pss_bytes = 0
        private_bytes = 0
        sampled = 0
        for pid in pids:
            fields: dict[str, int] = {}
            for line in (
                Path(f"/proc/{pid}/smaps_rollup")
                .read_text(encoding="ascii")
                .splitlines()
            ):
                name, separator, value = line.partition(":")
                if not separator:
                    continue
                parts = value.split()
                if parts and parts[0].isdecimal():
                    fields[name] = int(parts[0]) * 1_024
            pss_bytes += fields["Pss"]
            private_bytes += fields.get("Private_Clean", 0) + fields.get(
                "Private_Dirty", 0
            )
            sampled += 1
    except (KeyError, OSError, ValueError) as error:
        return {"available": False, "reason": str(error)}
    return {
        "available": sampled == len(pids),
        "source": "/proc/PID/smaps_rollup",
        "sampled_processes": sampled,
        "physical_footprint_bytes": pss_bytes,
        "private_bytes": private_bytes,
    }


def open_descriptor_snapshot(pid: int) -> dict[str, Any]:
    if pid <= 0:
        return {"available": False, "reason": "invalid process"}
    if platform.system() == "Linux":
        try:
            return {
                "available": True,
                "source": "/proc/PID/fd",
                "count": len(list(Path(f"/proc/{pid}/fd").iterdir())),
            }
        except OSError as error:
            return {"available": False, "reason": str(error)}
    if platform.system() == "Darwin":
        # proc_fdinfo is two int32 values. A fixed 4,096-entry buffer is well above Lemma's
        # reviewed descriptor bounds and avoids a size-probe race.
        try:
            libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
            proc_pidinfo = libproc.proc_pidinfo
            proc_pidinfo.argtypes = [
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_uint64,
                ctypes.c_void_p,
                ctypes.c_int,
            ]
            proc_pidinfo.restype = ctypes.c_int
            entry_bytes = ctypes.sizeof(ctypes.c_int32) * 2
            capacity = 4_096
            storage = ctypes.create_string_buffer(entry_bytes * capacity)
            received = proc_pidinfo(pid, 1, 0, storage, len(storage))
            if received <= 0 or received % entry_bytes != 0:
                return {
                    "available": False,
                    "reason": "proc_pidinfo(PROC_PIDLISTFDS) failed",
                }
            if received == len(storage):
                return {
                    "available": False,
                    "reason": "descriptor census exceeded 4,096 entries",
                }
            return {
                "available": True,
                "source": "proc_pidinfo PROC_PIDLISTFDS",
                "count": received // entry_bytes,
            }
        except (AttributeError, OSError) as error:
            return {"available": False, "reason": str(error)}
    return {"available": False, "reason": "unsupported platform"}


def process_group_snapshot(
    pids: set[int], processes: dict[int, tuple[int, int, int]]
) -> dict[str, Any]:
    if not pids:
        return {"available": False, "reason": "no live processes in role"}
    darwin_resources = darwin_resource_snapshot(pids)
    linux_cpu = linux_cpu_snapshot(pids)
    linux_memory = linux_memory_snapshot(pids)
    native_cpu = (
        darwin_resources if darwin_resources.get("available") is True else linux_cpu
    )
    native_memory = (
        darwin_resources if darwin_resources.get("available") is True else linux_memory
    )
    wakeups = (
        {
            key: darwin_resources[key]
            for key in (
                "available",
                "source",
                "sampled_processes",
                "package_idle",
                "interrupt",
                "total",
            )
        }
        if darwin_resources.get("available") is True
        else {
            "available": False,
            "reason": "no reviewed per-process wakeup counter on this platform",
        }
    )
    context_switches = linux_context_switch_snapshot(pids)
    return {
        "available": True,
        "process_count": len(pids),
        "rss_bytes": sum(processes[pid][1] for pid in pids),
        "physical_footprint_bytes": (
            native_memory["physical_footprint_bytes"]
            if native_memory.get("available") is True
            else None
        ),
        "physical_footprint_source": (
            native_memory.get("source")
            if native_memory.get("available") is True
            else None
        ),
        "private_bytes": (
            native_memory.get("private_bytes")
            if native_memory.get("available") is True
            else None
        ),
        "cpu_time_ns": (
            native_cpu["cpu_time_ns"]
            if native_cpu.get("available") is True
            else sum(processes[pid][2] for pid in pids)
        ),
        "cpu_time_source": (
            native_cpu.get("source")
            if native_cpu.get("available") is True
            else "ps time"
        ),
        "wakeups": wakeups,
        "context_switches": context_switches,
        "pids": sorted(pids),
    }


def resource_snapshot(
    root_pids: list[int],
    role_pids: dict[str, list[int]] | None = None,
) -> dict[str, Any]:
    """Capture role and process-tree CPU, RSS, physical footprint, and wakeups."""
    try:
        output = subprocess.run(
            ["ps", "-axo", "pid=,ppid=,rss=,time="],
            check=True,
            capture_output=True,
            text=True,
            timeout=2.0,
        ).stdout
        processes: dict[int, tuple[int, int, int]] = {}
        for line in output.splitlines():
            fields = line.split()
            if len(fields) != 4:
                continue
            pid, parent, rss_kib = (int(fields[index]) for index in range(3))
            processes[pid] = (parent, rss_kib * 1_024, parse_cpu_time(fields[3]))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return {"available": False, "error": str(error)}

    roots = {pid for pid in root_pids if pid > 0}
    if not roots:
        return {"available": False, "error": "no valid root process set to sample"}
    missing_roots = roots.difference(processes)
    if missing_roots:
        return {
            "available": False,
            "error": "one or more root processes are no longer live",
            "missing_root_pids": sorted(missing_roots),
        }
    selected = set(roots)
    changed = True
    while changed:
        changed = False
        for pid, (parent, _, _) in processes.items():
            if pid not in selected and parent in selected:
                selected.add(pid)
                changed = True

    total = process_group_snapshot(selected, processes)
    if total.get("available") is not True:
        return total
    roles: dict[str, Any] = {}
    classified: set[int] = set()
    for role, configured in (role_pids or {}).items():
        direct = {pid for pid in configured if pid in selected}
        if role == "attached_client":
            # Client-owned restoration guardians are mux overhead, not pane children.
            # Some adapters launch their daemon from a client: stop at another role's
            # root rather than absorbing that daemon and its pane subtree twice.
            other_roots = {
                pid
                for other_role, roots in (role_pids or {}).items()
                if other_role != role
                for pid in roots
            }
            changed = True
            while changed:
                changed = False
                for pid in selected.difference(direct, other_roots):
                    if processes[pid][0] in direct:
                        direct.add(pid)
                        changed = True
        roles[role] = process_group_snapshot(direct, processes)
        classified.update(direct)
    unclassified = selected.difference(classified)
    if unclassified:
        roles["pane_or_mux_children"] = process_group_snapshot(unclassified, processes)

    return {
        **total,
        "scope": "multiplexer direct roles and complete descendant process tree",
        "roles": roles,
    }


def runtime_resource_snapshot(runtime: MuxRuntime) -> dict[str, Any]:
    return resource_snapshot(runtime.resource_roots(), runtime.resource_role_pids())


def workload_cpu(
    runtime: MuxRuntime, before: dict[str, Any], repetitions: int
) -> dict[str, Any]:
    """Batch CPU outside native latency timing; includes probe launch/settling, not setup.

    Live-thread snapshots cannot account for threads that exit within the batch. Keep the
    raw endpoints and reject a decreasing counter rather than fabricating zero CPU.
    """
    if repetitions <= 0:
        raise ValueError("workload CPU requires positive repetitions")
    after = runtime_resource_snapshot(runtime)
    result: dict[str, Any] = {
        "scope": "live process/thread CPU around native probe batch; excludes fixture setup",
        "repetitions": repetitions,
        "before": before,
        "after": after,
        "roles": {},
    }
    for role in before.get("roles", {}):
        start = before["roles"][role]
        end = after.get("roles", {}).get(role, {})
        if (
            start.get("available") is True
            and end.get("available") is True
            and start.get("pids") == end.get("pids")
            and start.get("cpu_time_source") not in (None, "ps time")
            and start.get("cpu_time_source") == end.get("cpu_time_source")
            and end["cpu_time_ns"] >= start["cpu_time_ns"]
        ):
            elapsed = end["cpu_time_ns"] - start["cpu_time_ns"]
            result["roles"][role] = {
                "available": True,
                "batch_cpu_ns": elapsed,
                "cpu_ns_per_operation": elapsed / repetitions,
            }
        else:
            result["roles"][role] = {"available": False}
    return result


def process_tree_diagnostic(root_pids: list[int]) -> str:
    try:
        output = subprocess.run(
            [
                "ps",
                "-e",
                "-o",
                "pid=,ppid=,pgid=,sid=,tpgid=,stat=,wchan:24=,cmd=",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=2.0,
        ).stdout
    except (OSError, subprocess.SubprocessError) as error:
        return f"process census failed: {error}"
    records: list[tuple[int, int, str]] = []
    for line in output.splitlines():
        fields = line.split(maxsplit=2)
        try:
            if len(fields) == 3:
                records.append((int(fields[0]), int(fields[1]), line))
        except ValueError:
            continue
    selected = {process for process in root_pids if process > 0}
    changed = True
    while changed:
        changed = False
        for process, parent, _ in records:
            if process not in selected and parent in selected:
                selected.add(process)
                changed = True
    return "\n".join(line for process, _, line in records if process in selected)


def summarize_resource_samples(
    before_samples: list[dict[str, Any]], after_samples: list[dict[str, Any]]
) -> dict[str, Any]:
    if len(before_samples) != len(after_samples):
        raise RuntimeError("resource sample endpoints are unbalanced")
    cpu_samples = [
        max(0, int(after["cpu_time_ns"]) - int(before["cpu_time_ns"]))
        for before, after in zip(before_samples, after_samples)
    ]
    rss_samples = [int(after["rss_bytes"]) for after in after_samples]
    footprint_samples = [
        int(after["physical_footprint_bytes"])
        for after in after_samples
        if after.get("physical_footprint_bytes") is not None
    ]
    footprint_available = len(footprint_samples) == len(after_samples)
    return {
        "cpu_time": summary(cpu_samples),
        "rss": metric_summary(rss_samples, "bytes"),
        "physical_footprint": {
            "available": footprint_available,
            "source": (
                after_samples[-1].get("physical_footprint_source")
                if footprint_available
                else None
            ),
            **(
                metric_summary(footprint_samples, "bytes")
                if footprint_available
                else {"samples_bytes": []}
            ),
        },
        "wakeups": counter_interval_summary(
            before_samples,
            after_samples,
            "wakeups",
            "no reviewed per-process wakeup counter on this platform",
        ),
        "context_switches": counter_interval_summary(
            before_samples,
            after_samples,
            "context_switches",
            "no reviewed per-thread context-switch counter on this platform",
        ),
    }


def counter_interval_summary(
    before_samples: list[dict[str, Any]],
    after_samples: list[dict[str, Any]],
    key: str,
    unavailable_reason: str,
) -> dict[str, Any]:
    """Difference cumulative counters, rejecting intervals that cannot be compared.

    A changed process or thread set loses the exited members' counts, and a decreasing total
    proves that happened. Either interval is rejected and counted rather than clamped to zero.
    """
    samples: list[int] = []
    source: str | None = None
    available = True
    rejected = {"changed_task_set": 0, "decreasing_counter": 0}
    for before, after in zip(before_samples, after_samples):
        start = before.get(key)
        end = after.get(key)
        if (
            not isinstance(start, dict)
            or not isinstance(end, dict)
            or start.get("available") is not True
            or end.get("available") is not True
        ):
            available = False
            continue
        source = str(end.get("source"))
        if before.get("pids") != after.get("pids") or start.get(
            "task_set_sha256"
        ) != end.get("task_set_sha256"):
            rejected["changed_task_set"] += 1
            continue
        elapsed = int(end["total"]) - int(start["total"])
        if elapsed < 0:
            rejected["decreasing_counter"] += 1
            continue
        samples.append(elapsed)
    if available and not samples:
        available = False
        unavailable_reason = "every interval changed its task set or counter"
    return {
        "available": available,
        "source": source if available else None,
        "reason": None if available else unavailable_reason,
        "rejected_samples": rejected,
        **(metric_summary(samples, "count") if available else {"samples_count": []}),
    }


def sample_resources(
    runtime: MuxRuntime, repetitions: int, client: PtyProcess | None = None
) -> dict[str, Any]:
    sample_seconds = 1.0
    before_samples: list[dict[str, Any]] = []
    after_samples: list[dict[str, Any]] = []
    outer_bytes: list[int] = []
    # Readiness proves the fixture is running, but terminal projection and process accounting may
    # still contain that final setup mutation. Exclude one complete interval before opening the
    # first measured resource window.
    if client is None:
        time.sleep(sample_seconds)
    else:
        client.drain(sample_seconds)
    for _ in range(repetitions):
        before = runtime_resource_snapshot(runtime)
        if client is None:
            time.sleep(sample_seconds)
        else:
            outer_bytes.append(client.drain(sample_seconds))
        after = runtime_resource_snapshot(runtime)
        if before.get("available") is not True or after.get("available") is not True:
            raise RuntimeError("resource snapshot is unavailable")
        before_samples.append(before)
        after_samples.append(after)

    role_names: set[str] = set()
    for snapshot in (*before_samples, *after_samples):
        roles = snapshot.get("roles")
        if isinstance(roles, dict):
            for role in roles:
                if not isinstance(role, str):
                    raise RuntimeError("resource snapshot has a non-string role name")
                role_names.add(role)
    role_results: dict[str, Any] = {}
    for role in sorted(role_names):
        role_before = [
            snapshot.get("roles", {}).get(role, {}) for snapshot in before_samples
        ]
        role_after = [
            snapshot.get("roles", {}).get(role, {}) for snapshot in after_samples
        ]
        if all(
            sample.get("available") is True for sample in (*role_before, *role_after)
        ):
            role_results[role] = {
                "available": True,
                **summarize_resource_samples(role_before, role_after),
            }
        else:
            reason = next(
                (
                    str(sample.get("reason"))
                    for sample in (*role_before, *role_after)
                    if sample.get("available") is not True and sample.get("reason")
                ),
                "role was unavailable during sampling",
            )
            role_results[role] = {"available": False, "reason": reason}

    sampled_output = (
        {
            "available": True,
            **metric_summary(outer_bytes, "bytes"),
        }
        if client is not None
        else {
            "available": False,
            "reason": "no attached outer client was sampled",
            "samples_bytes": [],
        }
    )
    sampled_throughput = (
        {
            "available": True,
            **metric_summary(
                [int(value / sample_seconds) for value in outer_bytes],
                "bytes_per_second",
            ),
        }
        if client is not None
        else {
            "available": False,
            "reason": "no attached outer client was sampled",
            "samples_bytes_per_second": [],
        }
    )
    return {
        "status": "completed",
        "sample_duration_ns": int(sample_seconds * 1_000_000_000),
        **summarize_resource_samples(before_samples, after_samples),
        "outer_bytes": sampled_output,
        "outer_throughput": sampled_throughput,
        "roles": role_results,
    }


def screen_renders_markers(runtime: MuxRuntime) -> bool:
    return runtime.multiplexer in {"zellij", "herdr"}


def wait_for_shell_execution(
    runtime: MuxRuntime,
    client: PtyProcess,
    marker: bytes,
    command: bytes,
) -> None:
    if marker in command:
        raise ValueError("shell readiness marker must not appear in echoed input")
    client.write_all(command, 2.0)
    client.read_until(marker, 5.0, visible_text=screen_renders_markers(runtime))
    client.drain(0.005)


def idle_resources(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    client = runtime.start_and_attach("idle_resources")
    command = f"exec {shlex.quote(str(runtime.peer_path))} idle\r".encode()
    client.write_all(command, 2.0)
    client.read_until(IDLE_READY, 5.0, visible_text=screen_renders_markers(runtime))
    client.drain(0.01)
    return sample_resources(runtime, repetitions, client)


class PtyReceiptChannel:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.peer_path = Path(f"{path}.peer")
        self.descriptor = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self.descriptor.bind(str(path))
        except BaseException:
            self.descriptor.close()
            raise
        self.descriptor.setblocking(False)

    def acknowledge_visible(self) -> None:
        sent = self.descriptor.sendto(LATENCY_VISIBLE_ACK, str(self.peer_path))
        if sent != len(LATENCY_VISIBLE_ACK):
            raise RuntimeError(
                "incomplete autonomous-output visibility acknowledgement"
            )

    def send_control(self, path: Path, message: bytes) -> None:
        if self.descriptor.sendto(message, str(path)) != len(message):
            raise RuntimeError(f"incomplete fixture control datagram to {path}")

    def wait_for_receipt(self, expected: bytes, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            readable, _, _ = select.select(
                [self.descriptor], [], [], min(remaining, 0.02)
            )
            if not readable:
                continue
            received = self.descriptor.recv(4 * 1024)
            if received != expected:
                raise RuntimeError(
                    f"unexpected PTY receipt: expected={expected!r} actual={received!r}"
                )
            return
        raise TimeoutError(f"did not receive PTY readiness receipt {expected!r}")

    def close(self) -> None:
        self.descriptor.close()
        for path in (self.path, self.peer_path):
            try:
                path.unlink()
            except FileNotFoundError:
                pass


def release_autonomous_output(receipts: PtyReceiptChannel) -> None:
    receipts.acknowledge_visible()
    receipts.wait_for_receipt(LATENCY_NEXT_READY, 5.0)


class MuxRuntime(Protocol):
    multiplexer: str
    version: str
    peer_path: Path
    probe_path: Path
    gate_path: Path
    receipt_path: Path
    clients: list[PtyProcess]
    environment: dict[str, str]

    def attach(self, session: str) -> PtyProcess: ...

    def attach_arguments(self, session: str) -> list[str]: ...

    def start_and_attach(self, session: str) -> PtyProcess: ...

    def start_detached(self, session: str) -> None: ...

    def start_detached_with_attach_marker(self, session: str) -> None: ...

    def detach(self, client: PtyProcess, session: str) -> None: ...

    def resource_roots(self) -> list[int]: ...

    def resource_role_pids(self) -> dict[str, list[int]]: ...

    def binary_provenance(self) -> dict[str, Any]: ...

    def close(self) -> None: ...


def wait_for_startup_shell(runtime: MuxRuntime, client: PtyProcess) -> None:
    # Alternate-screen setup only proves that the outer client initialized. The startup marker is
    # emitted by the inner shell, so observing it proves that workload input can be routed safely.
    client.read_until(
        SHELL_READY_MARKER,
        5.0,
        visible_text=screen_renders_markers(runtime),
    )
    client.drain(0.005)


def retain_attach_marker(runtime: MuxRuntime, session: str) -> None:
    validation_client = runtime.attach(session)
    validation_client.read_until(
        ATTACH_VISIBLE_MARKER,
        5.0,
        visible_text=screen_renders_markers(runtime),
    )
    runtime.detach(validation_client, session)


class LemmaRuntime:
    multiplexer = "lemma"
    version = "development"

    def __init__(
        self,
        server: Path,
        cli: Path,
        peer: Path,
        probe: Path,
        trace_directory: Path | None = None,
        extension_fixture_mode: str | None = None,
        extension_fixture_path: Path | None = None,
        extension_fixture_process_count: int = 0,
    ) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="lemma-benchmark-")
        root = Path(self.temporary.name)
        self.socket_path = root / "daemon.sock"
        self.gate_path = root / "blocked.gate"
        self.receipt_path = root / "receipt.sock"
        self.server_path = server.resolve()
        self.cli_path = cli.resolve()
        self.peer_path = peer.resolve()
        self.probe_path = probe.resolve()
        self.extension_fixture_mode = extension_fixture_mode
        self.extension_fixture_path = (
            extension_fixture_path.resolve()
            if extension_fixture_path is not None
            else None
        )
        self.extension_fixture_process_count = extension_fixture_process_count
        self.extension_processes: list[subprocess.Popen[str]] = []
        self.extension_logs: list[Path] = []
        self.extension_fixture_ready: list[dict[str, Any]] = []
        self.environment = benchmark_environment(root)
        if trace_directory is not None:
            self.environment["LEMMA_LATENCY_TRACE"] = str(trace_directory.resolve())
        self.server = subprocess.Popen(
            [str(self.server_path), str(self.socket_path)],
            env=self.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.clients: list[PtyProcess] = []
        try:
            self._wait_ready()
            # Capture daemon-owned helpers before any pane exists. Their lifetimes are fixed
            # in these isolated fixtures; do not mislabel the forked Lua host as pane memory.
            baseline = resource_snapshot([self.server.pid])
            if baseline.get("available") is not True:
                raise RuntimeError("cannot identify Lemma daemon helper processes")
            self.helper_pids = [
                pid for pid in baseline["pids"] if pid != self.server.pid
            ]
        except BaseException:
            self.close()
            raise

    def _wait_ready(self) -> None:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection.connect(str(self.socket_path))
                connection.close()
                return
            except OSError:
                time.sleep(0.005)
        raise TimeoutError("Lemma benchmark server did not become ready")

    def command(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        normalized = list(arguments)
        requested_session = (
            normalized.pop()
            if len(normalized) == 2 and normalized[0] == "list"
            else None
        )
        completed = subprocess.run(
            [str(self.cli_path), str(self.socket_path), *normalized],
            env=self.environment,
            check=True,
            capture_output=True,
            text=True,
            timeout=5.0,
        )
        if requested_session is not None:
            prefix = f'lemma session "{requested_session}":'
            completed.stdout = "".join(
                line
                for line in completed.stdout.splitlines(keepends=True)
                if line.startswith(prefix)
            )
            if not completed.stdout:
                raise RuntimeError(
                    f"Lemma session {requested_session!r} was not listed"
                )
        return completed

    def attach_arguments(self, session: str) -> list[str]:
        return [str(self.cli_path), str(self.socket_path), "attach", session]

    def attach(self, session: str) -> PtyProcess:
        client = PtyProcess(
            self.attach_arguments(session),
            self.environment,
            terminal_restore_sequence=(
                LEMMA_OUTER_TERMINAL_RESTORE,
                PRE_TITLE_LEMMA_OUTER_TERMINAL_RESTORE,
                LEGACY_LEMMA_OUTER_TERMINAL_RESTORE,
            ),
        )
        self.clients.append(client)
        client.read_until(ALT_SCREEN, 5.0, preserve_suffix=True)
        return client

    def start_and_attach(self, session: str) -> PtyProcess:
        self.start_detached(session)
        client = self.attach(session)
        wait_for_startup_shell(self, client)
        self._start_extension_fixture(session)
        return client

    def _start_extension_fixture(self, session: str) -> None:
        mode: str | None = getattr(self, "extension_fixture_mode", None)
        if mode is None or getattr(self, "extension_processes", []):
            return
        if self.extension_fixture_path is None:
            raise RuntimeError("extension fixture path is unavailable")
        default_counts = {
            "idle-peers": 8,
            "idle-surfaces": 8,
            "changing-rows": 4,
            "storm": 8,
            "blocked-reader": 1,
            "slow-producer": 8,
            "crash-focused": 1,
            "crash-docked": 1,
        }
        process_count = (
            self.extension_fixture_process_count
            if self.extension_fixture_process_count > 0
            else default_counts[mode]
        )
        for index in range(process_count):
            log_path = Path(self.temporary.name) / f"extension-fixture-{index}.log"
            self.extension_logs.append(log_path)
            log = log_path.open("w", encoding="utf-8")
            command = [
                sys.executable,
                str(self.extension_fixture_path),
                "--mode",
                mode,
                "--socket",
                str(self.socket_path),
                "--session",
                session,
                "--name-prefix",
                f"p{index}-",
            ]
            if process_count > 1:
                command.extend(["--peers", "1"])
            process = subprocess.Popen(
                command,
                env=self.environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=log,
                text=True,
                start_new_session=True,
            )
            log.close()
            self.extension_processes.append(process)
            assert process.stdout is not None
            readable, _, _ = select.select([process.stdout], [], [], 15.0)
            if not readable:
                raise TimeoutError("extension fixture did not become ready")
            line = process.stdout.readline()
            try:
                ready = json.loads(line)
            except json.JSONDecodeError as error:
                details = log_path.read_text(encoding="utf-8")
                raise RuntimeError(
                    f"invalid extension fixture readiness: {line!r}; stderr={details!r}"
                ) from error
            if ready.get("status") != "ready" or ready.get("mode") != mode:
                raise RuntimeError(f"extension fixture setup failed: {ready}")
            self.extension_fixture_ready.append(ready)

    def start_detached(self, session: str) -> None:
        self.command("start", session)

    def start_detached_with_attach_marker(self, session: str) -> None:
        install_attach_shell_startup(self.environment, self.peer_path)
        self.command("start", session)
        retain_attach_marker(self, session)

    def detach(self, client: PtyProcess, session: str) -> None:
        client.write_all(b"\x02d", 2.0)
        client.wait_for_exit(5.0)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if ", detached," in self.command("list", session).stdout:
                return
            time.sleep(0.005)
        raise TimeoutError("Lemma validation client did not detach")

    def live_extension_processes(self) -> list[subprocess.Popen[str]]:
        return [
            process for process in self.extension_processes if process.poll() is None
        ]

    def resource_roots(self) -> list[int]:
        return [
            self.server.pid,
            *(client.pid for client in self.clients if client.pid > 0),
            *(process.pid for process in self.live_extension_processes()),
        ]

    def resource_role_pids(self) -> dict[str, list[int]]:
        return {
            "daemon": [self.server.pid],
            "daemon_helpers": self.helper_pids,
            "attached_client": [
                client.pid for client in self.clients if client.pid > 0
            ],
            "extension_fixture": [
                process.pid for process in self.live_extension_processes()
            ],
        }

    def binary_provenance(self) -> dict[str, Any]:
        result = {
            "server": executable_provenance(self.server_path),
            "cli": executable_provenance(self.cli_path),
            "workload": executable_provenance(self.peer_path),
            "probe": executable_provenance(self.probe_path),
        }
        if (
            self.extension_fixture_mode is not None
            and self.extension_fixture_path is not None
        ):
            result["extension_fixture"] = executable_provenance(
                self.extension_fixture_path
            )
        return result

    def stop_extension_fixture(self, *, crash: bool = False) -> None:
        for process in self.live_extension_processes():
            try:
                os.killpg(process.pid, signal.SIGKILL if crash else signal.SIGTERM)
            except ProcessLookupError:
                pass
        for process in self.live_extension_processes():
            process.wait(timeout=2.0)

    def close(self) -> None:
        if self.live_extension_processes():
            try:
                self.stop_extension_fixture()
            except subprocess.TimeoutExpired:
                self.stop_extension_fixture(crash=True)
        for process in self.extension_processes:
            if process.stdout is not None:
                process.stdout.close()
        for client in self.clients:
            client.close()
        self.clients.clear()
        if self.server.poll() is None:
            try:
                os.killpg(self.server.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.server.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.server.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.server.wait(timeout=0.5)
        for _ in range(20):
            try:
                self.temporary.cleanup()
                return
            except OSError:
                time.sleep(0.01)
        shutil.rmtree(self.temporary.name, ignore_errors=True)


class DirectRuntime:
    multiplexer = "direct"
    version = "host PTY baseline"

    def __init__(self, peer: Path, probe: Path) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="direct-benchmark-")
        root = Path(self.temporary.name)
        self.peer_path = peer.resolve()
        self.probe_path = probe.resolve()
        self.gate_path = root / "blocked.gate"
        self.receipt_path = root / "receipt.sock"
        self.environment = benchmark_environment(root)
        self.environment["PS1"] = "__LEMMA_DIRECT_READY__ "
        self.clients: list[PtyProcess] = []

    def attach_arguments(self, session: str) -> list[str]:
        del session
        return ["/bin/sh", "-i"]

    def attach(self, session: str) -> PtyProcess:
        client = PtyProcess(self.attach_arguments(session), self.environment)
        self.clients.append(client)
        client.read_until(b"__LEMMA_DIRECT_READY__", 5.0)
        client.drain(0.01)
        return client

    def start_and_attach(self, session: str) -> PtyProcess:
        return self.attach(session)

    def start_detached(self, session: str) -> None:
        del session
        raise RuntimeError("the direct PTY baseline has no detached-session model")

    def start_detached_with_attach_marker(self, session: str) -> None:
        del session
        raise RuntimeError("the direct PTY baseline has no attach model")

    def detach(self, client: PtyProcess, session: str) -> None:
        del session
        client.close()

    def resource_roots(self) -> list[int]:
        return [client.pid for client in self.clients if client.pid > 0]

    def resource_role_pids(self) -> dict[str, list[int]]:
        return {
            "direct_shell": [client.pid for client in self.clients if client.pid > 0]
        }

    def binary_provenance(self) -> dict[str, Any]:
        return {
            "shell": executable_provenance(Path("/bin/sh")),
            "workload": executable_provenance(self.peer_path),
            "probe": executable_provenance(self.probe_path),
        }

    def close(self) -> None:
        for client in self.clients:
            client.close()
        self.clients.clear()
        self.temporary.cleanup()


class TmuxRuntime:
    multiplexer = "tmux"

    def __init__(
        self,
        executable: Path,
        peer: Path,
        probe: Path,
        trace_directory: Path | None = None,
    ) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="tmux-benchmark-")
        root = Path(self.temporary.name)
        self.executable_path = executable.resolve()
        self.peer_path = peer.resolve()
        self.probe_path = probe.resolve()
        self.socket_path = root / "tmux.sock"
        self.gate_path = root / "blocked.gate"
        self.receipt_path = root / "receipt.sock"
        self.environment = benchmark_environment(root)
        if trace_directory is not None:
            self.environment["LEMMA_LATENCY_TRACE"] = str(trace_directory.resolve())
        self.clients: list[PtyProcess] = []
        self.server_pid = -1
        self.version = subprocess.run(
            [str(self.executable_path), "-V"],
            check=True,
            capture_output=True,
            text=True,
            timeout=2.0,
        ).stdout.strip()

    def _command(
        self, *arguments: str, check: bool = True
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                str(self.executable_path),
                "-S",
                str(self.socket_path),
                "-f",
                "/dev/null",
                *arguments,
            ],
            env=self.environment,
            check=check,
            capture_output=True,
            text=True,
            timeout=5.0,
        )

    def attach_arguments(self, session: str) -> list[str]:
        return [
            str(self.executable_path),
            "-S",
            str(self.socket_path),
            "-f",
            "/dev/null",
            "attach-session",
            "-t",
            session,
        ]

    def attach(self, session: str) -> PtyProcess:
        client = PtyProcess(self.attach_arguments(session), self.environment)
        self.clients.append(client)
        client.read_until(ALT_SCREEN, 5.0, preserve_suffix=True)
        return client

    def start_and_attach(self, session: str) -> PtyProcess:
        self.start_detached(session)
        client = self.attach(session)
        wait_for_startup_shell(self, client)
        return client

    def start_detached(self, session: str) -> None:
        self._command("new-session", "-d", "-s", session, "-x", "80", "-y", "24")
        if self.server_pid < 0:
            self.server_pid = int(
                self._command("display-message", "-p", "#{pid}").stdout.strip()
            )

    def start_detached_with_attach_marker(self, session: str) -> None:
        install_attach_shell_startup(self.environment, self.peer_path)
        self._command("new-session", "-d", "-s", session, "-x", "80", "-y", "24")
        if self.server_pid < 0:
            self.server_pid = int(
                self._command("display-message", "-p", "#{pid}").stdout.strip()
            )
        retain_attach_marker(self, session)

    def detach(self, client: PtyProcess, session: str) -> None:
        client.write_all(b"\x02d", 2.0)
        client.wait_for_exit(5.0)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            attached = self._command(
                "display-message", "-p", "-t", session, "#{session_attached}"
            ).stdout.strip()
            if attached == "0":
                return
            time.sleep(0.005)
        raise TimeoutError("tmux validation client did not detach")

    def resource_roots(self) -> list[int]:
        return [
            self.server_pid,
            *(client.pid for client in self.clients if client.pid > 0),
        ]

    def resource_role_pids(self) -> dict[str, list[int]]:
        return {
            "daemon": [self.server_pid],
            "attached_client": [
                client.pid for client in self.clients if client.pid > 0
            ],
        }

    def binary_provenance(self) -> dict[str, Any]:
        return {
            "multiplexer": executable_provenance(self.executable_path),
            "workload": executable_provenance(self.peer_path),
            "probe": executable_provenance(self.probe_path),
        }

    def close(self) -> None:
        for client in self.clients:
            client.close()
        self.clients.clear()
        if self.server_pid > 0:
            try:
                self._command("kill-server", check=False)
            except subprocess.SubprocessError:
                pass
        for _ in range(20):
            try:
                self.temporary.cleanup()
                return
            except OSError:
                time.sleep(0.01)
        shutil.rmtree(self.temporary.name, ignore_errors=True)


class ZellijRuntime:
    multiplexer = "zellij"

    def __init__(
        self,
        executable: Path,
        peer: Path,
        probe: Path,
        trace_directory: Path | None = None,
    ) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="zellij-benchmark-")
        root = Path(self.temporary.name)
        self.executable_path = executable.resolve()
        self.peer_path = peer.resolve()
        self.probe_path = probe.resolve()
        self.gate_path = root / "blocked.gate"
        self.receipt_path = root / "receipt.sock"
        self.environment = benchmark_environment(root)
        if trace_directory is not None:
            self.environment["LEMMA_LATENCY_TRACE"] = str(trace_directory.resolve())
        self.socket_directory = Path(tempfile.mkdtemp(prefix="lz-", dir="/tmp"))
        self.environment["ZELLIJ_SOCKET_DIR"] = str(self.socket_directory)
        # Zellij's default_shell takes no arguments and spawns a non-login shell. Match the
        # other subjects' login startup with a wrapper that replaces itself with the shell.
        self.login_shell_path = root / "login-shell"
        self.login_shell_path.write_text(
            f"#!/bin/sh\nexec {shlex.quote(self.environment['SHELL'])} -l\n",
            encoding="utf-8",
        )
        self.login_shell_path.chmod(0o700)
        self.config_path = root / "config.kdl"
        self.config_path.write_text(
            "show_startup_tips false\n"
            "show_release_notes false\n"
            "session_serialization false\n"
            "serialize_pane_viewport false\n"
            "disable_session_metadata true\n"
            f"default_shell {json.dumps(str(self.login_shell_path))}\n"
            'keybinds {\n    normal {\n        bind "Alt t" { NewTab; }\n    }\n}\n',
            encoding="utf-8",
        )
        self.session_prefix = f"lb-{os.getpid()}-"
        self.pane_listing_error: str | None = None
        self.sessions: list[str] = []
        self.clients: list[PtyProcess] = []
        self.version = subprocess.run(
            [str(self.executable_path), "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=2.0,
        ).stdout.strip()

    def _arguments(self, *arguments: str) -> list[str]:
        return [
            str(self.executable_path),
            "--config",
            str(self.config_path),
            *arguments,
        ]

    def _command(
        self, *arguments: str, check: bool = True, timeout: float = 5.0
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            self._arguments(*arguments),
            env=self.environment,
            check=check,
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def attach_arguments(self, session: str) -> list[str]:
        mapped_session = self.session_prefix + session.replace("_", "-")
        return self._arguments("attach", mapped_session)

    def attach(self, session: str) -> PtyProcess:
        client = PtyProcess(self.attach_arguments(session), self.environment)
        self.clients.append(client)
        client.read_until(ALT_SCREEN, 5.0, preserve_suffix=True)
        return client

    def _create_and_attach(self, session: str) -> PtyProcess:
        # Creating and attaching through one client is the only Zellij operation that owns both
        # sides of the startup lifetime. A background session can be published by list-sessions and
        # still disappear before a second attach process reaches it.
        mapped_session = self.session_prefix + session.replace("_", "-")
        self.sessions.append(mapped_session)
        client = PtyProcess(
            self._arguments("attach", "--create", mapped_session), self.environment
        )
        self.clients.append(client)
        client.read_until(ALT_SCREEN, 5.0, preserve_suffix=True)
        return client

    def start_and_attach(self, session: str) -> PtyProcess:
        client = self._create_and_attach(session)
        wait_for_startup_shell(self, client)
        return client

    def session_command(
        self, session: str, *arguments: str, timeout: float = 5.0
    ) -> subprocess.CompletedProcess[str]:
        mapped = self.session_prefix + session.replace("_", "-")
        return self._command("--session", mapped, "action", *arguments, timeout=timeout)

    def pane_count(self, session: str) -> int:
        listed = self._command(
            "--session",
            self.session_prefix + session.replace("_", "-"),
            "action",
            "list-panes",
            "--json",
            check=False,
            timeout=ZELLIJ_PANE_LISTING_SECONDS,
        )
        try:
            panes = json.loads(listed.stdout) if listed.returncode == 0 else None
        except json.JSONDecodeError:
            panes = None
        if not isinstance(panes, list):
            # While tabs are created or torn down, Zellij can reject the listing or publish an
            # empty or non-list result. Treat it as not yet observable; callers' deadlines bound it.
            self.pane_listing_error = (
                f"list-panes exit {listed.returncode}: "
                f"stderr={listed.stderr.strip()[-512:]!r} stdout={listed.stdout[:128]!r}"
            )
            return -1
        self.pane_listing_error = None
        return sum(not pane["is_plugin"] for pane in panes)

    def _wait_for_session(self, mapped_session: str) -> None:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            listed = self._command("list-sessions", "--short", check=False)
            if listed.returncode == 0 and mapped_session in listed.stdout.splitlines():
                return
            time.sleep(0.005)
        raise TimeoutError(f"Zellij session {mapped_session!r} was not published")

    def start_detached(self, session: str) -> None:
        mapped_session = self.session_prefix + session.replace("_", "-")
        self._command("attach", "--create-background", mapped_session)
        self.sessions.append(mapped_session)
        self._wait_for_session(mapped_session)

    def start_detached_with_attach_marker(self, session: str) -> None:
        install_attach_shell_startup(self.environment, self.peer_path)
        client = self._create_and_attach(session)
        client.read_until(ATTACH_VISIBLE_MARKER, 5.0, visible_text=True)
        self.detach(client, session)

    def detach(self, client: PtyProcess, session: str) -> None:
        del session
        # Mode changes round-trip through Zellij's server. Sending both keys in
        # one write can interpret 'd' in the old mode and leave the client attached.
        client.write_all(b"\x0f", 2.0)
        client.read_until(b"SESSION", 5.0, visible_text=True)
        client.write_all(b"d", 2.0)
        client.wait_for_exit(5.0)

    def _server_pids(self) -> list[int]:
        try:
            output = subprocess.run(
                ["ps", "-axo", "pid=,command="],
                check=True,
                capture_output=True,
                text=True,
                timeout=2.0,
            ).stdout
        except (OSError, subprocess.SubprocessError):
            return []
        return [
            int(line.split(maxsplit=1)[0])
            for line in output.splitlines()
            if "--server" in line and any(session in line for session in self.sessions)
        ]

    def resource_roots(self) -> list[int]:
        return [
            *self._server_pids(),
            *(client.pid for client in self.clients if client.pid > 0),
        ]

    def resource_role_pids(self) -> dict[str, list[int]]:
        return {
            "daemon": self._server_pids(),
            "attached_client": [
                client.pid for client in self.clients if client.pid > 0
            ],
        }

    def binary_provenance(self) -> dict[str, Any]:
        return {
            "multiplexer": executable_provenance(self.executable_path),
            "workload": executable_provenance(self.peer_path),
            "probe": executable_provenance(self.probe_path),
        }

    def close(self) -> None:
        for client in self.clients:
            client.close()
        self.clients.clear()
        server_pids = self._server_pids()
        for session in self.sessions:
            try:
                self._command("kill-session", session, check=False)
            except subprocess.SubprocessError:
                pass
        self.sessions.clear()
        deadline = time.monotonic() + 0.5
        remaining = set(server_pids)
        while remaining and time.monotonic() < deadline:
            for pid in tuple(remaining):
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    remaining.remove(pid)
            if remaining:
                time.sleep(0.01)
        for pid in remaining:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 0.5
        while remaining and time.monotonic() < deadline:
            for pid in tuple(remaining):
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    remaining.remove(pid)
            if remaining:
                time.sleep(0.01)
        for pid in remaining:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        shutil.rmtree(self.socket_directory, ignore_errors=True)
        self.temporary.cleanup()


class HerdrRuntime:
    multiplexer = "herdr"

    def __init__(
        self,
        executable: Path,
        peer: Path,
        probe: Path,
        trace_directory: Path | None = None,
    ) -> None:
        # Herdr derives per-session client sockets below the config directory. Keep the
        # isolated root short enough for Darwin's bounded sockaddr_un path.
        self.temporary = tempfile.TemporaryDirectory(prefix="hb-", dir="/tmp")
        root = Path(self.temporary.name)
        self.executable_path = executable.resolve()
        self.peer_path = peer.resolve()
        self.probe_path = probe.resolve()
        self.gate_path = root / "blocked.gate"
        self.receipt_path = root / "receipt.sock"
        self.environment = benchmark_environment(root)
        self.environment["XDG_STATE_HOME"] = str(root / "state")
        if trace_directory is not None:
            self.environment["LEMMA_LATENCY_TRACE"] = str(trace_directory.resolve())
        self.config_directory = Path(self.environment["XDG_CONFIG_HOME"]) / "herdr"
        self.config_path = self.config_directory / "config.toml"
        self.config_directory.mkdir(parents=True, exist_ok=True)
        self.config_path.write_text(HERDR_BENCHMARK_CONFIG, encoding="utf-8")
        self.environment["HERDR_CONFIG_PATH"] = str(self.config_path)
        self.sessions: set[str] = set()
        self.server_pids: dict[str, int] = {}
        self.owned_servers: list[subprocess.Popen[bytes]] = []
        self.clients: list[PtyProcess] = []
        self.version = subprocess.run(
            [str(self.executable_path), "--version"],
            env=self.environment,
            check=True,
            capture_output=True,
            text=True,
            timeout=2.0,
        ).stdout.strip()

    def _command(
        self, *arguments: str, check: bool = True, timeout: float = 5.0
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.executable_path), *arguments],
            env=self.environment,
            check=check,
            capture_output=True,
            text=True,
            timeout=timeout,
        )

    def session_command(
        self, session: str, *arguments: str
    ) -> subprocess.CompletedProcess[str]:
        return self._command("--session", session, *arguments)

    def _remember_server(self, session: str) -> None:
        socket_path = self.config_directory / "sessions" / session / "herdr.sock"
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                self.server_pids[session] = local_socket_peer_pid(socket_path)
                return
            except OSError:
                time.sleep(0.005)
        raise TimeoutError(f"Herdr session {session!r} API socket did not become ready")

    def attach_arguments(self, session: str) -> list[str]:
        return [str(self.executable_path), "--session", session]

    def attach(self, session: str) -> PtyProcess:
        self.sessions.add(session)
        client = PtyProcess(self.attach_arguments(session), self.environment)
        self.clients.append(client)
        client.read_until(ALT_SCREEN, 10.0, preserve_suffix=True)
        self._remember_server(session)
        return client

    def start_and_attach(self, session: str) -> PtyProcess:
        client = self.attach(session)
        wait_for_startup_shell(self, client)
        return client

    def start_detached(self, session: str) -> None:
        self.sessions.add(session)
        server = subprocess.Popen(
            [str(self.executable_path), "--session", session, "server"],
            env=self.environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        self.owned_servers.append(server)
        self._remember_server(session)
        self.session_command(session, "workspace", "create", "--cwd", os.getcwd())
        if self.pane_count(session) != 1:
            raise RuntimeError("Herdr detached session did not create one root pane")

    def start_detached_with_attach_marker(self, session: str) -> None:
        install_attach_shell_startup(self.environment, self.peer_path)
        validation_client = self.attach(session)
        validation_client.read_until(ATTACH_VISIBLE_MARKER, 5.0, visible_text=True)
        self.detach(validation_client, session)

    def detach(self, client: PtyProcess, session: str) -> None:
        del session
        client.write_all(b"\x02q", 2.0)
        client.wait_for_exit(10.0)

    def pane_count(self, session: str) -> int:
        response = self.session_command(session, "api", "snapshot")
        try:
            snapshot = json.loads(response.stdout)["result"]["snapshot"]
            return len(snapshot["panes"])
        except (KeyError, TypeError, json.JSONDecodeError) as error:
            raise RuntimeError("Herdr snapshot did not contain a pane list") from error

    def resource_roots(self) -> list[int]:
        return [
            *self.server_pids.values(),
            *(client.pid for client in self.clients if client.pid > 0),
        ]

    def resource_role_pids(self) -> dict[str, list[int]]:
        return {
            "daemon": list(self.server_pids.values()),
            "attached_client": [
                client.pid for client in self.clients if client.pid > 0
            ],
        }

    def binary_provenance(self) -> dict[str, Any]:
        return {
            "multiplexer": executable_provenance(self.executable_path),
            "workload": executable_provenance(self.peer_path),
            "probe": executable_provenance(self.probe_path),
        }

    def close(self) -> None:
        # On Darwin the detached server may remain a child of its launching client. Stop
        # servers first so forcibly closing an attached client cannot wait on that child.
        remaining_servers = set(self.server_pids.values())
        for session in self.sessions:
            try:
                stopped = self._command(
                    "session", "stop", session, "--json", check=False, timeout=20.0
                )
                if stopped.returncode == 0:
                    remaining_servers.discard(self.server_pids.get(session, -1))
            except subprocess.SubprocessError:
                pass
        for process in remaining_servers:
            try:
                os.kill(process, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for client in self.clients:
            client.close()
        for server in self.owned_servers:
            try:
                server.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=0.5)
        self.clients.clear()
        self.owned_servers.clear()
        self.sessions.clear()
        self.server_pids.clear()
        self.temporary.cleanup()


def warm_scroll(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    client = runtime.start_and_attach("warm_scroll")
    fixture_command = (
        f"{shlex.quote(str(runtime.peer_path))} warm-scroll-loop\r"
    ).encode()
    client.write_all(fixture_command, 2.0)
    client.read_until(
        WARM_READY_MARKER, 60.0, visible_text=screen_renders_markers(runtime)
    )
    client.drain()
    cpu_before = runtime_resource_snapshot(runtime)

    try:
        completed = subprocess.run(
            [
                str(runtime.probe_path),
                "command",
                str(client.descriptor),
                str(repetitions),
                "\r",
                WARM_MARKER.decode("ascii"),
            ],
            check=True,
            capture_output=True,
            text=True,
            pass_fds=(client.descriptor,),
            timeout=max(60.0, float(repetitions) * 60.0),
        )
    except subprocess.CalledProcessError as error:
        census = process_tree_diagnostic(runtime.resource_roots())
        raise RuntimeError(
            "native command probe failed: "
            f"stdout={error.stdout!r} stderr={error.stderr!r} processes={census!r}"
        ) from error
    try:
        measured = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("native command probe returned invalid JSON") from error
    latencies = measured.get("latency")
    outer_bytes = measured.get("outer_bytes")
    if (
        not isinstance(measured, dict)
        or measured.get("observer") != "native_poll"
        or not isinstance(latencies, dict)
        or not isinstance(outer_bytes, list)
        or len(outer_bytes) != repetitions
    ):
        raise RuntimeError("native command probe returned an invalid result")
    return {
        "status": "completed",
        "observer": measured["observer"],
        "clock": measured["clock"],
        **latencies,
        "outer_bytes": outer_bytes,
        "median_outer_bytes": percentile(outer_bytes, 0.50),
        "workload_cpu": workload_cpu(runtime, cpu_before, repetitions),
    }


def attach_to_visible(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    if repetitions != 1:
        raise ValueError("attach-to-visible isolates one runtime per native sample")
    # The startup fixture remains foreground. An untimed validation attach observes its marker
    # and detaches cleanly first, proving canonical state is ready before the native probe forks.
    session = "attach_visible"
    runtime.start_detached_with_attach_marker(session)
    cpu_before = runtime_resource_snapshot(runtime)
    completed = subprocess.run(
        [
            str(runtime.probe_path),
            "attach",
            "1",
            ATTACH_VISIBLE_MARKER.decode("ascii"),
            "--",
            *runtime.attach_arguments(session),
        ],
        env=runtime.environment,
        check=True,
        capture_output=True,
        text=True,
        timeout=10.0,
    )
    try:
        measured = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("native attach probe returned invalid JSON") from error
    latencies = measured.get("latency")
    outer_bytes = measured.get("outer_bytes")
    if (
        not isinstance(measured, dict)
        or measured.get("observer") != "native_poll"
        or not isinstance(latencies, dict)
        or not isinstance(outer_bytes, list)
        or len(outer_bytes) != 1
    ):
        raise RuntimeError("native attach probe returned an invalid result")
    return {
        "status": "completed",
        "observer": measured["observer"],
        "clock": measured["clock"],
        **latencies,
        "outer_bytes": outer_bytes,
        "median_outer_bytes": percentile(outer_bytes, 0.50),
        # The native probe owns the short-lived client; live-process snapshots cannot count it.
        "workload_cpu": {
            **workload_cpu(runtime, cpu_before, repetitions),
            "excluded": "short-lived attached client owned by native probe",
        },
    }


def latency_samples(
    client: PtyProcess,
    receipts: PtyReceiptChannel,
    probe: Path,
    label: str,
    repetitions: int,
    *,
    wait_for_peer_ready: bool = False,
    runtime: MuxRuntime | None = None,
) -> dict[str, Any]:
    cpu_before = runtime_resource_snapshot(runtime) if runtime is not None else None
    label_code = INTERACTION_LABEL_CODES.get(label)
    if label_code is None:
        raise ValueError(f"unknown interaction label: {label}")
    completed = subprocess.run(
        [
            str(probe),
            "latency",
            str(client.descriptor),
            str(receipts.descriptor.fileno()),
            str(receipts.peer_path),
            label,
            label_code.decode("ascii"),
            str(repetitions),
            "1" if wait_for_peer_ready else "0",
        ],
        check=True,
        capture_output=True,
        text=True,
        pass_fds=(client.descriptor, receipts.descriptor.fileno()),
        timeout=max(10.0, float(repetitions) * 6.0),
    )
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("native latency probe returned invalid JSON") from error
    if (
        not isinstance(result, dict)
        or result.get("schema") != 1
        or result.get("observer") != "native_poll"
    ):
        raise RuntimeError("native latency probe returned an invalid result")
    outer_bytes = result.get("outer_bytes")
    if not isinstance(outer_bytes, list) or len(outer_bytes) != repetitions:
        raise RuntimeError("native latency probe returned an invalid byte distribution")
    return {
        **(
            {"workload_cpu": workload_cpu(runtime, cpu_before, repetitions)}
            if runtime is not None and cpu_before is not None
            else {}
        ),
        "observer": result["observer"],
        "clock": result["clock"],
        "key_to_pty": result["key_to_pty"],
        "key_to_outer_bytes": result["key_to_outer_bytes"],
        "outer_bytes": outer_bytes,
        "median_outer_bytes": percentile(outer_bytes, 0.50),
    }


def interactive_under_output(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    receipts = PtyReceiptChannel(runtime.receipt_path)
    try:
        client = runtime.start_and_attach("interactive_output")
        launch = (
            f"exec {shlex.quote(str(runtime.peer_path))} latency-output "
            f"{shlex.quote(str(runtime.receipt_path))}\r"
        ).encode()
        client.write_all(launch, 2.0)
        client.read_until(
            LATENCY_OUTPUT_READY,
            5.0,
            visible_text=screen_renders_markers(runtime),
        )
        release_autonomous_output(receipts)
        client.drain(0.06)
        return {
            "status": "completed",
            **latency_samples(
                client,
                receipts,
                runtime.probe_path,
                "OUTPUT",
                repetitions,
                wait_for_peer_ready=True,
                runtime=runtime,
            ),
        }
    finally:
        receipts.close()


def interactive_open_loop(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    receipts = PtyReceiptChannel(runtime.receipt_path)
    try:
        client = runtime.start_and_attach("interactive_open_loop")
        launch = (
            f"exec {shlex.quote(str(runtime.peer_path))} latency "
            f"{shlex.quote(str(runtime.receipt_path))}\r"
        ).encode()
        client.write_all(launch, 2.0)
        client.read_until(
            LATENCY_READY, 5.0, visible_text=screen_renders_markers(runtime)
        )
        client.drain(0.01)
        cpu_before = runtime_resource_snapshot(runtime)
        try:
            completed = subprocess.run(
                [
                    str(runtime.probe_path),
                    "open-loop",
                    str(client.descriptor),
                    str(receipts.descriptor.fileno()),
                    "OPEN",
                    INTERACTION_LABEL_CODES["OPEN"].decode("ascii"),
                    str(repetitions),
                    "8333",
                    "bounded",
                ],
                check=True,
                capture_output=True,
                text=True,
                pass_fds=(client.descriptor, receipts.descriptor.fileno()),
                timeout=max(30.0, float(repetitions) / 60.0 + 10.0),
            )
        except subprocess.CalledProcessError as error:
            raise RuntimeError(
                f"native open-loop probe failed: stderr={error.stderr!r}"
            ) from error
        try:
            measured = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError(
                "native open-loop probe returned invalid JSON"
            ) from error
        if (
            not isinstance(measured, dict)
            or measured.get("observer") != "native_poll"
            or measured.get("offered_interval_ns") != 8_333_000
        ):
            raise RuntimeError("native open-loop probe returned an invalid result")
        return {
            "status": "completed",
            "observer": measured["observer"],
            "clock": measured["clock"],
            "workload_cpu": workload_cpu(runtime, cpu_before, repetitions),
            "offered_rate_hz": 120,
            "key_to_pty": measured["key_to_pty"],
            "key_to_outer_bytes": measured["key_to_outer_bytes"],
            "schedule_lateness": measured["schedule_lateness"],
        }
    finally:
        receipts.close()


def tui_redraw(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    receipts = PtyReceiptChannel(runtime.receipt_path)
    try:
        client = runtime.start_and_attach("tui_redraw")
        launch = (
            f"exec {shlex.quote(str(runtime.peer_path))} latency-tui "
            f"{shlex.quote(str(runtime.receipt_path))}\r"
        ).encode()
        client.write_all(launch, 2.0)
        client.read_until(
            TUI_REDRAW_READY,
            5.0,
            visible_text=screen_renders_markers(runtime),
        )
        client.drain(0.01)
        return {
            "status": "completed",
            **latency_samples(
                client,
                receipts,
                runtime.probe_path,
                "TUI",
                repetitions,
                runtime=runtime,
            ),
        }
    finally:
        receipts.close()


def tui_wheel_burst(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    receipts = PtyReceiptChannel(runtime.receipt_path)
    try:
        client = runtime.start_and_attach("tui_wheel")
        launch = (
            f"exec {shlex.quote(str(runtime.peer_path))} latency-tui-wheel "
            f"{shlex.quote(str(runtime.receipt_path))} {TUI_WHEEL_BURST_SIZE}\r"
        ).encode()
        client.write_all(launch, 2.0)
        client.read_until(
            TUI_WHEEL_READY,
            5.0,
            visible_text=screen_renders_markers(runtime),
        )
        client.drain(0.01)
        cpu_before = runtime_resource_snapshot(runtime)

        completed = subprocess.run(
            [
                str(runtime.probe_path),
                "wheel",
                str(client.descriptor),
                str(receipts.descriptor.fileno()),
                "WHEEL",
                INTERACTION_LABEL_CODES["WHEEL"].decode("ascii"),
                str(repetitions),
                str(TUI_WHEEL_BURST_SIZE),
                "bounded",
            ],
            check=True,
            capture_output=True,
            text=True,
            pass_fds=(client.descriptor, receipts.descriptor.fileno()),
            timeout=max(10.0, float(repetitions) * 6.0),
        )
        try:
            measured = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError("native wheel probe returned invalid JSON") from error
        outer_bytes = measured.get("outer_bytes")
        if (
            not isinstance(measured, dict)
            or measured.get("observer") != "native_poll"
            or not isinstance(outer_bytes, list)
            or len(outer_bytes) != repetitions
        ):
            raise RuntimeError("native wheel probe returned an invalid result")
        return {
            "status": "completed",
            "observer": measured["observer"],
            "clock": measured["clock"],
            "workload_cpu": workload_cpu(runtime, cpu_before, repetitions),
            "wheel_events_per_sample": TUI_WHEEL_BURST_SIZE,
            "key_to_pty": measured["key_to_pty"],
            "key_to_outer_bytes": measured["key_to_outer_bytes"],
            "outer_bytes": outer_bytes,
            "median_outer_bytes": percentile(outer_bytes, 0.50),
        }
    finally:
        receipts.close()


def trigger_sample(
    runtime: MuxRuntime, client: PtyProcess, trigger: bytes, token: bytes
) -> tuple[int, int]:
    """Run one native trigger-to-visible sample; the caller prepares an unseen unique token."""
    try:
        completed = subprocess.run(
            [
                str(runtime.probe_path),
                "trigger",
                str(client.descriptor),
                trigger.hex(),
                token.decode("ascii"),
            ],
            check=True,
            capture_output=True,
            text=True,
            pass_fds=(client.descriptor,),
            timeout=10.0,
        )
    except subprocess.CalledProcessError as error:
        raise RuntimeError(
            f"native trigger probe failed: stdout={error.stdout!r} stderr={error.stderr!r}"
        ) from error
    try:
        measured = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("native trigger probe returned invalid JSON") from error
    if not isinstance(measured, dict):
        raise RuntimeError("native trigger probe returned an invalid result")
    latency = measured.get("latency_ns")
    outer_bytes = measured.get("outer_bytes")
    if (
        measured.get("observer") != "native_poll"
        or not isinstance(latency, int)
        or latency <= 0
        or not isinstance(outer_bytes, int)
    ):
        raise RuntimeError("native trigger probe returned an invalid result")
    return latency, outer_bytes


def trigger_result(
    latencies: list[int],
    outer_bytes: list[int],
    runtime: MuxRuntime,
    cpu_before: dict[str, Any] | None,
    **metadata: Any,
) -> dict[str, Any]:
    result = {
        "status": "completed",
        "observer": "native_poll",
        "clock": "steady_clock",
        **metadata,
        **summary(latencies),
        "outer_bytes": outer_bytes,
        "median_outer_bytes": percentile(outer_bytes, 0.50),
    }
    if cpu_before is not None:
        result["workload_cpu"] = {
            **workload_cpu(runtime, cpu_before, len(latencies)),
            "includes": "untimed per-sample fixture preparation and probe launch",
        }
    return result


def painter_control(index: int, marker: bytes) -> bytes:
    # Consecutive fills differ, so every cell except the marker row changes between samples.
    return bytes((ord("a") + index % 26,)) + marker


def launch_painter(
    runtime: MuxRuntime,
    client: PtyProcess,
    receipts: PtyReceiptChannel,
    control: Path,
) -> None:
    command = (
        f"exec {shlex.quote(str(runtime.peer_path))} paint "
        f"{shlex.quote(str(receipts.path))} {shlex.quote(str(control))}\r"
    ).encode()
    client.write_all(command, 2.0)
    client.read_until(PAINT_READY, 5.0, visible_text=screen_renders_markers(runtime))
    client.drain(0.01)


def painted_switch_samples(
    runtime: MuxRuntime,
    client: PtyProcess,
    receipts: PtyReceiptChannel,
    controls: tuple[Path, Path],
    triggers: tuple[bytes, bytes],
    label: str,
    repetitions: int,
    **metadata: Any,
) -> dict[str, Any]:
    """Alternate the client between two painted screens; the client starts on the second.

    Before each sample the hidden target paints a unique token and acknowledges the write. A
    bounded settle lets the subject ingest that pane output, so the sample measures the switch.
    """
    latencies: list[int] = []
    outer_bytes: list[int] = []
    cpu_before = runtime_resource_snapshot(runtime)
    for index in range(repetitions):
        target = index % 2
        marker = interaction_marker(label, index)
        receipts.send_control(controls[target], painter_control(index, marker))
        receipts.wait_for_receipt(marker, 5.0)
        client.drain(0.05)
        latency, sample_bytes = trigger_sample(
            runtime, client, triggers[target], interaction_visible_token(label, index)
        )
        latencies.append(latency)
        outer_bytes.append(sample_bytes)
    return trigger_result(latencies, outer_bytes, runtime, cpu_before, **metadata)


def session_switch(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    if not isinstance(runtime, (LemmaRuntime, TmuxRuntime, HerdrRuntime)):
        raise TypeError("session switch requires an in-client Session switch trigger")
    receipts = PtyReceiptChannel(runtime.receipt_path)
    controls = (
        Path(f"{runtime.receipt_path}.a"),
        Path(f"{runtime.receipt_path}.b"),
    )
    names = ("switch_a", "switch_b")
    try:
        client = runtime.start_and_attach(names[0])
        launch_painter(runtime, client, receipts, controls[0])
        if isinstance(runtime, HerdrRuntime):
            # Herdr Sessions are separate servers; a Workspace is its in-client isolation unit.
            runtime.session_command(
                names[0], "workspace", "create", "--cwd", os.getcwd()
            )
            unit = "workspace"
        else:
            runtime.start_detached(names[1])
            unit = "session"
        if isinstance(runtime, LemmaRuntime):
            triggers = tuple(
                b"\x02:switch " + name.encode("ascii") + b"\r" for name in names
            )
            description = "C-b : switch NAME Enter (native command line)"
        else:
            keys = NAVIGATION_KEYS[runtime.multiplexer]
            triggers = (keys["previous_session"], keys["next_session"])
            description = (
                "C-b ( / C-b )"
                if isinstance(runtime, TmuxRuntime)
                else "prefix+( / prefix+) (configured workspace cycling)"
            )
        client.write_all(triggers[1], 2.0)
        wait_for_startup_shell(runtime, client)
        launch_painter(runtime, client, receipts, controls[1])
        return painted_switch_samples(
            runtime,
            client,
            receipts,
            controls,
            (triggers[0], triggers[1]),
            "SESSION",
            repetitions,
            switch_unit=unit,
            trigger=description,
        )
    finally:
        receipts.close()
        for control in controls:
            control.unlink(missing_ok=True)


def tab_switch(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    if isinstance(runtime, DirectRuntime):
        raise TypeError("tab switch requires a multiplexer")
    receipts = PtyReceiptChannel(runtime.receipt_path)
    controls = (
        Path(f"{runtime.receipt_path}.a"),
        Path(f"{runtime.receipt_path}.b"),
    )
    keys = NAVIGATION_KEYS[runtime.multiplexer]
    try:
        client = runtime.start_and_attach("tab_switch")
        launch_painter(runtime, client, receipts, controls[0])
        client.write_all(keys["new_tab"], 2.0)
        wait_for_startup_shell(runtime, client)
        launch_painter(runtime, client, receipts, controls[1])
        return painted_switch_samples(
            runtime,
            client,
            receipts,
            controls,
            (keys["previous_tab"], keys["next_tab"]),
            "TAB",
            repetitions,
            trigger="previous/next tab binding",
        )
    finally:
        receipts.close()
        for control in controls:
            control.unlink(missing_ok=True)


# Every subject starts new panes as login shells: Lemma always forks `$SHELL -l`, tmux uses its
# default login shell, and the Zellij and Herdr adapters configure login startup. The direct
# control matches. Login and non-login startup read different files, so mixed modes would compare
# different shell work.
SHELL_STARTUP_MODES = {
    "direct": "fork a PTY and exec $SHELL -l",
    "lemma": "daemon spawns $SHELL -l",
    "tmux": "default login shell (argv0 -NAME)",
    "zellij": "default_shell wrapper execs $SHELL -l (one extra /bin/sh exec)",
    "herdr": 'terminal.shell_mode = "login"',
}


def install_login_shell_marker(environment: dict[str, str], marker: bytes) -> None:
    """Print MARKER only from login startup, so a non-login shell cannot complete a sample."""
    # Markers are [A-Z0-9_]; the startup file, not echoed input, prints them.
    text = marker.decode("ascii")
    printf = f"printf '%s\\n' '{text}'\n"
    shell = Path(environment["SHELL"]).name
    home = Path(environment["HOME"])
    config = Path(environment["XDG_CONFIG_HOME"])
    if shell in {"sh", "dash", "ksh", "mksh"}:
        path, command = home / ".profile", printf
    elif shell == "bash":
        path, command = home / ".bash_profile", printf
    elif shell == "zsh":
        path, command = Path(environment["ZDOTDIR"]) / ".zprofile", printf
    elif shell == "fish":
        path = config / "fish" / "config.fish"
        command = f"if status is-login\n    {printf}end\n"
    elif shell in {"nu", "nushell"}:
        path = config / "nushell" / "config.nu"
        command = f"if $nu.is-login {{ print '{text}' }}\n"
    else:
        raise RuntimeError(
            f"benchmark startup does not support account login shell {environment['SHELL']!r}"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(command, encoding="utf-8")


def shell_startup_control(
    runtime: DirectRuntime, repetitions: int, label: str
) -> dict[str, Any]:
    """Direct control: the account login shell starts in a fresh PTY and prints its marker."""
    latencies: list[int] = []
    outer_bytes: list[int] = []
    shell = runtime.environment["SHELL"]
    for index in range(repetitions):
        marker = interaction_marker(label, index)
        install_login_shell_marker(runtime.environment, marker)
        try:
            completed = subprocess.run(
                [
                    str(runtime.probe_path),
                    "attach",
                    "1",
                    marker.decode("ascii"),
                    "--",
                    shell,
                    "-l",
                ],
                env=runtime.environment,
                check=True,
                capture_output=True,
                text=True,
                timeout=10.0,
            )
            measured = json.loads(completed.stdout)
        except subprocess.CalledProcessError as error:
            raise RuntimeError(
                f"native shell-startup probe failed: stderr={error.stderr!r}"
            ) from error
        except json.JSONDecodeError as error:
            raise RuntimeError(
                "native shell-startup probe returned invalid JSON"
            ) from error
        latency = measured.get("latency") if isinstance(measured, dict) else None
        samples = latency.get("samples_ns") if isinstance(latency, dict) else None
        sample_bytes = (
            measured.get("outer_bytes") if isinstance(measured, dict) else None
        )
        if (
            not isinstance(measured, dict)
            or measured.get("observer") != "native_poll"
            or not isinstance(samples, list)
            or len(samples) != 1
            or not isinstance(sample_bytes, list)
            or len(sample_bytes) != 1
        ):
            raise RuntimeError("native shell-startup probe returned an invalid result")
        latencies.extend(samples)
        outer_bytes.extend(sample_bytes)
    return trigger_result(
        latencies,
        outer_bytes,
        runtime,
        None,
        trigger="fork a PTY and exec the account login shell",
        shell_startup=SHELL_STARTUP_MODES["direct"],
    )


def new_shell_latency(
    runtime: MuxRuntime, repetitions: int, action: str, label: str
) -> dict[str, Any]:
    """Create a Pane or Tab by key and time until its login shell's unique marker is visible."""
    if isinstance(runtime, DirectRuntime):
        return shell_startup_control(runtime, repetitions, label)
    if not isinstance(
        runtime, (LemmaRuntime, TmuxRuntime, HerdrRuntime, ZellijRuntime)
    ):
        raise TypeError("new-shell latency requires a known runtime")
    session = action
    client = runtime.start_and_attach(session)
    trigger = NAVIGATION_KEYS[runtime.multiplexer][action]
    latencies: list[int] = []
    outer_bytes: list[int] = []
    cpu_before = runtime_resource_snapshot(runtime)
    for index in range(repetitions):
        marker = interaction_marker(label, index)
        install_login_shell_marker(runtime.environment, marker)
        latency, sample_bytes = trigger_sample(
            runtime, client, trigger, interaction_visible_token(label, index)
        )
        latencies.append(latency)
        outer_bytes.append(sample_bytes)
        # Close the focused new shell so every sample creates the same one-to-two transition.
        client.write_all(b"exit\r", 2.0)
        wait_for_profile_panes(runtime, client, session, 1)
        client.drain(0.05)
    return trigger_result(
        latencies,
        outer_bytes,
        runtime,
        cpu_before,
        trigger=f"{action.replace('_', ' ')} binding",
        shell_startup=SHELL_STARTUP_MODES[runtime.multiplexer],
    )


def new_pane(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    return new_shell_latency(runtime, repetitions, "new_pane", "SPLIT")


def new_tab(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    return new_shell_latency(runtime, repetitions, "new_tab", "NEWTAB")


def require_resized_pane(outer: tuple[int, int], pane: tuple[int, int]) -> None:
    """Subject chrome shrinks panes, but both dimensions must cross the 80x24 baseline."""
    baseline_columns, baseline_rows = RESIZE_GEOMETRIES[1]
    grew = outer[0] > baseline_columns
    if (pane[0] > baseline_columns, pane[1] > baseline_rows) != (grew, grew):
        raise RuntimeError(
            f"first repaint after resizing to {outer[0]}x{outer[1]} reported pane "
            f"{pane[0]}x{pane[1]}, not the requested side of "
            f"{baseline_columns}x{baseline_rows}"
        )


def resize_reflow(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    receipts = PtyReceiptChannel(runtime.receipt_path)
    try:
        client = runtime.start_and_attach("resize_reflow")
        launch = (
            f"exec {shlex.quote(str(runtime.peer_path))} resize-paint "
            f"{shlex.quote(str(receipts.path))} {shlex.quote(str(receipts.peer_path))}\r"
        ).encode()
        client.write_all(launch, 2.0)
        client.read_until(
            RESIZE_READY, 5.0, visible_text=screen_renders_markers(runtime)
        )
        client.drain(0.05)
        to_pty: list[int] = []
        to_outer: list[int] = []
        outer_bytes: list[int] = []
        outer_geometry: list[list[int]] = []
        pane_geometry: list[list[int]] = []
        cpu_before = runtime_resource_snapshot(runtime)
        for index in range(repetitions):
            columns, rows = RESIZE_GEOMETRIES[index % 2]
            marker = interaction_marker("RESIZE", index)
            # Measure isolated resizes rather than a drag burst: subjects may coalesce or
            # throttle resizes that follow the previous one closely.
            client.drain(RESIZE_QUIET_SECONDS)
            receipts.send_control(receipts.peer_path, painter_control(index, marker))
            receipts.wait_for_receipt(RESIZE_ARMED, 5.0)
            try:
                completed = subprocess.run(
                    [
                        str(runtime.probe_path),
                        "resize",
                        str(client.descriptor),
                        str(receipts.descriptor.fileno()),
                        str(rows),
                        str(columns),
                        marker.decode("ascii"),
                        interaction_visible_token("RESIZE", index).decode("ascii"),
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                    pass_fds=(client.descriptor, receipts.descriptor.fileno()),
                    timeout=10.0,
                )
                measured = json.loads(completed.stdout)
            except subprocess.CalledProcessError as error:
                raise RuntimeError(
                    f"native resize probe failed: stderr={error.stderr!r}"
                ) from error
            except json.JSONDecodeError as error:
                raise RuntimeError(
                    "native resize probe returned invalid JSON"
                ) from error
            try:
                pane_columns, pane_rows = (
                    int(value) for value in str(measured["pane_geometry"]).split("x")
                )
            except (KeyError, ValueError) as error:
                raise RuntimeError(
                    "native resize probe returned no pane geometry"
                ) from error
            require_resized_pane((columns, rows), (pane_columns, pane_rows))
            to_pty.append(int(measured["resize_to_pty_ns"]))
            to_outer.append(int(measured["resize_to_outer_bytes_ns"]))
            outer_bytes.append(int(measured["outer_bytes"]))
            outer_geometry.append([columns, rows])
            pane_geometry.append([pane_columns, pane_rows])
        return {
            "status": "completed",
            "observer": "native_poll",
            "clock": "steady_clock",
            "trigger": "outer PTY TIOCSWINSZ alternating 100x30 and 80x24",
            "resize_to_pty": summary(to_pty),
            "resize_to_outer_bytes": summary(to_outer),
            "outer_bytes": outer_bytes,
            "median_outer_bytes": percentile(outer_bytes, 0.50),
            "outer_geometry": outer_geometry,
            "pane_geometry": pane_geometry,
            "workload_cpu": {
                **workload_cpu(runtime, cpu_before, repetitions),
                "includes": "untimed per-sample fixture arming and probe launch",
            },
        }
    finally:
        receipts.close()


class WorkloadFailure(RuntimeError):
    def __init__(self, message: str, result: dict[str, Any]) -> None:
        super().__init__(message)
        self.result = result


def blocked_pty(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    receipts = PtyReceiptChannel(runtime.receipt_path)
    try:
        blocked = runtime.start_and_attach("blocked_benchmark")
        responsive = runtime.start_and_attach("responsive_benchmark")
        receipt_launch = (
            f"exec {shlex.quote(str(runtime.peer_path))} latency "
            f"{shlex.quote(str(runtime.receipt_path))}\r"
        ).encode()
        responsive.write_all(receipt_launch, 2.0)
        responsive.read_until(
            LATENCY_READY, 5.0, visible_text=screen_renders_markers(runtime)
        )
        responsive.drain(0.01)
        idle = latency_samples(
            responsive,
            receipts,
            runtime.probe_path,
            "IDLE",
            repetitions,
            runtime=runtime,
        )

        launch = (
            f"exec {shlex.quote(str(runtime.peer_path))} block "
            f"{shlex.quote(str(runtime.gate_path))} {PAYLOAD_SIZE}\r"
        ).encode()
        blocked.write_all(launch, 2.0)
        blocked.read_until(
            BLOCK_READY, 5.0, visible_text=screen_renders_markers(runtime)
        )
        payload = b"q" * PAYLOAD_SIZE
        result: dict[str, Any] = {"payload_bytes": PAYLOAD_SIZE, "idle": idle}
        try:
            # Block pane input only, not its outer terminal. The monitor owns
            # reads during filling, the other client's native probe, and recovery.
            with PtyOutputMonitor(
                blocked,
                BLOCK_DONE,
                failure_markers=(
                    b"__LEMMA_PTY_FAILED__",
                    b"lost connection",
                    b"server exited unexpectedly",
                    b"Received empty unknown from server",
                ),
                visible_text=screen_renders_markers(runtime),
            ) as completion:
                accepted = blocked.fill_until_stalled(payload)
                result.update(
                    bytes_before_backpressure=accepted,
                    client_backpressure_observed=accepted < len(payload),
                )
                completion.check()
                if accepted <= 0:
                    raise RuntimeError("blocked PTY accepted no payload")

                result["blocked_other_session"] = latency_samples(
                    responsive,
                    receipts,
                    runtime.probe_path,
                    "BLOCKED",
                    repetitions,
                    runtime=runtime,
                )
                runtime.gate_path.touch(mode=0o600, exist_ok=False)
                try:
                    blocked.write_all(payload[accepted:], 60.0)
                except (OSError, RuntimeError, TimeoutError):
                    # Prefer an observed peer failure over a secondary write
                    # timeout or EIO, without accepting an unclassified timeout.
                    completion.check()
                    raise
                completion.wait(60.0)
        except (OSError, RuntimeError, TimeoutError) as error:
            raise WorkloadFailure(
                str(error),
                {
                    **result,
                    "status": "failed",
                    "error": f"{type(error).__name__}: {error}",
                },
            ) from error

        return {**result, "status": "completed"}
    finally:
        receipts.close()


def session_profile(
    runtime: MuxRuntime, sessions: int, repetitions: int
) -> dict[str, Any]:
    for index in range(sessions):
        runtime.start_detached(f"session_profile_{index}")
    return {
        "status": "completed",
        "sessions": sessions,
        "panes_per_session": 1,
        "resources": sample_resources(runtime, repetitions),
    }


def workspace_profile(
    runtime: MuxRuntime, workspaces: int, repetitions: int
) -> dict[str, Any]:
    if not isinstance(runtime, HerdrRuntime):
        result = session_profile(runtime, workspaces, repetitions)
        result["workspaces"] = result.pop("sessions")
        result["panes_per_workspace"] = result.pop("panes_per_session")
        return result

    session = "workspace_profile"
    runtime.start_detached(session)
    for _ in range(1, workspaces):
        runtime.session_command(session, "workspace", "create", "--cwd", os.getcwd())
    if runtime.pane_count(session) != workspaces:
        raise RuntimeError(
            "Herdr workspace profile did not create one pane per workspace"
        )
    return {
        "status": "completed",
        "workspaces": workspaces,
        "panes_per_workspace": 1,
        "resources": sample_resources(runtime, repetitions),
    }


def component_resources(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    if not isinstance(runtime, LemmaRuntime):
        raise TypeError("component resources require Lemma")
    baseline = sample_resources(runtime, repetitions)
    runtime.command("start", "component_resources")
    detached = sample_resources(runtime, repetitions)
    client = runtime.attach("component_resources")
    client.drain()
    attached = sample_resources(runtime, repetitions, client)
    runtime.detach(client, "component_resources")
    detached_after_attach = sample_resources(runtime, repetitions)
    return {
        "status": "completed",
        "baseline": baseline,
        "detached_session": detached,
        "attached_session": attached,
        "detached_after_attach": detached_after_attach,
    }


def history_resources(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    if not isinstance(runtime, LemmaRuntime):
        raise TypeError("history resources require Lemma")
    client = runtime.start_and_attach("history_resources")
    client.drain()
    empty = sample_resources(runtime, repetitions, client)
    command = f"{shlex.quote(str(runtime.peer_path))} warm-scroll\r".encode()
    client.write_all(command, 2.0)
    client.read_until(WARM_MARKER, 60.0)
    client.drain()
    populated = sample_resources(runtime, repetitions, client)
    return {
        "status": "completed",
        "history_input_rows": 25_000,
        "empty": empty,
        "populated": populated,
    }


def blocked_client(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    if not isinstance(runtime, LemmaRuntime):
        raise TypeError("blocked-client workload requires Lemma")
    receipts = PtyReceiptChannel(runtime.receipt_path)
    blocked: socket.socket | None = None
    disconnect_probe: subprocess.Popen[str] | None = None
    ready_read = -1
    try:
        runtime.command("start", "blocked_client")
        responsive = runtime.start_and_attach("responsive_client_peer")
        receipt_launch = (
            f"exec {shlex.quote(str(runtime.peer_path))} latency "
            f"{shlex.quote(str(runtime.receipt_path))}\r"
        ).encode()
        responsive.write_all(receipt_launch, 2.0)
        responsive.read_until(LATENCY_READY, 5.0)
        responsive.drain(0.01)
        idle = latency_samples(
            responsive,
            receipts,
            runtime.probe_path,
            "CLIENT_IDLE",
            repetitions,
            runtime=runtime,
        )

        session = b"blocked_client"
        hello_payload = (
            bytes((len(session),)) + struct.pack("!HH", 500, 200) + b"\0" + session
        )
        blocked, attach_minor = connect_blocked_client(
            runtime.socket_path, hello_payload
        )
        flood_command = (
            f"exec {shlex.quote(str(runtime.peer_path))} output-flood\r"
        ).encode()
        flood_frame = attach_frame(
            ATTACH_KIND_INPUT, flood_command, 2, minor=attach_minor
        )
        ready_read, ready_write = os.pipe()
        disconnect_probe = subprocess.Popen(
            [
                str(runtime.probe_path),
                "disconnect",
                str(blocked.fileno()),
                str(ready_write),
                flood_frame.hex(),
                str(BLOCKED_CLIENT_DISCONNECT_DEADLINE_NS // 1_000_000),
            ],
            pass_fds=(blocked.fileno(), ready_write),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        os.close(ready_write)
        readable, _, _ = select.select([ready_read], [], [], 2.0)
        if not readable or os.read(ready_read, 1) != b"R":
            raise TimeoutError("native disconnect probe did not arm the blocked client")
        os.close(ready_read)
        ready_read = -1

        under_backpressure = latency_samples(
            responsive,
            receipts,
            runtime.probe_path,
            "CLIENT_BLOCKED",
            repetitions,
            runtime=runtime,
        )
        probe_output, probe_error = disconnect_probe.communicate(timeout=6.0)
        if disconnect_probe.returncode != 0:
            raise TimeoutError(
                "blocked attached client exceeded its no-progress disconnect bound: "
                f"{probe_error.strip()}"
            )
        try:
            disconnect_result = json.loads(probe_output)
            disconnect_latency_ns = int(disconnect_result["disconnect_latency_ns"])
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            raise RuntimeError(
                "native disconnect probe returned invalid JSON"
            ) from error
        if (
            disconnect_result.get("observer") != "native_poll"
            or disconnect_latency_ns > BLOCKED_CLIENT_DISCONNECT_DEADLINE_NS
        ):
            raise TimeoutError(
                "blocked attached client was observed after its disconnect bound"
            )
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if ", detached," in runtime.command("list", "blocked_client").stdout:
                break
            time.sleep(0.01)
        else:
            raise TimeoutError("blocked client disconnected without publishing detach")

        return {
            "status": "completed",
            "receive_buffer_bytes": 4 * 1_024,
            "private_attach_version": f"{ATTACH_PROTOCOL_MAJOR}.{attach_minor}",
            "disconnect": {
                "observer": "native_poll",
                "clock": disconnect_result["clock"],
                "latency_ns": disconnect_latency_ns,
            },
            "idle": idle,
            "blocked_other_session": under_backpressure,
        }
    finally:
        if ready_read >= 0:
            os.close(ready_read)
        if disconnect_probe is not None and disconnect_probe.poll() is None:
            disconnect_probe.kill()
            disconnect_probe.wait(timeout=1.0)
        if blocked is not None:
            blocked.close()
        receipts.close()


def extension_isolation(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    if not isinstance(runtime, LemmaRuntime):
        raise TypeError("extension-isolation workload requires Lemma")
    mode = runtime.extension_fixture_mode
    if mode not in {"blocked-reader", "crash-focused", "crash-docked"}:
        raise ValueError("extension-isolation requires a blocked or crash fixture")

    session = "extension_isolation"
    runtime.start_detached(session)
    client = runtime.attach(session)
    wait_for_startup_shell(runtime, client)
    client.write_all(b"exec cat\r", 2.0)
    client.drain(0.1)
    runtime._start_extension_fixture(session)
    client.drain(0.1)

    activity_before = runtime_resource_snapshot(runtime)
    paste_submit_ns: int | None = None
    paste_bytes = 0
    if mode == "blocked-reader":
        paste = b"\x1b[200~" + (b"x" * (1024 * 1024)) + b"\x1b[201~"
        paste_bytes = 1024 * 1024
        started = time.perf_counter_ns()
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            submitted = executor.submit(client.write_all, paste, 15.0)
            while not submitted.done():
                client.drain(0.005)
            submitted.result()
        paste_submit_ns = time.perf_counter_ns() - started
        client.drain(0.25)
    else:
        time.sleep(0.25)
    activity_after = runtime_resource_snapshot(runtime)

    marker = f"__LEMMA_EXTENSION_CLEANUP_{time.monotonic_ns():016x}__".encode()
    cleanup_started = time.perf_counter_ns()
    runtime.stop_extension_fixture(crash=mode.startswith("crash-"))
    client.write_all(marker + b"\r", 2.0)
    cleanup_output_bytes = len(client.read_until(marker, 5.0))
    cleanup_ns = time.perf_counter_ns() - cleanup_started
    telemetry: list[dict[str, Any]] = []
    for process in runtime.extension_processes:
        if process.stdout is None:
            continue
        line = process.stdout.readline()
        if line:
            try:
                telemetry.append(json.loads(line))
            except json.JSONDecodeError as error:
                raise RuntimeError(
                    f"invalid extension fixture telemetry: {line!r}"
                ) from error

    return {
        "status": "completed",
        "fixture": mode,
        "repetitions": repetitions,
        "paste_payload_bytes": paste_bytes,
        "paste_submit_ns": paste_submit_ns,
        "cleanup_to_outer_bytes_ns": cleanup_ns,
        "cleanup_output_bytes": cleanup_output_bytes,
        "fixture_telemetry": telemetry,
        "activity_resources": {
            "before": activity_before,
            "after": activity_after,
        },
        "resources_after_cleanup": runtime_resource_snapshot(runtime),
    }


def wait_for_profile_panes(
    runtime: LemmaRuntime | TmuxRuntime | HerdrRuntime | ZellijRuntime,
    client: PtyProcess,
    session: str,
    panes: int,
) -> None:
    deadline = time.monotonic() + (
        ZELLIJ_PANE_LISTING_SECONDS if isinstance(runtime, ZellijRuntime) else 5.0
    )
    while time.monotonic() < deadline:
        client.drain(0.005)
        if isinstance(runtime, LemmaRuntime):
            reached = f", {panes} pane(s)," in runtime.command("list", session).stdout
        elif isinstance(runtime, (HerdrRuntime, ZellijRuntime)):
            reached = runtime.pane_count(session) == panes
        else:
            listing = runtime._command(
                "list-panes", "-a", "-t", session, "-F", "#{pane_id}"
            ).stdout
            reached = len(listing.splitlines()) == panes
        if reached:
            return
        time.sleep(0.005)
    listing_error = getattr(runtime, "pane_listing_error", None)
    raise TimeoutError(
        f"{runtime.multiplexer} did not reach {panes} panes"
        + (f"; last {listing_error}" if listing_error else "")
    )


def send_prefix(client: PtyProcess, command: bytes) -> None:
    client.write_all(b"\x02" + command, 2.0)


def wait_for_profile_shell(
    runtime: LemmaRuntime | TmuxRuntime | HerdrRuntime | ZellijRuntime,
    client: PtyProcess,
    pane_index: int,
) -> None:
    # Pane publication only proves that the PTY child exists; sampling before login-shell
    # startup settles otherwise charges setup CPU to the nominally idle interval.
    if screen_renders_markers(runtime):
        # Rendered frames can omit a marker that a wrapped echo and multi-line host prompt
        # scroll out of a short pane before the next frame. A file the shell creates proves
        # execution without depending on pane height or prompt shape.
        ready = (
            Path(runtime.environment["TMPDIR"]) / f"profile-pane-{pane_index:04d}.ready"
        )
        client.write_all(f"touch {shlex.quote(str(ready))}\r".encode(), 2.0)
        deadline = time.monotonic() + 5.0
        while not ready.exists():
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"profile pane {pane_index} shell did not execute input"
                )
            client.drain(0.005)
        client.drain(0.005)
        return
    marker = f"__LEMMA_PROFILE_PANE_{pane_index:04d}_READY__".encode()
    # Keep the complete marker out of the echoed command so observation proves the shell executed
    # it.
    command = f"printf '__LEMMA_PROFILE_PANE_%04d_READY__\\n' {pane_index}\r".encode()
    wait_for_shell_execution(runtime, client, marker, command)


def launch_latency_peer(
    runtime: LemmaRuntime | TmuxRuntime | HerdrRuntime | ZellijRuntime,
    client: PtyProcess,
    autonomous_output: bool,
    receipts: PtyReceiptChannel | None = None,
) -> None:
    mode = "latency-output" if autonomous_output else "latency"
    ready = LATENCY_OUTPUT_READY if autonomous_output else LATENCY_READY
    command = (
        f"exec {shlex.quote(str(runtime.peer_path))} {mode} "
        f"{shlex.quote(str(runtime.receipt_path))}\r"
    ).encode()
    client.write_all(command, 2.0)
    client.read_until(ready, 5.0, visible_text=screen_renders_markers(runtime))
    if autonomous_output:
        if receipts is None:
            raise ValueError("autonomous output requires a receipt channel")
        release_autonomous_output(receipts)
    client.drain(0.06 if autonomous_output else 0.005)


def build_profile(
    runtime: LemmaRuntime | TmuxRuntime | HerdrRuntime | ZellijRuntime,
    client: PtyProcess,
    panes: int,
    session: str = "profile",
) -> None:
    pane_index = 1
    wait_for_profile_shell(runtime, client, pane_index)
    if isinstance(runtime, ZellijRuntime):
        for index in range(1, panes):
            if index % 4 == 0:
                runtime.session_command(session, "new-tab")
            else:
                runtime.session_command(
                    session,
                    "new-pane",
                    "--direction",
                    "right" if index % 4 == 1 else "down",
                )
            wait_for_profile_panes(runtime, client, session, index + 1)
            wait_for_profile_shell(runtime, client, index + 1)
        return
    if panes == 2:
        if isinstance(runtime, HerdrRuntime):
            runtime.session_command(
                session, "pane", "split", "--current", "--direction", "right", "--focus"
            )
        else:
            send_prefix(client, b"%")
        wait_for_profile_panes(runtime, client, session, 2)
        wait_for_profile_shell(runtime, client, 2)
        return
    tab_count = 1 if panes == 1 else panes // 4
    for tab_index in range(tab_count):
        if panes == 1:
            return

        if isinstance(runtime, HerdrRuntime):
            if tab_index > 0:
                runtime.session_command(session, "tab", "create", "--focus")
                pane_index += 1
                wait_for_profile_panes(runtime, client, session, pane_index)
                wait_for_profile_shell(runtime, client, pane_index)
            for direction in ("right", "down", "down"):
                runtime.session_command(
                    session,
                    "pane",
                    "split",
                    "--current",
                    "--direction",
                    direction,
                    "--focus",
                )
                pane_index += 1
                wait_for_profile_panes(runtime, client, session, pane_index)
                wait_for_profile_shell(runtime, client, pane_index)
            continue

        send_prefix(client, b"%")
        pane_index += 1
        wait_for_profile_panes(runtime, client, session, pane_index)
        wait_for_profile_shell(runtime, client, pane_index)

        send_prefix(client, b'"')
        pane_index += 1
        wait_for_profile_panes(runtime, client, session, pane_index)
        wait_for_profile_shell(runtime, client, pane_index)

        send_prefix(client, b"o")
        send_prefix(client, b'"')
        pane_index += 1
        wait_for_profile_panes(runtime, client, session, pane_index)
        wait_for_profile_shell(runtime, client, pane_index)

        if tab_index + 1 < tab_count:
            send_prefix(client, b"c")
            wait_for_profile_panes(runtime, client, session, pane_index + 1)
            pane_index += 1
            wait_for_profile_shell(runtime, client, pane_index)


def pane_profile(
    runtime: LemmaRuntime | TmuxRuntime | HerdrRuntime | ZellijRuntime,
    profile: str,
    panes: int,
    active: bool,
    repetitions: int,
) -> dict[str, Any]:
    receipts = PtyReceiptChannel(runtime.receipt_path)
    try:
        client = runtime.start_and_attach("profile")
        build_profile(runtime, client, panes)
        if not active:
            resources = sample_resources(runtime, repetitions, client)
            launch_latency_peer(runtime, client, False)
        else:
            launch_latency_peer(runtime, client, True, receipts)
            resources = sample_resources(runtime, repetitions, client)
        interaction = latency_samples(
            client,
            receipts,
            runtime.probe_path,
            f"{profile}_{'ACTIVE' if active else 'IDLE'}",
            repetitions,
            wait_for_peer_ready=active,
            runtime=runtime,
        )
        return {
            "status": "completed",
            "panes": panes,
            "activity": "active" if active else "idle",
            "resources": resources,
            "interaction": interaction,
        }
    finally:
        receipts.close()


def lifecycle_sentinel_arguments(peer_path: Path) -> tuple[str, ...]:
    # Use the built fixture rather than a distribution-specific utility path. The idle peer also
    # gives this measurement one explicit, quiescent process lifetime.
    return ("start", "lifecycle_sentinel", "--", str(peer_path), "idle")


def lifecycle_churn(runtime: MuxRuntime, repetitions: int) -> dict[str, Any]:
    if not isinstance(runtime, LemmaRuntime):
        raise TypeError("lifecycle churn requires Lemma")
    # Keep one ordinary session alive so deleting the churn target does not intentionally stop the
    # daemon whose descriptor and memory plateau this workload audits. Final-session auto-exit has
    # separate process coverage; the runtime process group reclaims this sentinel after sampling.
    runtime.command(*lifecycle_sentinel_arguments(runtime.peer_path))
    baseline = runtime_resource_snapshot(runtime)
    if baseline.get("available") is not True:
        raise RuntimeError("lifecycle churn baseline resource snapshot is unavailable")
    baseline_descriptors = open_descriptor_snapshot(runtime.server.pid)
    if baseline_descriptors.get("available") is not True:
        raise RuntimeError("lifecycle churn descriptor baseline is unavailable")

    tree_rss: list[int] = []
    daemon_rss: list[int] = []
    process_counts: list[int] = []
    descriptor_counts: list[int] = []
    focused_pids: list[int] = []
    restored_clients = 0
    for _ in range(repetitions):
        runtime.command("start", "lifecycle_churn")
        client = runtime.attach("lifecycle_churn")
        build_profile(runtime, client, 4, "lifecycle_churn")
        listing = runtime.command("list", "lifecycle_churn").stdout
        try:
            focused_pid = int(listing.split("focused pid ", 1)[1].split(",", 1)[0])
        except (IndexError, ValueError) as error:
            raise RuntimeError(
                "lifecycle churn could not identify its live pane"
            ) from error
        if focused_pid <= 0:
            raise RuntimeError(
                "lifecycle churn observed an invalid focused pane identity"
            )
        focused_pids.append(focused_pid)
        for expected in (3, 2, 1):
            send_prefix(client, b"x")
            wait_for_profile_panes(runtime, client, "lifecycle_churn", expected)
        runtime.detach(client, "lifecycle_churn")
        if client.terminal_state_restored is not True:
            raise RuntimeError("lifecycle churn leaked attached-client terminal state")
        restored_clients += 1
        runtime.command("kill", "lifecycle_churn")
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            listing = runtime.command("list").stdout
            if "lifecycle_churn" not in listing:
                break
            time.sleep(0.005)
        else:
            raise TimeoutError("lifecycle churn session was not reclaimed")
        snapshot = runtime_resource_snapshot(runtime)
        if snapshot.get("available") is not True:
            raise RuntimeError("lifecycle churn resource snapshot is unavailable")
        daemon = snapshot.get("roles", {}).get("daemon", {})
        if daemon.get("available") is not True:
            raise RuntimeError("lifecycle churn daemon snapshot is unavailable")
        descriptor_snapshot = open_descriptor_snapshot(runtime.server.pid)
        if descriptor_snapshot.get("available") is not True:
            raise RuntimeError("lifecycle churn descriptor snapshot is unavailable")
        process_count = int(snapshot["process_count"])
        descriptor_count = int(descriptor_snapshot["count"])
        if process_count != int(baseline["process_count"]):
            raise RuntimeError("lifecycle churn left a pane or client process live")
        if descriptor_count != int(baseline_descriptors["count"]):
            raise RuntimeError("lifecycle churn leaked a daemon descriptor")
        tree_rss.append(int(snapshot["rss_bytes"]))
        daemon_rss.append(int(daemon["rss_bytes"]))
        process_counts.append(process_count)
        descriptor_counts.append(descriptor_count)

    plateau_cycles = min(100, max(25, repetitions // 10))
    plateau: dict[str, Any]
    if repetitions >= plateau_cycles * 2:
        preceding = daemon_rss[-(plateau_cycles * 2) : -plateau_cycles]
        final = daemon_rss[-plateau_cycles:]
        final_range = max(final) - min(final)
        preceding_p95 = percentile(preceding, 0.95)
        final_p95 = percentile(final, 0.95)
        x_mean = (plateau_cycles - 1) / 2
        denominator = sum((index - x_mean) ** 2 for index in range(plateau_cycles))
        final_mean = sum(final) / len(final)
        numerator = sum(
            (index - x_mean) * (value - final_mean) for index, value in enumerate(final)
        )
        slope = numerator / denominator
        plateau = {
            "evaluated": True,
            "window_cycles": plateau_cycles,
            "preceding_p95_bytes": preceding_p95,
            "final_p95_bytes": final_p95,
            "final_range_bytes": final_range,
            "final_slope_bytes_per_cycle": slope,
            "maximum_final_range_bytes": 2 * 1_024 * 1_024,
            "maximum_p95_growth_bytes": 1 * 1_024 * 1_024,
            "maximum_final_slope_bytes_per_cycle": 4_096,
        }
        if (
            final_range > plateau["maximum_final_range_bytes"]
            or final_p95 > preceding_p95 + plateau["maximum_p95_growth_bytes"]
            or slope > plateau["maximum_final_slope_bytes_per_cycle"]
        ):
            raise RuntimeError(
                f"lifecycle churn did not return to a bounded memory plateau: {plateau}"
            )
    else:
        plateau = {
            "evaluated": False,
            "reason": f"at least {plateau_cycles * 2} cycles are required",
        }

    return {
        "status": "completed",
        "cycles": repetitions,
        "baseline_condition": "one ordinary sentinel session remains live across cycles",
        "operations_per_cycle": [
            "create",
            "attach",
            "split",
            "close",
            "detach",
            "kill",
        ],
        "identity_check": (
            "each incarnation exposed a positive live focused pane PID and the reused session "
            "name was absent before the next create"
        ),
        "focused_pids": focused_pids,
        "terminal_restorations": restored_clients,
        "tree_rss": metric_summary(tree_rss, "bytes"),
        "daemon_rss": metric_summary(daemon_rss, "bytes"),
        "process_counts": metric_summary(process_counts, "count"),
        "daemon_open_descriptors": {
            "source": baseline_descriptors["source"],
            **metric_summary(descriptor_counts, "count"),
        },
        "memory_plateau": plateau,
    }


def git_provenance() -> tuple[str, bool | None, str | None]:
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
            timeout=GIT_METADATA_TIMEOUT_SECONDS,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown", None, None

    try:
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=normal"],
            check=True,
            capture_output=True,
            text=True,
            timeout=GIT_METADATA_TIMEOUT_SECONDS,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return commit, None, None

    if not status:
        return commit, False, None
    try:
        tracked_diff = subprocess.run(
            ["git", "diff", "--binary", "HEAD"],
            check=True,
            capture_output=True,
            timeout=GIT_METADATA_TIMEOUT_SECONDS,
        ).stdout
        untracked = sorted(
            line[3:]
            for line in status.splitlines()
            if line.startswith("?? ") and Path(line[3:]).is_file()
        )
        digest = hashlib.sha256(tracked_diff)
        for name in untracked:
            digest.update(name.encode("utf-8"))
            digest.update(Path(name).read_bytes())
    except (OSError, subprocess.SubprocessError):
        return commit, True, None
    return commit, True, digest.hexdigest()


def latency_trace_metadata(directory: Path | None) -> dict[str, Any]:
    if directory is None:
        return {
            "requested": False,
            "available": False,
            "reason": "benchmark tracing was not requested",
        }
    files = []
    total_events = 0
    total_dropped = 0
    header = struct.Struct("<QIHHIIQQ24x")
    for path in sorted(directory.glob("*.ltrace")):
        try:
            encoded = path.read_bytes()
            magic, version, role, event_size, capacity, process, count, dropped = (
                header.unpack_from(encoded)
            )
        except (OSError, struct.error) as error:
            files.append({"path": str(path), "valid": False, "error": str(error)})
            continue
        valid = (
            magic == 0x3145_4341_5254_4D4C
            and version == 2
            and role in (1, 2)
            and event_size == 40
            and process > 0
            and count <= capacity == 524_288
            and encoded[40 : header.size] == bytes(header.size - 40)
            and len(encoded) == header.size + (capacity * event_size)
        )
        files.append(
            {
                "path": str(path),
                "valid": valid,
                "role": role,
                "process": process,
                "events": count,
                "dropped": dropped,
            }
        )
        if valid:
            total_events += count
            total_dropped += dropped
    return {
        "requested": True,
        "available": bool(files) and all(item.get("valid") is True for item in files),
        "reason": None if files else "trace-enabled binaries produced no trace files",
        "event_capacity_per_process": 524_288,
        "events": total_events,
        "dropped": total_dropped,
        "files": files,
    }


def main() -> int:
    manifest_parser = argparse.ArgumentParser(add_help=False)
    manifest_parser.add_argument(
        "--manifest", type=Path, default=Path("benchmarks/workloads.json")
    )
    manifest_arguments, _ = manifest_parser.parse_known_args()
    try:
        manifest = load_manifest(manifest_arguments.manifest)
    except ManifestError as error:
        manifest_parser.error(str(error))
    process_modes = tuple(
        workload["cli_mode"] for workload in manifest["process_workloads"]
    )

    parser = argparse.ArgumentParser(parents=[manifest_parser])
    parser.add_argument(
        "--mode",
        choices=(
            *process_modes,
            "comparison",
            "regression",
            "profiles",
            "session-profiles",
            "workspace-profiles",
            "all",
        ),
        default="comparison",
    )
    parser.add_argument(
        "--multiplexer",
        choices=("direct", "lemma", "tmux", "zellij", "herdr"),
        default="lemma",
    )
    parser.add_argument("--repetitions", type=int)
    parser.add_argument(
        "--intent", choices=("smoke", "extended", "gate", "manual"), default="manual"
    )
    parser.add_argument(
        "--server", type=Path, default=Path("build/release/lemma_test_server")
    )
    parser.add_argument(
        "--cli", type=Path, default=Path("build/release/lemma_test_cli")
    )
    parser.add_argument(
        "--peer", type=Path, default=Path("build/release/lemma_test_pty_peer")
    )
    parser.add_argument(
        "--probe", type=Path, default=Path("build/release/lemma_benchmark_probe")
    )
    parser.add_argument("--tmux", type=Path, default=Path("tmux"))
    parser.add_argument("--zellij", type=Path, default=Path("zellij"))
    parser.add_argument("--herdr", type=Path, default=Path("herdr"))
    parser.add_argument("--trace-directory", type=Path)
    parser.add_argument("--extension-fixture", choices=EXTENSION_FIXTURE_MODES)
    parser.add_argument(
        "--extension-fixture-path",
        type=Path,
        default=Path("benchmarks/extension_fixture.py"),
    )
    parser.add_argument("--extension-fixture-processes", type=int, default=0)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--allow-workload-failures",
        action="store_true",
        help="record competitor workload failures instead of failing the harness",
    )
    arguments = parser.parse_args()
    if arguments.repetitions is None:
        policy_name = arguments.intent if arguments.intent != "manual" else "smoke"
        policy = manifest["sample_policies"][policy_name]
        arguments.repetitions = (
            policy["profile_repetitions"]
            if arguments.mode == "profiles"
            else policy["process_repetitions"]
        )
    if arguments.repetitions < 1 or arguments.repetitions > 10_000:
        parser.error("--repetitions must be between 1 and 10000")
    selected_scenarios = (
        suite_workloads(manifest, arguments.mode)
        if arguments.mode in {"comparison", "regression", "all"}
        else []
    )
    individual_scenario = workload_for_mode(manifest, arguments.mode)
    if individual_scenario is not None:
        selected_scenarios = [individual_scenario]
    requires_attach_fixture = any(
        scenario["id"] == "attach_to_visible"
        and arguments.multiplexer in scenario["subjects"]
        for scenario in selected_scenarios
    )
    if requires_attach_fixture:
        account_shell = account_login_shell()
        if Path(account_shell).name not in ATTACH_STARTUP_SHELLS:
            parser.error(
                f"attach-to-visible does not support account login shell {account_shell!r}"
            )
    for fixture_executable in (arguments.peer, arguments.probe):
        if not fixture_executable.is_file():
            parser.error(f"missing executable: {fixture_executable}")
    if arguments.mode == "profiles" and arguments.multiplexer == "direct":
        parser.error("pane profiles require a multiplexer")
    if arguments.extension_fixture is not None:
        if arguments.multiplexer != "lemma":
            parser.error("extension fixtures require the lemma multiplexer")
        if not arguments.extension_fixture_path.is_file():
            parser.error(
                f"missing extension fixture: {arguments.extension_fixture_path}"
            )
        if not 0 <= arguments.extension_fixture_processes <= 32:
            parser.error("extension fixture process count exceeds the peer limit")
        if (
            arguments.extension_fixture
            in {"blocked-reader", "crash-focused", "crash-docked"}
            and arguments.extension_fixture_processes > 1
        ):
            parser.error("blocked and crash fixtures require one extension process")
    if arguments.trace_directory is not None:
        arguments.trace_directory.mkdir(parents=True, mode=0o700, exist_ok=True)
        if any(arguments.trace_directory.glob("*.ltrace")):
            parser.error("--trace-directory must not contain existing .ltrace files")

    build_profile: str | None = None
    tmux: Path | None = None
    zellij: Path | None = None
    herdr: Path | None = None
    if arguments.multiplexer == "direct":
        build_profile = arguments.probe.parent.name
    elif arguments.multiplexer == "lemma":
        for executable in (arguments.server, arguments.cli):
            if not executable.is_file():
                parser.error(f"missing executable: {executable}")
        build_profile = arguments.server.parent.name
    elif arguments.multiplexer == "tmux":
        tmux = Path(shutil.which(str(arguments.tmux)) or arguments.tmux)
        if not tmux.is_file():
            parser.error(f"missing executable: {arguments.tmux}")
    elif arguments.multiplexer == "zellij":
        zellij = Path(shutil.which(str(arguments.zellij)) or arguments.zellij)
        if not zellij.is_file():
            parser.error(f"missing executable: {arguments.zellij}")
    else:
        herdr = Path(shutil.which(str(arguments.herdr)) or arguments.herdr)
        if not herdr.is_file():
            parser.error(f"missing executable: {arguments.herdr}")

    def create_runtime() -> MuxRuntime:
        if arguments.multiplexer == "direct":
            return DirectRuntime(arguments.peer, arguments.probe)
        if arguments.multiplexer == "lemma":
            return LemmaRuntime(
                arguments.server,
                arguments.cli,
                arguments.peer,
                arguments.probe,
                arguments.trace_directory,
                arguments.extension_fixture,
                arguments.extension_fixture_path,
                arguments.extension_fixture_processes,
            )
        if arguments.multiplexer == "tmux":
            assert tmux is not None
            return TmuxRuntime(tmux, arguments.peer, arguments.probe)
        if arguments.multiplexer == "zellij":
            assert zellij is not None
            return ZellijRuntime(zellij, arguments.peer, arguments.probe)
        assert herdr is not None
        return HerdrRuntime(herdr, arguments.peer, arguments.probe)

    workloads: dict[str, Any] = {}
    pane_profiles: dict[str, Any] = {}
    session_profiles: dict[str, Any] = {}
    workspace_profiles: dict[str, Any] = {}
    runtime_version = "unknown"
    binary_provenance: dict[str, Any] = {}

    def run_operation(
        operation: Callable[[MuxRuntime, int], dict[str, Any]],
        repetitions: int | None = None,
    ) -> dict[str, Any]:
        nonlocal runtime_version, binary_provenance
        runtime: MuxRuntime | None = None
        try:
            runtime = create_runtime()
            runtime_version = runtime.version
            result = operation(
                runtime, arguments.repetitions if repetitions is None else repetitions
            )
            result["resources_after_workload"] = runtime_resource_snapshot(runtime)
            if isinstance(runtime, LemmaRuntime):
                result["extension_fixture_ready"] = runtime.extension_fixture_ready
            if not binary_provenance:
                binary_provenance = runtime.binary_provenance()
            return result
        except (
            OSError,
            RuntimeError,
            TimeoutError,
            subprocess.SubprocessError,
        ) as error:
            if runtime is not None and not binary_provenance:
                binary_provenance = runtime.binary_provenance()
            if not arguments.allow_workload_failures:
                raise
            if isinstance(error, WorkloadFailure):
                return error.result
            return {
                "status": "failed",
                "error": f"{type(error).__name__}: {error}",
            }
        finally:
            if runtime is not None:
                runtime.close()

    operations: dict[str, Callable[[MuxRuntime, int], dict[str, Any]]] = {
        "warm_scroll": warm_scroll,
        "attach_to_visible": attach_to_visible,
        "interactive_under_output": interactive_under_output,
        "interactive_open_loop": interactive_open_loop,
        "tui_redraw": tui_redraw,
        "tui_wheel_burst": tui_wheel_burst,
        "session_switch": session_switch,
        "tab_switch": tab_switch,
        "new_pane": new_pane,
        "new_tab": new_tab,
        "resize_reflow": resize_reflow,
        "idle_resources": idle_resources,
        "blocked_pty": blocked_pty,
        "blocked_client": blocked_client,
        "extension_isolation": extension_isolation,
        "component_resources": component_resources,
        "history_resources": history_resources,
        "lifecycle_churn": lifecycle_churn,
    }

    def classify_failure(workload: str, result: dict[str, Any]) -> dict[str, Any]:
        if result.get("status") != "failed":
            return result
        error = result.get("error")
        reviewed = expected_failure(
            manifest,
            arguments.multiplexer,
            workload,
            error if isinstance(error, str) else "",
        )
        if reviewed is not None:
            result["failure_expected"] = True
            result["failure_classification"] = reviewed["classification"]
        return result

    selected = [
        (scenario, operations[scenario["id"]]) for scenario in selected_scenarios
    ]
    for scenario, operation in selected:
        name = scenario["id"]
        if arguments.multiplexer not in scenario["subjects"]:
            workloads[name] = unsupported_result(scenario, arguments.multiplexer)
            continue
        if name != "attach_to_visible":
            workloads[name] = classify_failure(name, run_operation(operation))
            continue

        attach_samples: list[int] = []
        attach_bytes: list[int] = []
        attach_cpu: list[dict[str, Any]] = []
        resources_after_workload: dict[str, Any] | None = None
        attach_failure: dict[str, Any] | None = None
        for repetition in range(arguments.repetitions):
            result = run_operation(operation, 1)
            if result.get("status") != "completed":
                attach_failure = {
                    **result,
                    "completed_repetitions": repetition,
                }
                break
            attach_samples.extend(result["samples_ns"])
            attach_bytes.extend(result["outer_bytes"])
            attach_cpu.append(result["workload_cpu"])
            resources_after_workload = result.get("resources_after_workload")
        if attach_failure is not None:
            workloads[name] = classify_failure(name, attach_failure)
        else:
            workloads[name] = {
                "status": "completed",
                "observer": "native_poll",
                "clock": "steady_clock",
                **summary(attach_samples),
                "outer_bytes": attach_bytes,
                "median_outer_bytes": percentile(attach_bytes, 0.50),
                "workload_cpu_samples": attach_cpu,
                "resources_after_workload": resources_after_workload,
            }

    if arguments.mode in ("profiles", "all") and arguments.multiplexer in {
        "lemma",
        "tmux",
        "herdr",
        "zellij",
    }:
        profile_suite = manifest["profile_suites"][arguments.intent]
        profile_definitions = {
            profile["id"]: int(profile["panes"])
            for profile in manifest["pane_profiles"]
        }
        for profile in profile_suite:
            panes = profile_definitions[profile]
            pane_profiles[profile] = {}
            for activity in (False, True):
                key = "active" if activity else "idle"

                def profile_operation(
                    runtime: MuxRuntime,
                    repetitions: int,
                    *,
                    profile_id: str = profile,
                    pane_count: int = panes,
                    active: bool = activity,
                ) -> dict[str, Any]:
                    if not isinstance(
                        runtime,
                        (LemmaRuntime, TmuxRuntime, HerdrRuntime, ZellijRuntime),
                    ):
                        raise TypeError("pane profile requires a multiplexer runtime")
                    return pane_profile(
                        runtime, profile_id, pane_count, active, repetitions
                    )

                pane_profiles[profile][key] = run_operation(profile_operation)

    if (
        arguments.mode in ("session-profiles", "all")
        and arguments.multiplexer != "direct"
    ):
        session_suite = manifest["session_profile_suites"][arguments.intent]
        session_definitions = {
            profile["id"]: int(profile["sessions"])
            for profile in manifest["session_profiles"]
        }
        for profile in session_suite:
            sessions = session_definitions[profile]

            def session_profile_operation(
                runtime: MuxRuntime,
                repetitions: int,
                *,
                session_count: int = sessions,
            ) -> dict[str, Any]:
                return session_profile(runtime, session_count, repetitions)

            session_profiles[profile] = run_operation(session_profile_operation)

    if (
        arguments.mode in ("workspace-profiles", "all")
        and arguments.multiplexer != "direct"
    ):
        workspace_suite = manifest["workspace_profile_suites"][arguments.intent]
        workspace_definitions = {
            profile["id"]: int(profile["workspaces"])
            for profile in manifest["workspace_profiles"]
        }
        for profile in workspace_suite:
            workspaces = workspace_definitions[profile]

            def workspace_profile_operation(
                runtime: MuxRuntime,
                repetitions: int,
                *,
                workspace_count: int = workspaces,
            ) -> dict[str, Any]:
                return workspace_profile(runtime, workspace_count, repetitions)

            workspace_profiles[profile] = run_operation(workspace_profile_operation)

    commit, worktree_dirty, worktree_diff_sha256 = git_provenance()
    host_load_average = list(os.getloadavg())
    maximum_gate_load = manifest["regression_budgets"]["scope"][
        "maximum_load_average_1m"
    ]
    report = {
        "schema": 5,
        "suite": "core-mux-baseline",
        "run_intent": arguments.intent,
        "generated_at": datetime.now(UTC).isoformat(),
        "manifest": {
            "path": str(arguments.manifest.resolve()),
            "schema": manifest["schema"],
            "sha256": hashlib.sha256(arguments.manifest.read_bytes()).hexdigest(),
        },
        "scenario_ids": [scenario["id"] for scenario in selected_scenarios],
        "statistics_valid": {
            "p50": True,
            "p95": arguments.repetitions >= 20,
            "p99": arguments.repetitions >= 100,
        },
        "environment_valid": host_load_average[0] <= maximum_gate_load,
        "maximum_gate_load_average_1m": maximum_gate_load,
        "multiplexer": arguments.multiplexer,
        "multiplexer_version": runtime_version,
        "extension_fixture": arguments.extension_fixture,
        "extension_fixture_processes": arguments.extension_fixture_processes,
        "commit": commit,
        "worktree_dirty": worktree_dirty,
        "worktree_diff_sha256": worktree_diff_sha256,
        "host": platform.node(),
        "host_fingerprint": host_fingerprint(),
        "system": platform.system(),
        "system_release": platform.release(),
        "architecture": platform.machine(),
        "python": platform.python_version(),
        "host_load_average": host_load_average,
        "terminal": manifest["terminal"],
        "repetitions": arguments.repetitions,
        "binaries": binary_provenance,
        "build_profile": build_profile,
        "latency_trace": latency_trace_metadata(arguments.trace_directory),
        "private_attach_framing": (
            {
                "supported_versions": [
                    f"{ATTACH_PROTOCOL_MAJOR}.{minor}"
                    for minor in ATTACH_SUPPORTED_MINORS
                ],
                "envelope_bytes_per_message": ATTACH_HEADER_BYTES,
                "render_generation_bytes_per_frame": 4,
                "render_wire_overhead_bytes_per_frame": ATTACH_HEADER_BYTES + 4,
                "outer_bytes_metric_excludes_private_framing": True,
            }
            if arguments.multiplexer == "lemma"
            else None
        ),
        "workloads": workloads,
        "pane_profiles": pane_profiles,
        "session_profiles": session_profiles,
        "workspace_profiles": workspace_profiles,
    }

    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
