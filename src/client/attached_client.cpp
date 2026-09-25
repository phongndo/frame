#include "client/attached_client.hpp"

#include "client/host_input_parser.hpp"
#include "client/host_terminal_theme.hpp"
#include "daemon/server.hpp"
#include "diagnostic/latency_trace.hpp"
#include "platform/io.hpp"
#include "platform/terminal_mode.hpp"
#include "protocol/attachment.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

namespace lemma::client {
namespace {

constexpr auto host_input_flush_delay = std::chrono::milliseconds(50);
// Recognized OSC records are transport, not ambiguous Escape keys. Partial progress cannot renew
// this deadline; exhaustion still fails closed instead of reinterpreting clipboard bytes as input.
constexpr auto host_terminal_reply_timeout = std::chrono::seconds(30);
// Window managers emit SIGWINCH repeatedly during one physical resize gesture. A trailing-edge
// commit prevents those samples from becoming a stream of child PTY resizes and shell redraws.
constexpr auto outer_resize_quiet_delay = std::chrono::milliseconds(50);
// XTWINOPS 22;2 / 23;2 save and restore the user's window title around Lemma's OSC 2 titles.
// Do not request Kitty's "report all keys" flag: several outer terminals accept that flag but
// omit associated text, which turns ordinary printable input into semantically incomplete events.
// Disambiguation, event types, alternate keys, and associated text preserve metadata where it is
// available while ordinary layout/IME text remains ordinary bytes.
constexpr std::string_view outer_terminal_enter =
    "\x1B[?1049h\x1B[2J\x1B[H\x1B[?2004h\x1B[?1004h\x1B[?1002h\x1B[?1006h\x1B[>23u"
    "\x1B[?2048s\x1B[?2048h\x1B[?5522$p\x1B[22;2t";
constexpr std::string_view outer_terminal_restore =
    "\x18\x1B_Gq=2,m=0;\x1B\\\x1B_Ga=d,d=A,q=2\x1B\\"
    "\x1B[0m\x1B[?2026l\x1B[?1l\x1B[?9l\x1B[?1000l\x1B[?1002l\x1B[?1003l"
    "\x1B[?1004l\x1B[?1005l\x1B[?1006l\x1B[?1007l\x1B[?1015l\x1B[?1016l"
    "\x1B[?2004l\x1B]112\x1B\\\x1B[0 q\x1B[?25h\x1B[?7h\x1B[<u\x1B[?2048r\x1B[23;2t\x1B[?1049l";
constexpr std::string_view interruption_diagnostic = "lemma attach interrupted by signal\n";
constexpr auto signal_cleanup_storage = [] {
  std::array<char, outer_terminal_restore.size() + interruption_diagnostic.size()> payload{};
  std::size_t offset = 0;
  for (const char character : outer_terminal_restore) {
    payload.at(offset) = character;
    ++offset;
  }
  for (const char character : interruption_diagnostic) {
    payload.at(offset) = character;
    ++offset;
  }
  return payload;
}();
constexpr std::string_view signal_cleanup_payload{signal_cleanup_storage.data(),
                                                  signal_cleanup_storage.size()};
constexpr std::array handled_termination_signals{SIGINT, SIGTERM, SIGHUP, SIGQUIT};
using platform::close_descriptor;

volatile sig_atomic_t resize_pending = 0;
volatile sig_atomic_t resize_wakeup_descriptor = -1;
volatile sig_atomic_t termination_signal = 0;
volatile sig_atomic_t termination_wakeup_descriptor = -1;
volatile sig_atomic_t termination_wakeup_read_descriptor = -1;
volatile sig_atomic_t terminal_restore_wakeup_descriptor = -1;
volatile sig_atomic_t termination_render_descriptor = -1;
volatile sig_atomic_t termination_render_closed = 0;

[[nodiscard]] auto open_terminal_output(const char* const path) noexcept -> int {
  // open is variadic because its third argument is present only with O_CREAT.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return ::open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
}

[[nodiscard]] auto write_interruptibly(const int descriptor,
                                       const std::span<const std::byte> bytes) noexcept -> bool {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (termination_signal != 0) {
      return false;
    }
    const auto written = ::write(descriptor, bytes.subspan(offset).data(), bytes.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto write_text_interruptibly(const int descriptor,
                                            const std::string_view text) noexcept -> bool {
  return write_interruptibly(descriptor, std::as_bytes(std::span(text.data(), text.size())));
}

// Terminal progress, signal wakeup, and bounded retry outcomes are deliberately handled together.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto write_terminal_interruptibly(const int descriptor,
                                                const std::span<const std::byte> bytes) noexcept
    -> bool {
  std::size_t offset = 0;
  constexpr auto progress_timeout = std::chrono::seconds(2);
  auto progress_deadline = std::chrono::steady_clock::now() + progress_timeout;
  while (offset < bytes.size()) {
    if (termination_signal != 0) {
      return false;
    }
    // Keep the tty below its non-writable high-water mark. On Darwin, filling a PTY all the way to
    // EAGAIN can also make an immediate termios ioctl fail until the master consumes output. Small
    // writes followed by a readiness check preserve kernel headroom for terminal control cleanup.
    constexpr std::size_t terminal_write_chunk = 1'024;
    const auto remaining = bytes.subspan(offset);
    const auto chunk = remaining.first(std::min(remaining.size(), terminal_write_chunk));
    const auto written = ::write(descriptor, chunk.data(), chunk.size());
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      progress_deadline = std::chrono::steady_clock::now() + progress_timeout;
      // Observe signal wakeup without adding a presentation latency floor. A later saturated write
      // is nonblocking and enters the bounded readiness path below.
      pollfd wakeup{.fd = termination_wakeup_read_descriptor, .events = POLLIN, .revents = 0};
      if (::poll(&wakeup, 1, 0) > 0 && (wakeup.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        return false;
      }
      pollfd writable{.fd = descriptor, .events = POLLOUT, .revents = 0};
      if (::poll(&writable, 1, 0) > 0 && (writable.revents & POLLOUT) != 0) {
        continue;
      }
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EIO) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= progress_deadline) {
      return false;
    }
    std::array<pollfd, 2> readiness{{
        {.fd = descriptor, .events = POLLOUT, .revents = 0},
        {.fd = termination_wakeup_read_descriptor, .events = POLLIN, .revents = 0},
    }};
    constexpr auto terminal_retry = std::chrono::milliseconds(10);
    const auto remaining_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(progress_deadline - now);
    const auto retry =
        std::max(std::min(remaining_time, terminal_retry), std::chrono::milliseconds(1));
    const auto ready = ::poll(readiness.data(), readiness.size(), static_cast<int>(retry.count()));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if ((readiness.back().revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      return false;
    }
    if ((readiness.front().revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 &&
        (readiness.front().revents & POLLOUT) == 0) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto write_terminal_text_interruptibly(const int descriptor,
                                                     const std::string_view text) noexcept -> bool {
  return write_terminal_interruptibly(descriptor,
                                      std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto send_interruptibly(const int socket,
                                      const std::span<const std::byte> bytes) noexcept -> bool {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (termination_signal != 0) {
      return false;
    }
    const auto sent =
        ::send(socket, bytes.subspan(offset).data(), bytes.size() - offset, MSG_NOSIGNAL);
    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto write_text_until(const int descriptor, const std::string_view text,
                                    const std::chrono::steady_clock::time_point deadline) noexcept
    -> bool {
  const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    const auto written = ::write(descriptor, bytes.subspan(offset).data(), bytes.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    // A flow-controlled Darwin PTY can transiently report zero or EIO as well as EAGAIN while
    // its master is still open. Retain the suffix for every no-progress result until the bounded
    // deadline; an actually unusable descriptor therefore remains bounded too.
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    constexpr auto retry_delay = std::chrono::milliseconds(10);
    std::this_thread::sleep_for(
        std::max(std::min(remaining, retry_delay), std::chrono::milliseconds(1)));
  }
  return true;
}

[[nodiscard]] auto write_signal_cleanup(const int descriptor) noexcept -> bool {
  // RawTerminal has already restored termios before this function is called. Retain the client
  // until the queued payload reaches the terminal endpoint, so closing the final slave descriptor
  // cannot discard its restore suffix or diagnostic. The test/user may begin draining as soon as
  // termios is restored, and the single monotonic deadline keeps an abandoned endpoint bounded.
  constexpr auto output_timeout = std::chrono::seconds(10);
  const auto deadline = std::chrono::steady_clock::now() + output_timeout;
  if (!write_text_until(descriptor, signal_cleanup_payload, deadline)) {
    return false;
  }
  while (std::chrono::steady_clock::now() < deadline) {
    int queued = 0;
    // ioctl is variadic because its third argument depends on the request.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::ioctl(descriptor, TIOCOUTQ, &queued) == 0 && queued == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void on_window_changed([[maybe_unused]] const int signal_number) noexcept {
  const int saved_error = errno;
  resize_pending = 1;
  constexpr std::byte notification{0};
  const int wakeup = resize_wakeup_descriptor;
  if (wakeup >= 0) {
    static_cast<void>(::write(wakeup, &notification, sizeof(notification)));
  }
  errno = saved_error;
}

void on_termination(const int signal_number) noexcept {
  const int saved_error = errno;
  if (termination_signal == 0) {
    termination_signal = signal_number;
  }
  constexpr std::byte notification{0};
  const int wakeup = termination_wakeup_descriptor;
  if (wakeup >= 0) {
    static_cast<void>(::write(wakeup, &notification, sizeof(notification)));
  }
  const int render = termination_render_descriptor;
  if (render >= 0) {
    // close is async-signal-safe. The independent cleanup descriptor keeps this from becoming the
    // tty's last close, while retiring the saturated writer lets the emergency restorer progress.
    static_cast<void>(::close(render));
    termination_render_descriptor = -1;
    termination_render_closed = 1;
  }
  const int terminal_restore = terminal_restore_wakeup_descriptor;
  if (terminal_restore >= 0) {
    static_cast<void>(::write(terminal_restore, &notification, sizeof(notification)));
  }
  errno = saved_error;
}

enum class SignalWakeupRole : std::uint8_t {
  termination,
  resize,
};

class SignalWakeup final {
public:
  explicit SignalWakeup(const SignalWakeupRole role) noexcept : role_(role) {}
  SignalWakeup(const SignalWakeup&) = delete;
  auto operator=(const SignalWakeup&) -> SignalWakeup& = delete;
  SignalWakeup(SignalWakeup&&) = delete;
  auto operator=(SignalWakeup&&) -> SignalWakeup& = delete;
  ~SignalWakeup() {
    if (role_ == SignalWakeupRole::termination) {
      termination_wakeup_descriptor = -1;
      termination_wakeup_read_descriptor = -1;
    } else {
      resize_wakeup_descriptor = -1;
    }
    close_descriptor(read_descriptor_);
    close_descriptor(write_descriptor_);
  }

  [[nodiscard]] auto install() noexcept -> bool {
    std::array<int, 2> descriptors{};
    if (::pipe(descriptors.data()) != 0) {
      return false;
    }
    read_descriptor_ = descriptors.front();
    write_descriptor_ = descriptors.back();
    if (!platform::set_nonblocking(read_descriptor_) ||
        !platform::set_nonblocking(write_descriptor_)) {
      close_descriptor(read_descriptor_);
      close_descriptor(write_descriptor_);
      return false;
    }
    if (role_ == SignalWakeupRole::termination) {
      termination_wakeup_read_descriptor = read_descriptor_;
      termination_wakeup_descriptor = write_descriptor_;
    } else {
      resize_wakeup_descriptor = write_descriptor_;
    }
    return true;
  }

  [[nodiscard]] auto descriptor() const noexcept -> int { return read_descriptor_; }

  void drain() const noexcept {
    std::array<std::byte, 64> notifications{};
    constexpr std::size_t attempts_max = 16;
    for (std::size_t attempt = 0; attempt < attempts_max; ++attempt) {
      const auto received = ::read(read_descriptor_, notifications.data(), notifications.size());
      if (received > 0 || (received < 0 && errno == EINTR)) {
        continue;
      }
      return;
    }
  }

private:
  SignalWakeupRole role_;
  int read_descriptor_{-1};
  int write_descriptor_{-1};
};

class SignalActions final {
public:
  SignalActions() = default;
  SignalActions(const SignalActions&) = delete;
  auto operator=(const SignalActions&) -> SignalActions& = delete;
  SignalActions(SignalActions&&) = delete;
  auto operator=(SignalActions&&) -> SignalActions& = delete;
  ~SignalActions() { restore(); }

  [[nodiscard]] auto install() noexcept -> bool {
    if (installed_ != 0) {
      return false;
    }
    struct sigaction resize_action{};
    resize_action.sa_handler = &on_window_changed;
    if (sigemptyset(&resize_action.sa_mask) != 0 ||
        ::sigaction(SIGWINCH, &resize_action, &previous_resize_) != 0) {
      return false;
    }
    resize_installed_ = true;

    struct sigaction termination_action{};
    termination_action.sa_handler = &on_termination;
    static_cast<void>(sigemptyset(&termination_action.sa_mask));
    for (std::size_t index = 0; index < handled_termination_signals.size(); ++index) {
      const auto signal_number = std::span(handled_termination_signals).subspan(index, 1).front();
      auto& previous = std::span(previous_termination_).subspan(index, 1).front();
      if (::sigaction(signal_number, &termination_action, &previous) != 0) {
        restore();
        return false;
      }
      ++installed_;
    }
    return true;
  }

private:
  void restore() noexcept {
    while (installed_ > 0) {
      --installed_;
      const auto signal_number =
          std::span(handled_termination_signals).subspan(installed_, 1).front();
      auto& previous = std::span(previous_termination_).subspan(installed_, 1).front();
      static_cast<void>(::sigaction(signal_number, &previous, nullptr));
    }
    if (resize_installed_) {
      static_cast<void>(::sigaction(SIGWINCH, &previous_resize_, nullptr));
      resize_installed_ = false;
    }
  }

  struct sigaction previous_resize_{};
  std::array<struct sigaction, handled_termination_signals.size()> previous_termination_{};
  std::size_t installed_{0};
  bool resize_installed_{false};
};

class OuterTerminal final {
public:
  OuterTerminal() = default;
  OuterTerminal(const OuterTerminal&) = delete;
  auto operator=(const OuterTerminal&) -> OuterTerminal& = delete;
  OuterTerminal(OuterTerminal&&) = delete;
  auto operator=(OuterTerminal&&) -> OuterTerminal& = delete;
  ~OuterTerminal() {
    if (active_) {
      active_ = false;
      constexpr auto normal_cleanup_timeout = std::chrono::seconds(2);
      const bool normally_restored =
          termination_signal == 0 &&
          write_text_until(render_descriptor_, outer_terminal_restore,
                           std::chrono::steady_clock::now() + normal_cleanup_timeout);
      if (termination_signal != 0 && !signal_cleanup_suppressed_) {
        // Discard the interrupted render and close its description before queueing cleanup. Some
        // PTY implementations discard global output while tearing down an interrupted writer, so
        // closing it after the restore payload could erase the payload we just retained.
        retire_render_output();
        static_cast<void>(write_signal_cleanup(cleanup_descriptor_));
      } else if (termination_signal != 0) {
        abort_render_output();
      } else if (!normally_restored) {
        // A daemon-loss or EOF cleanup must not keep the process alive forever if the terminal has
        // stopped consuming output. Retire stale output and make one independent bounded attempt.
        retire_render_output();
        static_cast<void>(
            write_text_until(cleanup_descriptor_, outer_terminal_restore,
                             std::chrono::steady_clock::now() + normal_cleanup_timeout));
      }
    }
    close_descriptor(render_descriptor_);
    close_descriptor(cleanup_descriptor_);
  }

  [[nodiscard]] auto enter() noexcept -> bool {
    std::array<char, 1'024> terminal_path{};
    if (::ttyname_r(STDOUT_FILENO, terminal_path.data(), terminal_path.size()) != 0) {
      return false;
    }
    cleanup_descriptor_ = open_terminal_output(terminal_path.data());
    render_descriptor_ = open_terminal_output(terminal_path.data());
    if (cleanup_descriptor_ < 0 || render_descriptor_ < 0) {
      close_descriptor(render_descriptor_);
      close_descriptor(cleanup_descriptor_);
      return false;
    }
    active_ = true;
    return write_terminal_text_interruptibly(render_descriptor_, outer_terminal_enter);
  }

  [[nodiscard]] auto render_descriptor() const noexcept -> int { return render_descriptor_; }
  [[nodiscard]] auto cleanup_descriptor() const noexcept -> int { return cleanup_descriptor_; }

  void suppress_signal_cleanup() noexcept { signal_cleanup_suppressed_ = true; }

  void adopt_signal_render_retirement() noexcept {
    if (termination_render_closed != 0) {
      render_descriptor_ = -1;
      termination_render_closed = 0;
    }
  }

  void retire_render_writer() noexcept {
    // cleanup_descriptor_ keeps the tty open, so retiring the nonblocking render description does
    // not perform last-close terminal drainage.
    if (termination_render_descriptor == render_descriptor_) {
      termination_render_descriptor = -1;
    }
    close_descriptor(render_descriptor_);
  }

  void retire_render_output() noexcept {
    // Close before flushing: on Darwin, discarding a saturated per-writer queue may not take effect
    // until that writer has been retired.
    retire_render_writer();
    abort_render_output();
  }

  void abort_render_output() const noexcept {
    // Flush the shared queue while keeping every operation non-draining. Callers choose when it is
    // safe to retire the render description; signal teardown keeps an independent cleanup endpoint
    // open so that retirement is not a last-close drainage boundary.
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    static_cast<void>(sigemptyset(&ignored.sa_mask));
    struct sigaction previous{};
    const bool action_changed = ::sigaction(SIGTTOU, &ignored, &previous) == 0;
    // TCOON itself can wait for room to enqueue a flow-control event on a saturated Darwin PTY,
    // defeating this non-draining cleanup boundary. Discard queued output directly through the
    // dedicated nonblocking descriptions; never fall back to the possibly blocking stdout handle.
    constexpr std::size_t flush_attempts_max = 100;
    for (std::size_t attempt = 0; attempt < flush_attempts_max; ++attempt) {
      if (::tcflush(render_descriptor_, TCOFLUSH) == 0 ||
          ::tcflush(cleanup_descriptor_, TCOFLUSH) == 0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (action_changed) {
      static_cast<void>(::sigaction(SIGTTOU, &previous, nullptr));
    }
  }

private:
  int cleanup_descriptor_{-1};
  int render_descriptor_{-1};
  bool active_{false};
  bool signal_cleanup_suppressed_{false};
};

[[nodiscard]] auto terminal_size() noexcept -> platform::WindowSize {
  return platform::terminal_size(STDOUT_FILENO, protocol::columns_max, protocol::rows_max);
}

[[nodiscard]] auto send_host_terminal_theme_query(const int descriptor) noexcept -> bool {
  std::array<char, host_theme_query_bytes_max> query{};
  const auto query_size = encode_host_terminal_theme_query(query);
  return query_size > 0 &&
         write_terminal_text_interruptibly(descriptor, std::string_view(query.data(), query_size));
}

[[nodiscard]] auto advance_sequence(std::uint32_t& sequence) noexcept -> bool {
  if (sequence == std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  ++sequence;
  return true;
}

[[nodiscard]] auto send_small_message(const int connection, const protocol::SmallMessage& message,
                                      std::uint32_t& sequence) noexcept -> bool {
  return send_interruptibly(connection, message.bytes()) && advance_sequence(sequence);
}

[[nodiscard]] auto send_resize(const int connection, const platform::WindowSize size,
                               std::uint32_t& sequence) noexcept -> bool {
  return send_small_message(
             connection,
             protocol::encode_cell_size(
                 {.width = size.cell_width_px, .height = size.cell_height_px}, sequence),
             sequence) &&
         send_small_message(
             connection,
             protocol::encode_resize({.columns = size.columns, .rows = size.rows}, sequence),
             sequence);
}

template <typename Header>
[[nodiscard]] auto send_payload(const int connection, const Header& header,
                                const std::span<const std::byte> input,
                                std::uint32_t& sequence) noexcept -> bool {
  return send_interruptibly(connection, header) && send_interruptibly(connection, input) &&
         advance_sequence(sequence);
}

[[nodiscard]] auto send_input(const int connection, const std::span<const std::byte> input,
                              std::uint32_t& sequence) noexcept -> bool {
  return input.empty() ||
         send_payload(connection, protocol::encode_input_header(input.size(), sequence), input,
                      sequence);
}

[[nodiscard]] auto send_paste(const int connection, const std::span<const std::byte> input,
                              std::uint32_t& sequence) noexcept -> bool {
  return input.empty() ||
         send_payload(connection, protocol::encode_paste_header(input.size(), sequence), input,
                      sequence);
}

void report_disconnect(const protocol::ServerMessage& message) noexcept {
  static_cast<void>(write_text_interruptibly(STDERR_FILENO, "lemma attach failed: "));
  static_cast<void>(write_text_interruptibly(
      STDERR_FILENO,
      message.diagnostic.empty() ? std::string_view{"daemon disconnected"} : message.diagnostic));
  static_cast<void>(write_text_interruptibly(STDERR_FILENO, "\n"));
}

enum class HandshakeResult : std::uint8_t {
  accepted,
  rejected,
  error,
};

// The branches are bounded handshake transport and typed protocol outcomes.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto receive_handshake(const int connection, protocol::ServerDecoder& decoder,
                                     const protocol::Dimensions expected) noexcept
    -> HandshakeResult {
  while (true) {
    const auto decoded = decoder.next();
    if (!decoded.has_value()) {
      static_cast<void>(write_text_interruptibly(STDERR_FILENO, "lemma attach protocol error: "));
      static_cast<void>(write_text_interruptibly(
          STDERR_FILENO, protocol::decode_error_diagnostic(decoded.error())));
      static_cast<void>(write_text_interruptibly(STDERR_FILENO, "\n"));
      return HandshakeResult::error;
    }
    if (decoded->has_value()) {
      const auto& message = **decoded;
      if (message.kind == protocol::ServerMessageKind::disconnect) {
        report_disconnect(message);
        decoder.consume();
        return HandshakeResult::rejected;
      }
      if (message.kind != protocol::ServerMessageKind::hello || message.dimensions != expected) {
        static_cast<void>(write_text_interruptibly(
            STDERR_FILENO, "lemma attach protocol error: invalid daemon hello\n"));
        return HandshakeResult::error;
      }
      decoder.consume();
      return HandshakeResult::accepted;
    }

    auto available = decoder.writable_bytes();
    if (available.empty()) {
      return HandshakeResult::error;
    }
    const auto received = ::recv(connection, available.data(), available.size(), 0);
    if (received > 0) {
      if (!decoder.commit(static_cast<std::size_t>(received)).has_value()) {
        return HandshakeResult::error;
      }
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return HandshakeResult::error;
  }
}

enum class ServerParseResult : std::uint8_t {
  keep,
  clean_disconnect,
  disconnect,
  peer_closed,
  error,
};

class LiveDiagnostic final {
public:
  void record_protocol_error(const protocol::DecodeError error) noexcept {
    if (!empty()) {
      return;
    }
    append("lemma attach protocol error: ");
    append(protocol::decode_error_diagnostic(error));
    append("\n");
  }

  void record_disconnect(const protocol::ServerMessage& message) noexcept {
    if (!empty() || message.reason == protocol::DisconnectReason::normal) {
      return;
    }
    append("lemma attach failed: ");
    append(message.diagnostic.empty() ? std::string_view{"daemon disconnected"}
                                      : message.diagnostic);
    append("\n");
  }

  void record_terminal_error() noexcept {
    if (empty()) {
      append("lemma attach error: outer terminal write failed\n");
    }
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0; }
  [[nodiscard]] auto view() const noexcept -> std::string_view { return {storage_.data(), size_}; }

private:
  void append(const std::string_view text) noexcept {
    const auto count = std::min(text.size(), storage_.size() - size_);
    std::ranges::copy(std::span(text).first(count),
                      std::span(storage_).subspan(size_, count).begin());
    size_ += count;
  }

  std::array<char, protocol::diagnostic_bytes_max + 64U> storage_{};
  std::size_t size_{0};
};

// Live parsing validates a complete frame before presenting it to the outer terminal.
[[nodiscard]] auto
process_server_messages(protocol::ServerDecoder& decoder, const int terminal_descriptor,
                        diagnostic::LatencyTraceMarkerMatcher& output_trace_matcher,
                        std::uint64_t& pending_trace_correlation,
#ifdef LEMMA_ENABLE_LATENCY_TRACE
                        const diagnostic::LatencyTraceEventHandle socket_read_event,
#endif
                        LiveDiagnostic& live_diagnostic) noexcept -> ServerParseResult {
  while (true) {
    const auto decoded = decoder.next();
    if (!decoded.has_value()) {
      live_diagnostic.record_protocol_error(decoded.error());
      return ServerParseResult::error;
    }
    if (!decoded->has_value()) {
      return ServerParseResult::keep;
    }
    const auto& message = **decoded;
    if (message.kind == protocol::ServerMessageKind::hello) {
      live_diagnostic.record_protocol_error(protocol::DecodeError::invalid_kind);
      return ServerParseResult::error;
    }
    if (message.kind == protocol::ServerMessageKind::disconnect) {
      live_diagnostic.record_disconnect(message);
      const bool clean = message.reason == protocol::DisconnectReason::normal;
      decoder.consume();
      return clean ? ServerParseResult::clean_disconnect : ServerParseResult::disconnect;
    }

    std::uint64_t trace_correlation = 0;
#ifdef LEMMA_ENABLE_LATENCY_TRACE
    trace_correlation =
        output_trace_matcher.observe_expected_visible(message.ansi, pending_trace_correlation);
    if (trace_correlation != 0) {
      // process_server_messages drains every complete envelope before another recv. Therefore the
      // successful recv that reserved this append-only event is the exact read that made this
      // marker-bearing frame complete, including fragmented and coalesced frames.
      static_cast<void>(diagnostic::correlate_client_socket_read_latency_trace(socket_read_event,
                                                                               trace_correlation));
      pending_trace_correlation = 0;
    }
#else
    static_cast<void>(output_trace_matcher);
    static_cast<void>(pending_trace_correlation);
#endif
    diagnostic::record_latency_trace(
        diagnostic::LatencyTraceStage::client_outer_terminal_write_started,
        static_cast<std::uint32_t>(terminal_descriptor), message.ansi.size(), trace_correlation);
    if (!write_terminal_interruptibly(terminal_descriptor, message.ansi)) {
      live_diagnostic.record_terminal_error();
      return ServerParseResult::error;
    }
    diagnostic::record_latency_trace(
        diagnostic::LatencyTraceStage::client_outer_terminal_write_finished,
        static_cast<std::uint32_t>(terminal_descriptor), message.ansi.size(), trace_correlation);
    decoder.consume();
  }
}

[[nodiscard]] auto receive_server(const int connection, protocol::ServerDecoder& decoder,
                                  const int terminal_descriptor,
                                  diagnostic::LatencyTraceMarkerMatcher& output_trace_matcher,
                                  std::uint64_t& pending_trace_correlation,
                                  LiveDiagnostic& live_diagnostic) noexcept -> ServerParseResult {
  const auto buffered = process_server_messages(decoder, terminal_descriptor, output_trace_matcher,
                                                pending_trace_correlation,
#ifdef LEMMA_ENABLE_LATENCY_TRACE
                                                {},
#endif
                                                live_diagnostic);
  if (buffered != ServerParseResult::keep) {
    return buffered;
  }
  auto available = decoder.writable_bytes();
  if (available.empty()) {
    live_diagnostic.record_protocol_error(protocol::DecodeError::buffer_full);
    return ServerParseResult::error;
  }
  const auto received = ::recv(connection, available.data(), available.size(), 0);
  if (received == 0) {
    return ServerParseResult::peer_closed;
  }
  if (received < 0) {
    // Abrupt daemon death on a Unix socket can RST instead of FIN when unread bytes remain.
    // That is peer loss, not a distinct protocol or read-path failure.
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      return ServerParseResult::keep;
    }
    return ServerParseResult::peer_closed;
  }
  const auto size = static_cast<std::size_t>(received);
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  // The read timestamp is reserved before decoding can reveal the exact marker token. The decoder
  // may later correlate this one append-only event; unrelated reads remain explicitly uncorrelated.
  const auto socket_read_event = diagnostic::record_client_socket_read_latency_trace(
      static_cast<std::uint32_t>(connection), size);
#endif
  const auto committed = decoder.commit(size);
  if (!committed.has_value()) {
    live_diagnostic.record_protocol_error(committed.error());
    return ServerParseResult::error;
  }
  return process_server_messages(decoder, terminal_descriptor, output_trace_matcher,
                                 pending_trace_correlation,
#ifdef LEMMA_ENABLE_LATENCY_TRACE
                                 socket_read_event,
#endif
                                 live_diagnostic);
}

// This is the client reactor; branches correspond directly to terminal and socket readiness.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto attach_client(const daemon::RuntimeEndpoint& endpoint,
                                 const std::string_view session) -> int {
  diagnostic::set_latency_trace_role(diagnostic::LatencyTraceRole::attached_client);
  resize_pending = 0;
  resize_wakeup_descriptor = -1;
  termination_signal = 0;
  termination_wakeup_descriptor = -1;
  termination_wakeup_read_descriptor = -1;
  terminal_restore_wakeup_descriptor = -1;
  termination_render_descriptor = -1;
  termination_render_closed = 0;
  if (::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0) {
    static_cast<void>(
        write_text_interruptibly(STDERR_FILENO, "lemma attach requires a terminal\n"));
    return 1;
  }

  protocol::ServerDecoder decoder;
  HostInputParser host_input_parser;
  // Runtime-sized host input storage is prepared before raw terminal or presentation mutation.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::unique_ptr<std::byte[]> classified_input_storage;
  try {
    classified_input_storage =
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        std::make_unique_for_overwrite<std::byte[]>(host_input_output_bytes_max);
  } catch (const std::bad_alloc&) {
    static_cast<void>(
        write_text_interruptibly(STDERR_FILENO, "lemma attach input allocation failed\n"));
    return 1;
  }
  if (!decoder.prepare().has_value() || !host_input_parser.prepare().has_value()) {
    static_cast<void>(
        write_text_interruptibly(STDERR_FILENO, "lemma attach decoder allocation failed\n"));
    return 1;
  }
  const auto classified_input =
      std::span(classified_input_storage.get(), host_input_output_bytes_max);
  int connection = daemon::open_server_connection(endpoint);
  if (connection < 0) {
    static_cast<void>(
        write_text_interruptibly(STDERR_FILENO, "no lemma daemon; run `lemma` first\n"));
    return 1;
  }
  SignalWakeup termination_wakeup(SignalWakeupRole::termination);
  SignalWakeup resize_wakeup(SignalWakeupRole::resize);
  if (!termination_wakeup.install() || !resize_wakeup.install()) {
    close_descriptor(connection);
    return 1;
  }
  SignalActions signal_actions;
  if (!signal_actions.install()) {
    close_descriptor(connection);
    return 1;
  }

  bool clean_detach = false;
  bool typed_disconnect = false;
  bool protocol_failure = false;
  bool terminal_setup_succeeded = false;
  LiveDiagnostic live_diagnostic;
  std::uint32_t client_sequence = 2;
  {
    // Declare presentation state first so RawTerminal's final restoration runs before any bounded
    // presentation-output cleanup. The signal path restores explicitly, while reverse destruction
    // ordering preserves the same termios-before-presentation boundary on every other exit.
    OuterTerminal outer_terminal;
    platform::RawTerminal raw_terminal;
    const bool raw_terminal_entered = raw_terminal.enter(STDIN_FILENO);
    if (raw_terminal_entered) {
      terminal_restore_wakeup_descriptor = raw_terminal.restore_wakeup_descriptor();
    }

    HostTerminalThemeParser host_theme_parser;
    bool host_theme_query_pending = false;
    bool host_theme_update_pending = false;
    constexpr auto host_theme_query_timeout = std::chrono::milliseconds(100);
    auto host_theme_deadline = std::chrono::steady_clock::time_point{};
    const auto size = terminal_size();
    const protocol::Dimensions dimensions{.columns = size.columns, .rows = size.rows};
    const auto hello = protocol::encode_client_hello(session, dimensions);
    const bool handshake_accepted =
        raw_terminal_entered && send_interruptibly(connection, hello.bytes()) &&
        receive_handshake(connection, decoder, dimensions) == HandshakeResult::accepted;
    terminal_setup_succeeded =
        handshake_accepted && outer_terminal.enter() &&
        send_small_message(
            connection,
            protocol::encode_cell_size({.width = size.cell_width_px, .height = size.cell_height_px},
                                       client_sequence),
            client_sequence);
    if (terminal_setup_succeeded) {
      termination_render_descriptor = outer_terminal.render_descriptor();
      // A rejected handshake has not acquired an AttachmentRuntime and must not leave terminal
      // query replies for the parent shell. Arm filtering before the write so a partial query write
      // cannot produce an unowned response.
      host_theme_query_pending = true;
      host_theme_deadline = std::chrono::steady_clock::now() + host_theme_query_timeout;
      static_cast<void>(send_host_terminal_theme_query(outer_terminal.render_descriptor()));
    }

    std::array<std::byte, protocol::input_bytes_max> input{};
#ifdef LEMMA_ENABLE_LATENCY_TRACE
    diagnostic::LatencyTraceMarkerMatcher input_trace_matcher;
#endif
    diagnostic::LatencyTraceMarkerMatcher output_trace_matcher;
    std::uint64_t pending_trace_correlation = 0;
    auto host_input_deadline = std::chrono::steady_clock::time_point{};
    auto outer_resize_deadline = std::chrono::steady_clock::time_point{};
    auto sent_size = size;
    auto observed_size = size;
    bool in_band_geometry = false;
    bool outer_resize_deferred = false;
    bool attached = terminal_setup_succeeded;
    const auto commit_outer_resize = [&]() noexcept {
      if (!outer_resize_deferred) {
        return true;
      }
      const auto settled_size = in_band_geometry ? observed_size : terminal_size();
      if ((settled_size.columns != sent_size.columns || settled_size.rows != sent_size.rows ||
           settled_size.cell_width_px != sent_size.cell_width_px ||
           settled_size.cell_height_px != sent_size.cell_height_px) &&
          !send_resize(connection, settled_size, client_sequence)) {
        return false;
      }
      sent_size = settled_size;
      outer_resize_deferred = false;
      return true;
    };
    const auto defer_outer_resize = [&]() noexcept {
      resize_pending = 0;
      outer_resize_deferred = true;
      outer_resize_deadline = std::chrono::steady_clock::now() + outer_resize_quiet_delay;
    };
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    const auto forward_host_batch = [&](const HostInputBatch& batch) noexcept {
      for (const auto& event : std::span(batch.events).first(batch.event_count)) {
        if (event.kind != HostInputKind::window_size && !commit_outer_resize()) {
          return false;
        }
        const auto bytes = std::span(classified_input).subspan(event.offset, event.size);
        switch (event.kind) {
        case HostInputKind::clipboard_supported:
        case HostInputKind::clipboard_unsupported:
          if (!send_small_message(
                  connection,
                  protocol::encode_clipboard_support(
                      event.kind == HostInputKind::clipboard_supported, client_sequence),
                  client_sequence)) {
            return false;
          }
          break;
        case HostInputKind::theme_reply:
          // Filtering belongs after paste/key classification, and remains active after startup.
          host_theme_parser.push(bytes);
          host_theme_parser.finish();
          host_theme_parser.consume_pending_input();
          host_theme_update_pending = true;
          break;
        case HostInputKind::window_size:
          // Once observed, native reports outrank potentially stale or pixel-less proxy ioctls.
          observed_size = event.window_size;
          in_band_geometry = true;
          defer_outer_resize();
          break;
        case HostInputKind::ordinary:
          if (!send_input(connection, bytes, client_sequence)) {
            return false;
          }
          break;
        case HostInputKind::terminal_reply:
        case HostInputKind::terminal_reply_stream:
          if (!send_payload(connection,
                            protocol::encode_terminal_reply_header(bytes.size(), client_sequence),
                            bytes, client_sequence)) {
            return false;
          }
          break;
        case HostInputKind::paste:
          if (!send_paste(connection, bytes, client_sequence)) {
            return false;
          }
          break;
        case HostInputKind::key:
          if (!send_small_message(connection,
                                  protocol::encode_key(event.key, bytes, client_sequence),
                                  client_sequence)) {
            return false;
          }
          break;
        case HostInputKind::focus:
          if (!send_small_message(connection, protocol::encode_focus(event.focus, client_sequence),
                                  client_sequence)) {
            return false;
          }
          break;
        case HostInputKind::mouse:
          if (!send_small_message(connection, protocol::encode_mouse(event.mouse, client_sequence),
                                  client_sequence)) {
            return false;
          }
          break;
        }
      }
      return true;
    };
    const auto forward_physical_input = [&](const std::span<const std::byte> bytes) noexcept {
      const auto current_size = in_band_geometry ? observed_size : terminal_size();
      const auto previous_report = host_input_parser.pending_report();
      const auto parsed = host_input_parser.parse(
          bytes, classified_input, {.columns = current_size.columns, .rows = current_size.rows});
      if (!parsed.has_value() || !forward_host_batch(*parsed)) {
        return false;
      }
      if (const auto report = host_input_parser.pending_report(); report.has_value()) {
        if (report != previous_report) {
          host_input_deadline = std::chrono::steady_clock::now() + host_terminal_reply_timeout;
        }
      } else if (host_input_parser.has_pending_sequence() && !host_input_parser.paste_active()) {
        host_input_deadline = std::chrono::steady_clock::now() + host_input_flush_delay;
      }
      return true;
    };
    const auto finish_host_theme_query = [&]() noexcept {
      host_theme_query_pending = false;
      host_theme_update_pending = false;
      if (host_theme_parser.overflowed()) {
        static_cast<void>(write_text_interruptibly(
            STDERR_FILENO, "lemma attach host-theme query exceeded retained input capacity\n"));
        return false;
      }
      const auto theme = host_theme_parser.theme();
      return !theme.has_value() ||
             send_small_message(connection,
                                protocol::encode_host_theme_update(*theme, client_sequence),
                                client_sequence);
    };
    while (attached && termination_signal == 0) {
      if ((host_theme_query_pending && (host_theme_parser.complete() ||
                                        std::chrono::steady_clock::now() >= host_theme_deadline)) ||
          (!host_theme_query_pending && host_theme_update_pending)) {
        if (!finish_host_theme_query()) {
          break;
        }
      }
      const auto buffered = process_server_messages(decoder, outer_terminal.render_descriptor(),
                                                    output_trace_matcher, pending_trace_correlation,
#ifdef LEMMA_ENABLE_LATENCY_TRACE
                                                    {},
#endif
                                                    live_diagnostic);
      if (buffered == ServerParseResult::clean_disconnect) {
        clean_detach = true;
        break;
      }
      if (buffered == ServerParseResult::disconnect) {
        typed_disconnect = true;
        break;
      }
      if (buffered == ServerParseResult::error) {
        protocol_failure = true;
        break;
      }

      if (resize_pending != 0) {
        defer_outer_resize();
        resize_wakeup.drain();
      } else if (outer_resize_deferred &&
                 std::chrono::steady_clock::now() >= outer_resize_deadline &&
                 !commit_outer_resize()) {
        break;
      }

      int poll_timeout = -1;
      if (host_input_parser.has_pending_sequence() && !host_input_parser.paste_active()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= host_input_deadline) {
          const auto flushed = host_input_parser.flush_pending(classified_input);
          if (!flushed.has_value() || !forward_host_batch(*flushed)) {
            break;
          }
        } else {
          const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(host_input_deadline - now);
          poll_timeout = static_cast<int>(std::max(remaining.count(), std::int64_t{1}));
        }
      }
      if (host_theme_query_pending) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            host_theme_deadline - std::chrono::steady_clock::now());
        const auto theme_timeout = static_cast<int>(std::max(remaining.count(), std::int64_t{1}));
        poll_timeout = poll_timeout < 0 ? theme_timeout : std::min(poll_timeout, theme_timeout);
      }
      if (outer_resize_deferred) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            outer_resize_deadline - std::chrono::steady_clock::now());
        const auto resize_timeout = static_cast<int>(std::max(remaining.count(), std::int64_t{1}));
        poll_timeout = poll_timeout < 0 ? resize_timeout : std::min(poll_timeout, resize_timeout);
      }

      std::array<pollfd, 4> descriptors{{
          {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
          {.fd = termination_wakeup.descriptor(), .events = POLLIN, .revents = 0},
          {.fd = resize_wakeup.descriptor(), .events = POLLIN, .revents = 0},
          {.fd = connection, .events = POLLIN, .revents = 0},
      }};
      const auto poll_result = ::poll(descriptors.data(), descriptors.size(), poll_timeout);
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      const auto& input_events = descriptors.front();
      const auto& termination_events = std::span(descriptors).subspan(1, 1).front();
      const auto& resize_events = std::span(descriptors).subspan(2, 1).front();
      const auto& server_events = descriptors.back();
      if ((termination_events.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        termination_wakeup.drain();
        if (termination_signal != 0) {
          break;
        }
      }
      if ((resize_events.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        defer_outer_resize();
        resize_wakeup.drain();
      }
      if ((server_events.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        const auto received =
            receive_server(connection, decoder, outer_terminal.render_descriptor(),
                           output_trace_matcher, pending_trace_correlation, live_diagnostic);
        if (received == ServerParseResult::clean_disconnect) {
          clean_detach = true;
          break;
        }
        if (received == ServerParseResult::disconnect) {
          typed_disconnect = true;
          break;
        }
        if (received == ServerParseResult::peer_closed) {
          break;
        }
        if (received == ServerParseResult::error) {
          protocol_failure = true;
          break;
        }
      }

      if ((input_events.revents & POLLIN) != 0) {
        // Input observed after a physical resize must not overtake the settled geometry update.
        // It also gives an actively interacting user an immediate endpoint without waiting for the
        // trailing-edge timer.
        if (resize_pending != 0) {
          defer_outer_resize();
          resize_wakeup.drain();
        }
        if (!commit_outer_resize()) {
          break;
        }
        const auto bytes_read = ::read(STDIN_FILENO, input.data(), input.size());
        if (bytes_read <= 0) {
          break;
        }
        const auto input_size = static_cast<std::size_t>(bytes_read);
        std::uint64_t trace_correlation = 0;
#ifdef LEMMA_ENABLE_LATENCY_TRACE
        trace_correlation = input_trace_matcher.observe(std::span(input).first(input_size));
        if (trace_correlation != 0) {
          pending_trace_correlation = trace_correlation;
        }
#endif
        diagnostic::record_latency_trace(diagnostic::LatencyTraceStage::client_physical_input_read,
                                         static_cast<std::uint32_t>(STDIN_FILENO), input_size,
                                         trace_correlation);
        const auto physical_input = std::span(input).first(input_size);
        if (!forward_physical_input(physical_input)) {
          break;
        }
      }
    }
    if (termination_signal != 0) {
      // The signal handler has already retired the saturated writer using only async-signal-safe
      // operations. Synchronize the presentation guard before helper pipes can reuse that fd.
      outer_terminal.adopt_signal_render_retirement();
      const auto restoration_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      // Discard the retired writer's blocked queue through the retained nonblocking cleanup
      // endpoint before asking any termios strategy to make progress.
      outer_terminal.abort_render_output();
      // The emergency restorer now runs nonblocking without the saturated writer held open. Give it
      // a bounded interval to report a confirmed restore before invoking foreground or detached
      // fallbacks that may consume the rest of the same end-to-end deadline.
      const auto emergency_deadline = std::min(
          restoration_deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
      bool termios_restored = raw_terminal.wait_for_emergency_restore(emergency_deadline);
      if (!termios_restored && std::chrono::steady_clock::now() < restoration_deadline) {
        termios_restored = raw_terminal.restore();
      }
      outer_terminal.retire_render_writer();
      if (!termios_restored && std::chrono::steady_clock::now() < restoration_deadline) {
        termios_restored = raw_terminal.restore_bounded(restoration_deadline);
      }
      if (!termios_restored) {
        // RawTerminal's immediately following destructor retains one final bounded attempt. Avoid
        // placing it behind the emergency presentation write, then terminate on persistent failure.
        outer_terminal.suppress_signal_cleanup();
      }
    }
    terminal_restore_wakeup_descriptor = -1;
    termination_render_descriptor = -1;
  }

  close_descriptor(connection);
  if (termination_signal != 0) {
    return 128 + termination_signal;
  }
  if (!terminal_setup_succeeded) {
    return 1;
  }
  if ((typed_disconnect || protocol_failure) && !live_diagnostic.empty()) {
    static_cast<void>(write_text_interruptibly(STDERR_FILENO, live_diagnostic.view()));
  }
  if (!clean_detach && !typed_disconnect && !protocol_failure) {
    static_cast<void>(
        write_text_interruptibly(STDERR_FILENO, "lemma session ended or connection was lost\n"));
  }
  return clean_detach ? 0 : 1;
}

} // namespace

[[nodiscard]] auto attach(const daemon::RuntimeEndpoint& endpoint, const std::string_view session)
    -> int {
  return session.empty() || daemon::validate_session(session) ? attach_client(endpoint, session)
                                                              : 1;
}

} // namespace lemma::client
