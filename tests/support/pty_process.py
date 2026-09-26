"""Reusable bounded outer-PTY and ANSI-screen support for mux tests and benchmarks."""

from __future__ import annotations

import errno
import fcntl
import os
import pty
import select
import signal
import struct
import termios
import threading
import time
from collections import deque
from types import TracebackType
from typing import Any, Self

FINAL_PTY_OUTPUT_BYTES = 64 * 1024


class AnsiScreenTracker:
    """Track the bounded ASCII fixture surface used for visible completion markers."""

    def __init__(self, columns: int, rows: int) -> None:
        self.columns = columns
        self.rows = rows
        self.cells = [bytearray(b" " * columns) for _ in range(rows)]
        self.row = 0
        self.column = 0
        self.saved = (0, 0)
        # Inclusive zero-based DECSTBM margins.
        self.top = 0
        self.bottom = rows - 1
        self.observed_fixture_rows: deque[bytes] = deque(maxlen=64)
        self.state = "ground"
        self.csi = bytearray()
        self.synchronized_text: str | None = None

    def resize(self, columns: int, rows: int) -> None:
        resized = [bytearray(b" " * columns) for _ in range(rows)]
        for row in range(min(rows, self.rows)):
            resized[row][: min(columns, self.columns)] = self.cells[row][
                : min(columns, self.columns)
            ]
        self.columns = columns
        self.rows = rows
        self.cells = resized
        self.row = min(self.row, rows - 1)
        self.column = min(self.column, columns - 1)
        self.top = 0
        self.bottom = rows - 1

    def _scroll(self, amount: int) -> None:
        """Scroll the margin region up by a positive amount or down by a negative one."""
        height = self.bottom - self.top + 1
        amount = max(-height, min(height, amount))
        region = self.cells[self.top : self.bottom + 1]
        blank = [bytearray(b" " * self.columns) for _ in range(abs(amount))]
        region = region[amount:] + blank if amount > 0 else blank + region[:amount]
        self.cells[self.top : self.bottom + 1] = region

    def _line_feed(self) -> None:
        if self.row == self.bottom:
            self._scroll(1)
        elif self.row + 1 < self.rows:
            self.row += 1

    @staticmethod
    def _parameters(encoded: bytes) -> list[int]:
        text = encoded.decode("ascii", errors="ignore").lstrip("?<=>")
        values = []
        for part in text.split(";"):
            try:
                values.append(int(part) if part else 0)
            except ValueError:
                values.append(0)
        return values or [0]

    def _erase_display(self, mode: int) -> None:
        if mode in (2, 3):
            self.cells = [bytearray(b" " * self.columns) for _ in range(self.rows)]
        elif mode == 0:
            self.cells[self.row][self.column :] = b" " * (self.columns - self.column)
            for row in range(self.row + 1, self.rows):
                self.cells[row][:] = b" " * self.columns
        elif mode == 1:
            for row in range(self.row):
                self.cells[row][:] = b" " * self.columns
            self.cells[self.row][: self.column + 1] = b" " * (self.column + 1)

    def _erase_line(self, mode: int) -> None:
        if mode == 0:
            self.cells[self.row][self.column :] = b" " * (self.columns - self.column)
        elif mode == 1:
            self.cells[self.row][: self.column + 1] = b" " * (self.column + 1)
        elif mode == 2:
            self.cells[self.row][:] = b" " * self.columns

    def _dispatch_csi(self, final: int) -> None:
        parameters = self._parameters(bytes(self.csi))
        first = parameters[0] or 1
        if final in (ord("H"), ord("f")):
            row = parameters[0] if parameters[0] else 1
            column = parameters[1] if len(parameters) > 1 and parameters[1] else 1
            self.row = max(0, min(self.rows - 1, row - 1))
            self.column = max(0, min(self.columns - 1, column - 1))
        elif final == ord("A"):
            self.row = max(0, self.row - first)
        elif final in (ord("B"), ord("e")):
            self.row = min(self.rows - 1, self.row + first)
        elif final in (ord("C"), ord("a")):
            self.column = min(self.columns - 1, self.column + first)
        elif final == ord("D"):
            self.column = max(0, self.column - first)
        elif final in (ord("G"), ord("`")):
            self.column = max(0, min(self.columns - 1, first - 1))
        elif final == ord("d"):
            self.row = max(0, min(self.rows - 1, first - 1))
        elif final == ord("J"):
            self._erase_display(parameters[0])
        elif final == ord("K"):
            self._erase_line(parameters[0])
        elif final == ord("X"):
            end = min(self.columns, self.column + first)
            self.cells[self.row][self.column : end] = b" " * (end - self.column)
        elif final in (ord("h"), ord("l")) and self.csi.startswith(b"?"):
            if 2026 in parameters:
                if final == ord("h") and self.synchronized_text is None:
                    self.synchronized_text = self.text()
                elif final == ord("l"):
                    self.synchronized_text = None
        elif final == ord("S") and not self.csi.startswith(b"?"):
            self._scroll(first)
        elif final == ord("T") and not self.csi.startswith(b"?"):
            self._scroll(-first)
        elif final == ord("r") and not self.csi.startswith(b"?"):
            top = (parameters[0] or 1) - 1
            bottom = (
                parameters[1] if len(parameters) > 1 and parameters[1] else self.rows
            ) - 1
            if 0 <= top < bottom < self.rows:
                self.top, self.bottom = top, bottom
                self.row, self.column = 0, 0
        elif final == ord("s"):
            self.saved = (self.row, self.column)
        elif final == ord("u"):
            self.row, self.column = self.saved

    def _feed(self, data: bytes, observe_marker: bytes | None) -> bool:
        observed = False
        for value in data:
            if self.state == "ground":
                if value == 0x1B:
                    self.state = "escape"
                elif value == 0x0D:
                    self.column = 0
                elif value == 0x0A:
                    self._line_feed()
                elif value == 0x08:
                    self.column = max(0, self.column - 1)
                elif value == 0x09:
                    self.column = min(self.columns - 1, ((self.column // 8) + 1) * 8)
                elif 0x20 <= value <= 0x7E:
                    self.cells[self.row][self.column] = value
                    current_row = self.cells[self.row]
                    if value == ord("_") and b"__LEMMA_" in current_row:
                        self.observed_fixture_rows.append(bytes(current_row))
                    if observe_marker is not None and observe_marker in current_row:
                        observed = True
                    if self.column + 1 < self.columns:
                        self.column += 1
                continue
            if self.state == "escape":
                if value == ord("["):
                    self.csi.clear()
                    self.state = "csi"
                elif value == ord("]"):
                    self.state = "osc"
                elif value in (ord("P"), ord("X"), ord("^"), ord("_")):
                    self.state = "string"
                elif value == ord("7"):
                    self.saved = (self.row, self.column)
                    self.state = "ground"
                elif value == ord("8"):
                    self.row, self.column = self.saved
                    self.state = "ground"
                elif 0x20 <= value <= 0x2F:
                    self.state = "escape_intermediate"
                else:
                    self.state = "ground"
                continue
            if self.state == "escape_intermediate":
                if 0x30 <= value <= 0x7E:
                    self.state = "ground"
                continue
            if self.state == "csi":
                if 0x40 <= value <= 0x7E:
                    self._dispatch_csi(value)
                    self.state = "ground"
                elif len(self.csi) < 128:
                    self.csi.append(value)
                continue
            if self.state in ("osc", "string"):
                if value == 0x07 and self.state == "osc":
                    self.state = "ground"
                elif value == 0x1B:
                    self.state = "string_escape"
                continue
            if self.state == "string_escape":
                self.state = "ground" if value == ord("\\") else "string"
        return observed

    def feed(self, data: bytes) -> None:
        self._feed(data, None)

    def contains(self, marker: bytes) -> bool:
        return any(marker in row for row in self.observed_fixture_rows) or any(
            marker in row for row in self.cells
        )

    def feed_observing(self, data: bytes, marker: bytes) -> bool:
        return self._feed(data, marker if marker else None)

    def text(self) -> str:
        # UI assertions observe presented frames, never a partial clear/redraw. Marker
        # observation stays separate so fixture completion can retain transient output.
        if self.synchronized_text is not None:
            return self.synchronized_text
        return "\n".join(
            bytes(row).decode("ascii", errors="replace").rstrip() for row in self.cells
        )


class PtyOutputMonitor:
    """Own a PTY's reads until scope exit, retaining a completion or failure outcome.

    The caller may write to the process or work with other clients, but must not
    read, drain, resize, or close this process while the monitor is active. Output
    keeps flowing even after the outcome is known; retention remains bounded.
    This observes completion only, not a measured latency endpoint.
    """

    def __init__(
        self,
        process: PtyProcess,
        marker: bytes,
        *,
        failure_markers: tuple[bytes, ...] = (),
        visible_text: bool = False,
    ) -> None:
        self.process = process
        self.marker = marker
        self.failure_markers = failure_markers
        self.visible_text = visible_text
        self._stop = threading.Event()
        self._finished = threading.Event()
        self._error: Exception | None = None
        self._thread = threading.Thread(target=self._run, name="pty-output-monitor")

    def __enter__(self) -> Self:
        self._thread.start()
        return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self._stop.set()
        # The only blocking operation in the reader is a bounded select; join
        # before returning PTY/screen ownership, including on a writer failure.
        self._thread.join()
        if exception_type is None:
            self.check()

    def check(self) -> None:
        if self._finished.is_set() and self._error is not None:
            raise self._error

    def wait(self, timeout: float) -> None:
        if not self._finished.wait(timeout):
            raise TimeoutError(
                f"did not observe {self.marker!r}; tail={self.process.output_tail[-4096:]!r}"
            )
        self.check()

    def _finish(self, error: Exception | None = None) -> None:
        if not self._finished.is_set():
            self._error = error
            self._finished.set()

    def _run(self) -> None:
        retained = b""
        retained_limit = FINAL_PTY_OUTPUT_BYTES + max(
            len(marker) for marker in (self.marker, *self.failure_markers)
        )
        try:
            while not self._stop.is_set():
                from_pending = bool(self.process.pending_read)
                if from_pending:
                    data = self.process.pending_read
                    self.process.pending_read = b""
                else:
                    readable, _, _ = select.select(
                        [self.process.descriptor], [], [], 0.02
                    )
                    if not readable:
                        continue
                    try:
                        data = os.read(self.process.descriptor, FINAL_PTY_OUTPUT_BYTES)
                    except BlockingIOError:
                        continue
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                        data = b""
                    if not data:
                        self._finish(
                            RuntimeError(
                                f"PTY closed before {self.marker!r}; "
                                f"tail={self.process.output_tail[-4096:]!r}"
                            )
                        )
                        return
                retained = (retained + data)[-retained_limit:]
                observed = False
                if not from_pending:
                    observed = self.process.screen.feed_observing(
                        data, self.marker if self.visible_text else b""
                    )
                    self.process._retain_output(data)
                for marker in self.failure_markers:
                    if marker in retained:
                        self._finish(
                            RuntimeError(
                                f"observed failure {marker!r} while waiting for {self.marker!r}; "
                                f"tail={retained[-512:]!r}"
                            )
                        )
                        break
                if (
                    self.marker in retained
                    or observed
                    or (self.visible_text and self.process.screen.contains(self.marker))
                ):
                    self._finish()
        except Exception as error:
            self._finish(error)


class PtyProcess:
    def __init__(
        self,
        arguments: list[str],
        environment: dict[str, str],
        *,
        terminal_restore_sequence: bytes | tuple[bytes, ...] | None = None,
    ) -> None:
        release_read, release_write = os.pipe()
        try:
            ready_read, ready_write = os.pipe()
        except BaseException:
            os.close(release_read)
            os.close(release_write)
            raise
        try:
            pid, descriptor = pty.fork()
        except BaseException:
            os.close(release_read)
            os.close(release_write)
            os.close(ready_read)
            os.close(ready_write)
            raise
        if pid == 0:
            os.close(release_write)
            os.close(ready_read)
            os.write(ready_write, b"\0")
            os.close(ready_write)
            try:
                released = os.read(release_read, 1)
            finally:
                os.close(release_read)
            if released != b"\0":
                os._exit(127)
            os.execve(arguments[0], arguments, environment)

        os.close(release_read)
        os.close(ready_write)
        self.pid = pid
        self.descriptor = descriptor
        self.pending_read = b""
        self.screen = AnsiScreenTracker(80, 24)
        self.initial_terminal_attributes: list[Any] | None = None
        self.terminal_restore_sequence = terminal_restore_sequence
        self.final_output = b""
        self.output_tail = b""
        self.terminal_modes_restored: bool | None = None
        self.terminal_state_restored: bool | None = None
        try:
            # forkpty does not set close-on-exec. A later client must not keep this
            # master alive and prevent the first client's terminal teardown.
            os.set_inheritable(descriptor, False)
            fcntl.ioctl(
                descriptor, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0)
            )
            self.initial_terminal_attributes = termios.tcgetattr(descriptor)
            flags = fcntl.fcntl(descriptor, fcntl.F_GETFL)
            fcntl.fcntl(descriptor, fcntl.F_SETFL, flags | os.O_NONBLOCK)
            # The parent can return from forkpty before the child creates its
            # process group. Wait for it before callers can send group signals.
            readable, _, _ = select.select([ready_read], [], [], 5.0)
            if not readable:
                raise TimeoutError("PTY child did not initialize its process group")
            if os.read(ready_read, 1) != b"\0":
                raise RuntimeError("PTY child exited before initialization")
            os.write(release_write, b"\0")
        except BaseException:
            os.close(release_write)
            os.close(ready_read)
            self.close()
            raise
        os.close(release_write)
        os.close(ready_read)

    @property
    def running(self) -> bool:
        return self.pid > 0

    def _retain_output(self, data: bytes) -> None:
        self.output_tail = (self.output_tail + data)[-FINAL_PTY_OUTPUT_BYTES:]

    def wait_for_exit(self, timeout: float) -> None:
        if self.pid <= 0:
            return
        self.final_output = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            # Detaching clients restore terminal modes before exit. Keep consuming and retaining
            # the final PTY output so cleanup cannot block and escape-controlled modes are checked.
            self.drain(
                min(0.01, max(0.0, deadline - time.monotonic())),
                retain_final_output=True,
            )
            try:
                waited, status = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError as error:
                raise RuntimeError("PTY child was reaped unexpectedly") from error
            if waited == self.pid:
                # Consume bytes queued immediately before exit before inspecting the cleanup tail.
                self.drain(0.05, retain_final_output=True)
                try:
                    current_attributes = termios.tcgetattr(self.descriptor)
                    attributes_restored = (
                        self.initial_terminal_attributes is not None
                        and current_attributes == self.initial_terminal_attributes
                    )
                except termios.error:
                    attributes_restored = False
                if self.terminal_restore_sequence is None:
                    self.terminal_modes_restored = None
                    self.terminal_state_restored = attributes_restored
                else:
                    sequences = (
                        (self.terminal_restore_sequence,)
                        if isinstance(self.terminal_restore_sequence, bytes)
                        else self.terminal_restore_sequence
                    )
                    self.terminal_modes_restored = any(
                        sequence in self.final_output for sequence in sequences
                    )
                    self.terminal_state_restored = (
                        attributes_restored and self.terminal_modes_restored is True
                    )
                try:
                    os.close(self.descriptor)
                except OSError:
                    pass
                self.descriptor = -1
                self.pid = -1
                if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                    raise RuntimeError(
                        f"PTY child exited unsuccessfully: status={status}"
                    )
                return
            time.sleep(0.005)
        raise TimeoutError("PTY child did not exit after detach")

    def close(self) -> None:
        if self.pid <= 0:
            return
        try:
            os.close(self.descriptor)
        except OSError:
            pass
        self.descriptor = -1
        try:
            os.kill(self.pid, signal.SIGHUP)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            try:
                waited, _ = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                waited = self.pid
            if waited == self.pid:
                self.pid = -1
                return
            time.sleep(0.01)
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            try:
                waited, _ = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                waited = self.pid
            if waited == self.pid:
                self.pid = -1
                return
            time.sleep(0.01)
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass
        self.pid = -1

    def write_all(self, data: bytes, timeout: float) -> None:
        offset = 0
        deadline = time.monotonic() + timeout
        while offset < len(data):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"PTY write timed out after {offset}/{len(data)} bytes"
                )
            try:
                offset += os.write(self.descriptor, data[offset:])
            except BlockingIOError:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(
                        f"PTY write timed out after {offset}/{len(data)} bytes"
                    )
                select.select([], [self.descriptor], [], min(remaining, 0.02))
                continue
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"PTY write timed out after {offset}/{len(data)} bytes"
                )

    def resize(self, columns: int, rows: int) -> None:
        if not 0 < columns <= 500 or not 0 < rows <= 200 or self.pid <= 0:
            raise ValueError("PTY resize is outside the private attach bounds")
        encoded = struct.pack("HHHH", rows, columns, 0, 0)
        fcntl.ioctl(self.descriptor, termios.TIOCSWINSZ, encoded)
        actual = fcntl.ioctl(
            self.descriptor, termios.TIOCGWINSZ, struct.pack("HHHH", 0, 0, 0, 0)
        )
        actual_rows, actual_columns, _, _ = struct.unpack("HHHH", actual)
        if (actual_columns, actual_rows) != (columns, rows):
            raise RuntimeError("outer PTY did not retain the requested dimensions")
        self.screen.resize(columns, rows)
        try:
            os.killpg(self.pid, signal.SIGWINCH)
        except ProcessLookupError:
            raise RuntimeError("attached client exited during resize") from None

    def fill_until_stalled(self, data: bytes, stall_seconds: float = 0.25) -> int:
        offset = 0
        last_progress = time.monotonic()
        deadline = last_progress + 5.0
        while offset < len(data) and time.monotonic() < deadline:
            try:
                written = os.write(self.descriptor, data[offset:])
            except BlockingIOError:
                written = 0
            if written:
                offset += written
                last_progress = time.monotonic()
            elif time.monotonic() - last_progress >= stall_seconds:
                break
            else:
                time.sleep(0.001)
        return offset

    def read_until(
        self,
        marker: bytes,
        timeout: float,
        *,
        started_ns: int | None = None,
        failure_markers: tuple[bytes, ...] = (),
        preserve_suffix: bool = False,
        visible_text: bool = False,
    ) -> tuple[int, int]:
        if started_ns is None:
            started_ns = time.monotonic_ns()
        deadline = time.monotonic() + timeout
        total = 0
        retained = b""
        while time.monotonic() < deadline:
            from_pending = bool(self.pending_read)
            if from_pending:
                data = self.pending_read
                self.pending_read = b""
            else:
                remaining = deadline - time.monotonic()
                readable, _, _ = select.select(
                    [self.descriptor], [], [], min(remaining, 0.02)
                )
                if not readable:
                    continue
                try:
                    data = os.read(self.descriptor, 64 * 1024)
                except BlockingIOError:
                    continue
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
            total += len(data)
            retained = (retained + data)[-(len(marker) + 64 * 1024) :]
            visible_marker_observed = False
            if not from_pending:
                if visible_text:
                    visible_marker_observed = self.screen.feed_observing(data, marker)
                else:
                    self.screen.feed(data)
                self._retain_output(data)
            marker_offset = retained.find(marker)
            if (
                marker_offset >= 0
                or visible_marker_observed
                or (visible_text and self.screen.contains(marker))
            ):
                suffix = (
                    retained[marker_offset + len(marker) :]
                    if marker_offset >= 0
                    else b""
                )
                if preserve_suffix:
                    self.pending_read = suffix
                    total -= len(suffix)
                return time.monotonic_ns() - started_ns, total
            for failure_marker in failure_markers:
                if failure_marker in retained:
                    raise RuntimeError(
                        f"observed failure {failure_marker!r} while waiting for {marker!r}; "
                        f"tail={retained[-512:]!r}"
                    )
        raise TimeoutError(f"did not observe {marker!r}; tail={retained[-4096:]!r}")

    def drain(
        self, duration: float = 0.05, *, retain_final_output: bool = False
    ) -> int:
        deadline = time.monotonic() + duration
        total = len(self.pending_read)
        if retain_final_output and self.pending_read:
            self.final_output = (self.final_output + self.pending_read)[
                -FINAL_PTY_OUTPUT_BYTES:
            ]
        self.pending_read = b""
        while time.monotonic() < deadline:
            readable, _, _ = select.select([self.descriptor], [], [], 0.005)
            if not readable:
                continue
            try:
                data = os.read(self.descriptor, 64 * 1024)
            except BlockingIOError:
                continue
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            if not data:
                break
            total += len(data)
            self.screen.feed(data)
            self._retain_output(data)
            if retain_final_output:
                self.final_output = (self.final_output + data)[-FINAL_PTY_OUTPUT_BYTES:]
        return total
