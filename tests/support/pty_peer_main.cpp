#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds block_receive_idle_timeout{30s};

[[nodiscard]] auto write_all(const std::string_view text) noexcept -> bool {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto remaining = std::span(text).subspan(offset);
    const auto written = ::write(STDOUT_FILENO, remaining.data(), remaining.size());
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

[[nodiscard]] auto write_all_until(const std::string_view text,
                                   const std::chrono::steady_clock::time_point deadline) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto remaining = std::span(text).subspan(offset);
    const auto written = ::write(STDOUT_FILENO, remaining.data(), remaining.size());
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        std::chrono::steady_clock::now() < deadline) {
      pollfd event{.fd = STDOUT_FILENO, .events = POLLOUT, .revents = 0};
      const auto time_left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      const auto timeout = static_cast<int>(std::max(time_left.count(), std::int64_t{1}));
      const auto polled = ::poll(&event, 1, timeout);
      if (polled > 0 || (polled < 0 && errno == EINTR)) {
        continue;
      }
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto enter_raw_input() noexcept -> bool {
  termios state{};
  if (::tcgetattr(STDIN_FILENO, &state) != 0) {
    return false;
  }
  ::cfmakeraw(&state);
  return ::tcsetattr(STDIN_FILENO, TCSANOW, &state) == 0;
}

[[nodiscard]] auto wait_for_gate(const char* const path) noexcept -> bool {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (::access(path, F_OK) == 0) {
      return true;
    }
    if (errno != ENOENT) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

[[nodiscard]] auto parse_size(const std::string_view value) noexcept -> std::size_t {
  std::size_t size = 0;
  const auto input = std::span(value);
  const auto* const end = std::to_address(input.end());
  const auto result = std::from_chars(input.data(), end, size);
  return result.ec == std::errc{} && result.ptr == end ? size : 0;
}

[[nodiscard]] auto read_exact_digest(const std::size_t expected, std::uint64_t& digest,
                                     std::size_t& received,
                                     const std::chrono::milliseconds idle_timeout) noexcept
    -> bool {
  constexpr std::uint64_t offset_basis = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;
  digest = offset_basis;
  received = 0;
  std::array<std::byte, std::size_t{16} * 1'024U> bytes{};
  // The harness bounds the complete send/recovery operation. This guard detects
  // stalled input, not a slow subject that is still delivering the payload.
  auto deadline = std::chrono::steady_clock::now() + idle_timeout;
  while (received < expected && std::chrono::steady_clock::now() < deadline) {
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    const auto remaining = expected - received;
    const auto polled = ::poll(&event, 1, 100);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled <= 0) {
      continue;
    }
    const auto count = ::read(STDIN_FILENO, bytes.data(), std::min(bytes.size(), remaining));
    if (count > 0) {
      const auto size = static_cast<std::size_t>(count);
      for (const auto byte : std::span(bytes).first(size)) {
        digest ^= std::to_integer<std::uint8_t>(byte);
        digest *= prime;
      }
      received += size;
      deadline = std::chrono::steady_clock::now() + idle_timeout;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return received == expected;
}

void linger_for_render() noexcept { std::this_thread::sleep_for(250ms); }

[[nodiscard]] auto run_block(const char* const gate, const std::size_t bytes,
                             const std::chrono::milliseconds idle_timeout) noexcept -> int {
  if (bytes == 0 || bytes > std::size_t{8} * 1'024U * 1'024U || !enter_raw_input() ||
      !write_all("\r\n__LEMMA_PTY_READY__\r\n") || !wait_for_gate(gate)) {
    return 1;
  }
  std::uint64_t digest = 0;
  std::size_t received = 0;
  if (!read_exact_digest(bytes, digest, received, idle_timeout)) {
    std::array<char, 32> received_text{};
    const auto received_storage = std::span(received_text);
    const auto encoded =
        std::to_chars(received_storage.data(), std::to_address(received_storage.end()), received);
    static_cast<void>(write_all("\r\n__LEMMA_PTY_FAILED__ received="));
    if (encoded.ec == std::errc{}) {
      static_cast<void>(write_all(
          {received_text.data(), static_cast<std::size_t>(encoded.ptr - received_text.data())}));
    }
    static_cast<void>(write_all("\r\n"));
    linger_for_render();
    return 1;
  }
  std::array<char, 32> digest_text{};
  const auto digest_storage = std::span(digest_text);
  const auto encoded =
      std::to_chars(digest_storage.data(), std::to_address(digest_storage.end()), digest, 16);
  if (encoded.ec != std::errc{}) {
    return 1;
  }
  std::array<char, 32> count_text{};
  const auto count_storage = std::span(count_text);
  const auto count =
      std::to_chars(count_storage.data(), std::to_address(count_storage.end()), bytes);
  if (count.ec != std::errc{}) {
    return 1;
  }
  const bool written =
      write_all("\r\n__LEMMA_PTY_DONE__ bytes=") &&
      write_all({count_text.data(), static_cast<std::size_t>(count.ptr - count_text.data())}) &&
      write_all(" digest=") &&
      write_all({digest_text.data(), static_cast<std::size_t>(encoded.ptr - digest_text.data())}) &&
      write_all("\r\n");
  linger_for_render();
  return written ? 0 : 1;
}

// This test peer keeps every ordering failure explicit for bounded diagnostics.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_order(const char* const gate, const std::string_view user_input) noexcept
    -> int {
  if (user_input.empty() || user_input.size() > 1'024U || !enter_raw_input() ||
      !write_all("\x1B[5n") || !write_all("\r\n__LEMMA_ORDER_READY__\r\n") ||
      !wait_for_gate(gate)) {
    return 1;
  }
  constexpr std::string_view response = "\x1B[0n";
  std::array<char, 1'028> received{};
  const auto expected = response.size() + user_input.size();
  std::size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (offset < expected && std::chrono::steady_clock::now() < deadline) {
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    if (::poll(&event, 1, 100) <= 0) {
      continue;
    }
    auto remaining = std::span(received).subspan(offset, expected - offset);
    const auto count = ::read(STDIN_FILENO, remaining.data(), remaining.size());
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return 1;
  }
  const std::string_view actual(received.data(), offset);
  const bool matches = offset == expected &&
                       std::ranges::equal(std::span(actual).first(response.size()), response) &&
                       std::ranges::equal(std::span(actual).subspan(response.size()), user_input);
  if (!matches) {
    static_cast<void>(write_all("\r\n__LEMMA_ORDER_FAILED__ bytes="));
    for (const char character : actual) {
      const auto byte = static_cast<unsigned char>(character);
      std::array<char, 2> encoded{};
      constexpr std::string_view digits = "0123456789abcdef";
      encoded.front() = std::span(digits).subspan(byte >> 4U, 1).front();
      encoded.back() = std::span(digits).subspan(byte & 0x0FU, 1).front();
      static_cast<void>(write_all({encoded.data(), encoded.size()}));
    }
    static_cast<void>(write_all("\r\n"));
    linger_for_render();
    return 1;
  }
  const bool written = write_all("\r\n__LEMMA_ORDER_OK__\r\n");
  linger_for_render();
  return written ? 0 : 1;
}

[[nodiscard]] auto write_background_line() noexcept -> bool {
  constexpr std::string_view line =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopq\r\n";
  static_assert(line.size() == 81);
  const auto written = ::write(STDOUT_FILENO, line.data(), line.size());
  return written >= 0 || errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

struct TuiFrameGeometry final {
  std::size_t rows{0};
  std::size_t columns{0};
};

[[nodiscard]] auto tui_frame_geometry() noexcept -> std::optional<TuiFrameGeometry> {
  winsize size{};
  // ioctl is variadic because its third argument depends on the request.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_row < 2 || size.ws_col < 2 ||
      size.ws_row > 200 || size.ws_col > 500) {
    return std::nullopt;
  }
  // Stay one row and column inside the child viewport. Writing its final cell would set the
  // autowrap latch, and a following CRLF can scroll away the synchronized marker on muxes whose
  // chrome reduces the child PTY below the 80x24 outer terminal.
  return TuiFrameGeometry{.rows = static_cast<std::size_t>(size.ws_row - 1U),
                          .columns = static_cast<std::size_t>(size.ws_col - 1U)};
}

[[nodiscard]] auto write_tui_frame(const std::string_view marker, const std::size_t sequence,
                                   const TuiFrameGeometry geometry) noexcept -> bool {
  if (marker.empty() || marker.size() > geometry.columns || geometry.rows == 0 ||
      geometry.columns == 0 || geometry.columns > 500) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  std::array<char, 500> line{};
  std::ranges::fill(std::span(line).first(geometry.columns),
                    static_cast<char>('a' + (sequence % 26U)));
  if (!write_all_until("\x1B[?2026h\x1B[H", deadline)) {
    return false;
  }
  const auto marker_row = geometry.rows / 2U;
  for (std::size_t row = 0; row < geometry.rows; ++row) {
    if (row == marker_row) {
      std::ranges::copy(marker, line.begin());
    }
    if (!write_all_until({line.data(), geometry.columns}, deadline) ||
        (row + 1U < geometry.rows && !write_all_until("\r\n", deadline))) {
      return false;
    }
    if (row == marker_row) {
      std::ranges::fill(std::span(line).first(geometry.columns),
                        static_cast<char>('a' + (sequence % 26U)));
    }
  }
  return write_all_until("\x1B[?2026l", deadline);
}

// The branches are the explicit bounded states of the synthetic wheel peer.
// NOLINTBEGIN(readability-function-cognitive-complexity)
[[nodiscard]] auto run_tui_wheel(const std::string_view receipt_path,
                                 const std::size_t burst_size) noexcept -> int {
  if (receipt_path.empty() || burst_size == 0 || burst_size > 256U) {
    return 1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (receipt_path.size() >= sizeof(address.sun_path)) {
    return 1;
  }
  std::memcpy(std::span(address.sun_path).data(), receipt_path.data(), receipt_path.size());
  const int receipt = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  if (receipt < 0) {
    return 1;
  }
  // The socket ABI intentionally erases the concrete address type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* generic = reinterpret_cast<const sockaddr*>(&address);
  constexpr std::string_view ready = "\x1B[?1049h\x1B[?1000h\x1B[?1006h\x1B[?2026h\x1B[2J\x1B[H"
                                     "__LEMMA_TUI_WHEEL_READY__\x1B[?2026l";
  const auto frame_geometry = tui_frame_geometry();
  if (!frame_geometry.has_value() || ::connect(receipt, generic, sizeof(address)) != 0 ||
      !enter_raw_input() || !write_all(ready)) {
    static_cast<void>(::close(receipt));
    return 1;
  }

  constexpr std::string_view armed_receipt = "__LEMMA_TUI_WHEEL_ARMED__";
  std::array<char, 128> marker{};
  std::size_t marker_size = 0;
  std::size_t wheel_events = 0;
  std::size_t sequence = 0;
  bool armed = false;
  auto deadline = std::chrono::steady_clock::now() + 120s;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&event, 1, 100);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      break;
    }
    if (polled == 0) {
      continue;
    }
    std::array<char, 512> input{};
    const auto count = ::read(STDIN_FILENO, input.data(), input.size());
    if (count == 0) {
      static_cast<void>(::close(receipt));
      return 0;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      break;
    }
    deadline = std::chrono::steady_clock::now() + 120s;
    for (const char byte : std::span(input).first(static_cast<std::size_t>(count))) {
      if (!armed) {
        if (byte != '\n') {
          if (marker_size == marker.size()) {
            static_cast<void>(::close(receipt));
            return 1;
          }
          std::span(marker).subspan(marker_size, 1).front() = byte;
          ++marker_size;
          continue;
        }
        if (marker_size == 0) {
          static_cast<void>(::close(receipt));
          return 1;
        }
        const auto sent = ::send(receipt, armed_receipt.data(), armed_receipt.size(), MSG_NOSIGNAL);
        if (sent < 0 || static_cast<std::size_t>(sent) != armed_receipt.size()) {
          static_cast<void>(::close(receipt));
          return 1;
        }
        armed = true;
        continue;
      }
      if (byte != 'M') {
        continue;
      }
      ++wheel_events;
      if (wheel_events != burst_size) {
        continue;
      }
      const auto frame = std::string_view(marker.data(), marker_size);
      const auto sent = ::send(receipt, frame.data(), frame.size(), MSG_NOSIGNAL);
      if (sent < 0 || static_cast<std::size_t>(sent) != frame.size() ||
          !write_tui_frame(frame, sequence, *frame_geometry)) {
        static_cast<void>(::close(receipt));
        return 1;
      }
      ++sequence;
      marker_size = 0;
      wheel_events = 0;
      armed = false;
    }
  }
  static_cast<void>(::close(receipt));
  return 1;
}
// NOLINTEND(readability-function-cognitive-complexity)

[[nodiscard]] auto make_output_nonblocking() noexcept -> bool {
  // fcntl is variadic even when F_GETFL has no third argument.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const auto flags = ::fcntl(STDOUT_FILENO, F_GETFL);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return flags >= 0 && ::fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct GeometryReport final {
  std::uint16_t rows{0};
  std::uint16_t columns{0};
};

[[nodiscard]] auto append_text(std::span<char>& output, const std::string_view text) noexcept
    -> bool {
  if (output.size() < text.size()) {
    return false;
  }
  std::ranges::copy(text, output.begin());
  output = output.subspan(text.size());
  return true;
}

[[nodiscard]] auto append_number(std::span<char>& output, const std::uint16_t value) noexcept
    -> bool {
  const auto encoded = std::to_chars(output.data(), std::to_address(output.end()), value);
  if (encoded.ec != std::errc{}) {
    return false;
  }
  output = output.subspan(static_cast<std::size_t>(encoded.ptr - output.data()));
  return true;
}

[[nodiscard]] auto write_geometry_frame(const winsize size) noexcept -> bool {
  std::array<char, 512> frame{};
  std::span<char> remaining = frame;
  if (!append_text(remaining, "\x1B[H\x1B[") || !append_number(remaining, size.ws_row) ||
      !append_text(remaining, ";1H")) {
    return false;
  }
  const auto line_size = std::min<std::size_t>(size.ws_col, 256U);
  if (remaining.size() < line_size) {
    return false;
  }
  std::ranges::fill(remaining.first(line_size), 'x');
  remaining = remaining.subspan(line_size);
  if (!append_text(remaining, "\x1B[1;1H#\x1B[") || !append_number(remaining, size.ws_row) ||
      !append_text(remaining, ";") || !append_number(remaining, size.ws_col) ||
      !append_text(remaining, "H#\x1B[18t")) {
    return false;
  }
  return write_all({frame.data(), frame.size() - remaining.size()});
}

[[nodiscard]] auto parse_geometry_report(const std::string_view input) noexcept
    -> std::optional<GeometryReport> {
  constexpr std::string_view prefix = "\x1B[8;";
  const auto start = input.find(prefix);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  auto values = input;
  values.remove_prefix(start + prefix.size());
  const auto separator = values.find(';');
  const auto terminator =
      values.find('t', separator == std::string_view::npos ? 0 : separator + 1U);
  if (separator == std::string_view::npos || terminator == std::string_view::npos) {
    return std::nullopt;
  }
  GeometryReport report;
  const auto value_bytes = std::span(values);
  const auto rows = value_bytes.first(separator);
  const auto columns = value_bytes.subspan(separator + 1U, terminator - separator - 1U);
  const auto* const rows_begin = std::to_address(rows.begin());
  const auto* const rows_end = std::to_address(rows.end());
  const auto* const columns_begin = std::to_address(columns.begin());
  const auto* const columns_end = std::to_address(columns.end());
  const auto rows_result = std::from_chars(rows_begin, rows_end, report.rows);
  const auto columns_result = std::from_chars(columns_begin, columns_end, report.columns);
  if (rows_result.ec != std::errc{} || rows_result.ptr != rows_end ||
      columns_result.ec != std::errc{} || columns_result.ptr != columns_end) {
    return std::nullopt;
  }
  return report;
}

struct GeometryResponse final {
  std::optional<GeometryReport> report;
  bool stopped{false};
};

[[nodiscard]] auto read_geometry_response() noexcept -> GeometryResponse {
  std::array<char, 128> input{};
  std::size_t size = 0;
  const auto deadline = std::chrono::steady_clock::now() + 250ms;
  while (std::chrono::steady_clock::now() < deadline && size < input.size()) {
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&event, 1, 10);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled <= 0) {
      continue;
    }
    auto available = std::span(input).subspan(size);
    const auto count = ::read(STDIN_FILENO, available.data(), available.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    size += static_cast<std::size_t>(count);
    const std::string_view received(input.data(), size);
    const auto report = parse_geometry_report(received);
    const bool stopped = received.contains('q');
    if (report.has_value() || stopped) {
      return {.report = report, .stopped = stopped};
    }
  }
  return {};
}

// This full-screen peer detects only stable mismatches: if TIOCGWINSZ is unchanged across a
// Ghostty CSI 18 t response, the response must describe that same child-owned geometry.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_geometry_sync() noexcept -> int {
  if (!enter_raw_input() || !write_all("\x1B[?1049h\x1B[2J\x1B[H__LEMMA_GEOMETRY_SYNC_READY__")) {
    return 1;
  }
  std::this_thread::sleep_for(50ms);

  std::size_t samples = 0;
  std::size_t mismatches = 0;
  std::size_t missing_responses = 0;
  bool stopped = false;
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (!stopped && std::chrono::steady_clock::now() < deadline) {
    winsize before{};
    winsize after{};
    // ioctl is variadic because its third argument depends on the request.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &before) != 0 || !write_geometry_frame(before)) {
      return 1;
    }
    const auto response = read_geometry_response();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &after) != 0) {
      return 1;
    }
    stopped = response.stopped;
    if (!response.report.has_value()) {
      if (!stopped) {
        ++missing_responses;
      }
      continue;
    }
    ++samples;
    if (before.ws_row == after.ws_row && before.ws_col == after.ws_col &&
        (response.report->rows != before.ws_row || response.report->columns != before.ws_col)) {
      ++mismatches;
    }
  }

  std::array<char, 32> sample_text{};
  std::array<char, 32> mismatch_text{};
  std::array<char, 32> missing_text{};
  const auto encoded_samples =
      std::to_chars(sample_text.data(), std::to_address(sample_text.end()), samples);
  const auto encoded_mismatches =
      std::to_chars(mismatch_text.data(), std::to_address(mismatch_text.end()), mismatches);
  const auto encoded_missing =
      std::to_chars(missing_text.data(), std::to_address(missing_text.end()), missing_responses);
  const bool clean = stopped && samples > 0 && mismatches == 0 && missing_responses == 0;
  if (!stopped || samples == 0 || encoded_samples.ec != std::errc{} ||
      encoded_mismatches.ec != std::errc{} || encoded_missing.ec != std::errc{} ||
      !write_all(clean ? "\x1B[?1049l\r\n__GEOMETRY_SYNC_OK__\r\n"
                       : "\x1B[?1049l\r\n__GEOMETRY_SYNC_FAILED__\r\n") ||
      !write_all("__LEMMA_GEOMETRY_SYNC__ mismatches=") ||
      !write_all({mismatch_text.data(),
                  static_cast<std::size_t>(encoded_mismatches.ptr - mismatch_text.data())}) ||
      !write_all(" missing=") ||
      !write_all({missing_text.data(),
                  static_cast<std::size_t>(encoded_missing.ptr - missing_text.data())}) ||
      !write_all(" samples=") ||
      !write_all({sample_text.data(),
                  static_cast<std::size_t>(encoded_samples.ptr - sample_text.data())}) ||
      !write_all("\r\n")) {
    return 1;
  }
  std::this_thread::sleep_for(1s);
  return clean ? 0 : 1;
}

// Keep resize stress independent of interactive-shell job-control and signal timing.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_resize_flood() noexcept -> int {
  termios original_terminal{};
  if (::tcgetattr(STDIN_FILENO, &original_terminal) != 0) {
    return 1;
  }
  auto raw_terminal = original_terminal;
  ::cfmakeraw(&raw_terminal);
  // fcntl is variadic even when F_GETFL has no third argument.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const auto output_flags = ::fcntl(STDOUT_FILENO, F_GETFL);
  bool output_nonblocking = false;
  if (output_flags >= 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    output_nonblocking = ::fcntl(STDOUT_FILENO, F_SETFL, output_flags | O_NONBLOCK) == 0;
  }
  if (output_flags < 0 || ::tcsetattr(STDIN_FILENO, TCSANOW, &raw_terminal) != 0 ||
      !write_all("\r\n__LEMMA_RESIZE_FLOOD__\r\n") || !output_nonblocking) {
    return 1;
  }

  bool stopped = false;
  // This is only a leaked-peer lifetime guard; the test's existing assertion deadlines stay fixed.
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  while (!stopped && std::chrono::steady_clock::now() < deadline) {
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&event, 1, 1);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      break;
    }
    if (polled > 0 && (event.revents & POLLIN) != 0) {
      std::array<char, 64> input{};
      const auto count = ::read(STDIN_FILENO, input.data(), input.size());
      if (count > 0) {
        const auto received = std::span(input).first(static_cast<std::size_t>(count));
        stopped = std::ranges::find(received, 'q') != received.end();
      } else if (count == 0 || errno != EINTR) {
        break;
      }
    }
    if (!stopped && !write_background_line()) {
      break;
    }
  }

  // fcntl and ioctl are variadic because their final argument depends on the request.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const bool output_restored = ::fcntl(STDOUT_FILENO, F_SETFL, output_flags) == 0;
  const bool terminal_restored = ::tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal) == 0;
  winsize size{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const bool size_read = ::ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0;
  if (!stopped || !output_restored || !terminal_restored || !size_read) {
    return 1;
  }

  std::array<char, 16> rows{};
  std::array<char, 16> columns{};
  const auto encoded_rows = std::to_chars(rows.data(), std::to_address(rows.end()), size.ws_row);
  const auto encoded_columns =
      std::to_chars(columns.data(), std::to_address(columns.end()), size.ws_col);
  if (encoded_rows.ec != std::errc{} || encoded_columns.ec != std::errc{} ||
      !write_all("\r\n__LEMMA_RESIZE_FINAL__ ") ||
      !write_all({rows.data(), static_cast<std::size_t>(encoded_rows.ptr - rows.data())}) ||
      !write_all(" ") ||
      !write_all(
          {columns.data(), static_cast<std::size_t>(encoded_columns.ptr - columns.data())}) ||
      !write_all("\r\n")) {
    return 1;
  }
  linger_for_render();
  return 0;
}

enum class LatencyMode : std::uint8_t {
  idle,
  autonomous_output,
  tui_redraw,
};

[[nodiscard]] constexpr auto latency_ready_marker(const LatencyMode mode) noexcept
    -> std::string_view {
  switch (mode) {
  case LatencyMode::idle:
    return "\r\n__LEMMA_LATENCY_READY__\r\n";
  case LatencyMode::autonomous_output:
    return "\r\n__LEMMA_LATENCY_OUTPUT_READY__\r\n";
  case LatencyMode::tui_redraw:
    return "\x1B[?1049h\x1B[?2026h\x1B[2J\x1B[H"
           "__LEMMA_TUI_REDRAW_READY__\x1B[?2026l";
  }
  return {};
}

constexpr std::string_view latency_visible_ack = "__LEMMA_LATENCY_VISIBLE__";
constexpr std::string_view latency_next_ready = "__LEMMA_LATENCY_NEXT__";
constexpr std::string_view latency_peer_suffix = ".peer";

[[nodiscard]] auto bind_latency_peer(const int receipt,
                                     const std::string_view receipt_path) noexcept -> bool {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (receipt_path.size() + latency_peer_suffix.size() >= sizeof(address.sun_path)) {
    return false;
  }
  auto path = std::span(address.sun_path);
  std::memcpy(path.data(), receipt_path.data(), receipt_path.size());
  std::memcpy(path.subspan(receipt_path.size()).data(), latency_peer_suffix.data(),
              latency_peer_suffix.size());
  if (::unlink(path.data()) != 0 && errno != ENOENT) {
    return false;
  }
  // The socket ABI intentionally erases the concrete address type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* generic = reinterpret_cast<const sockaddr*>(&address);
  return ::bind(receipt, generic, sizeof(address)) == 0;
}

[[nodiscard]] auto wait_for_latency_visible_ack(const int receipt) noexcept -> bool {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd event{.fd = receipt, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&event, 1, 100);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled <= 0) {
      if (polled < 0) {
        return false;
      }
      continue;
    }
    std::array<char, 64> input{};
    const auto count = ::recv(receipt, input.data(), input.size(), 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return count >= 0 &&
           std::string_view(input.data(), static_cast<std::size_t>(count)) == latency_visible_ack;
  }
  return false;
}

[[nodiscard]] auto send_latency_next_ready(const int receipt) noexcept -> bool {
  const auto sent =
      ::send(receipt, latency_next_ready.data(), latency_next_ready.size(), MSG_NOSIGNAL);
  return sent >= 0 && static_cast<std::size_t>(sent) == latency_next_ready.size();
}

// This benchmark peer keeps each framed receipt and echo bounded and explicit.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_latency(const std::string_view receipt_path,
                               const LatencyMode mode = LatencyMode::idle) noexcept -> int {
  if (receipt_path.empty()) {
    return 1;
  }
  const bool autonomous_output = mode == LatencyMode::autonomous_output;
  const bool tui_redraw = mode == LatencyMode::tui_redraw;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (receipt_path.size() >= sizeof(address.sun_path)) {
    return 1;
  }
  std::memcpy(std::span(address.sun_path).data(), receipt_path.data(), receipt_path.size());
  const int receipt = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  if (receipt < 0 || (autonomous_output && !bind_latency_peer(receipt, receipt_path))) {
    if (receipt >= 0) {
      static_cast<void>(::close(receipt));
    }
    return 1;
  }
  // The socket ABI intentionally erases the concrete address type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* generic = reinterpret_cast<const sockaddr*>(&address);
  const auto ready = latency_ready_marker(mode);
  const auto frame_geometry = tui_redraw ? tui_frame_geometry() : std::nullopt;
  if ((tui_redraw && !frame_geometry.has_value()) ||
      ::connect(receipt, generic, sizeof(address)) != 0 || !enter_raw_input() ||
      !write_all(ready)) {
    static_cast<void>(::close(receipt));
    return 1;
  }
  // Readiness is setup, not a sample. Do not begin autonomous output until the harness has
  // observed the marker; a fixed delay can let a contended renderer lose it before its next frame.
  if (autonomous_output && (!wait_for_latency_visible_ack(receipt) || !make_output_nonblocking() ||
                            !send_latency_next_ready(receipt))) {
    static_cast<void>(::close(receipt));
    return 1;
  }

  std::array<char, 128> marker{};
  std::size_t marker_size = 0;
  std::size_t tui_sequence = 0;
  auto deadline = std::chrono::steady_clock::now() + 120s;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    // Keep autonomous output near 1 kHz, but block on input until the next output deadline. A
    // zero-time poll followed by sleep made token acknowledgement wait for the sleep and measured
    // fixture scheduling rather than mux latency.
    const auto polled = ::poll(&event, 1, autonomous_output ? 1 : 100);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      static_cast<void>(::close(receipt));
      return 1;
    }
    if (polled == 0) {
      if (autonomous_output && !write_background_line()) {
        static_cast<void>(::close(receipt));
        return 1;
      }
      continue;
    }
    std::array<char, 128> input{};
    const auto count = ::read(STDIN_FILENO, input.data(), input.size());
    if (count == 0) {
      static_cast<void>(::close(receipt));
      return 0;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      static_cast<void>(::close(receipt));
      return 1;
    }
    deadline = std::chrono::steady_clock::now() + 120s;
    bool marker_echo_written = false;
    for (const char byte : std::span(input).first(static_cast<std::size_t>(count))) {
      if (byte != '\n') {
        if (marker_size == marker.size()) {
          static_cast<void>(::close(receipt));
          return 1;
        }
        std::span(marker).subspan(marker_size, 1).front() = byte;
        ++marker_size;
        continue;
      }
      if (marker_size == 0) {
        static_cast<void>(::close(receipt));
        return 1;
      }
      const auto frame = std::string_view(marker.data(), marker_size);
      const auto sent = ::send(receipt, frame.data(), frame.size(), MSG_NOSIGNAL);
      if (sent < 0) {
        static_cast<void>(::close(receipt));
        return 1;
      }
      const auto sent_size = static_cast<std::size_t>(sent);
      bool output_written = false;
      if (autonomous_output) {
        const auto visibility_deadline = std::chrono::steady_clock::now() + 1s;
        output_written = write_all_until(frame, visibility_deadline) &&
                         write_all_until("\r\n", visibility_deadline);
      } else if (tui_redraw) {
        output_written = write_tui_frame(frame, tui_sequence, *frame_geometry);
        ++tui_sequence;
      } else {
        output_written = write_all(frame) && write_all("\r\n");
      }
      if (sent_size != marker_size || !output_written) {
        static_cast<void>(::close(receipt));
        return 1;
      }
      marker_size = 0;
      marker_echo_written = true;
    }
    if (autonomous_output && marker_echo_written) {
      // Keep output paused until the exact token is observable. This barrier is outside the
      // measured interval and prevents a delayed renderer from losing the token to later rows.
      if (!wait_for_latency_visible_ack(receipt) || !send_latency_next_ready(receipt)) {
        static_cast<void>(::close(receipt));
        return 1;
      }
    } else if (autonomous_output && !write_background_line()) {
      static_cast<void>(::close(receipt));
      return 1;
    }
  }
  static_cast<void>(::close(receipt));
  return 1;
}

volatile std::sig_atomic_t winch_observed = 0;

extern "C" void observe_winch(int signal_number) noexcept;

extern "C" void observe_winch([[maybe_unused]] const int signal_number) noexcept {
  winch_observed = 1;
}

// The fixture explicitly handles each signal, readiness, and bounded I/O outcome.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_winch() noexcept -> int {
  struct sigaction action{};
  action.sa_handler = &observe_winch;
  if (sigemptyset(&action.sa_mask) != 0 || ::sigaction(SIGWINCH, &action, nullptr) != 0 ||
      !enter_raw_input() || !write_all("__LEMMA_WINCH_READY__\r\n")) {
    return 1;
  }
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (winch_observed != 0) {
      winch_observed = 0;
      winsize size{};
      // ioctl is variadic because its third argument depends on the request.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &size) != 0) {
        return 1;
      }
      std::array<char, 16> rows{};
      std::array<char, 16> columns{};
      const auto encoded_rows =
          std::to_chars(rows.data(), std::to_address(rows.end()), size.ws_row);
      const auto encoded_columns =
          std::to_chars(columns.data(), std::to_address(columns.end()), size.ws_col);
      if (encoded_rows.ec != std::errc{} || encoded_columns.ec != std::errc{} ||
          !write_all("__LEMMA_WINCH_") ||
          !write_all({rows.data(), static_cast<std::size_t>(encoded_rows.ptr - rows.data())}) ||
          !write_all("_") ||
          !write_all(
              {columns.data(), static_cast<std::size_t>(encoded_columns.ptr - columns.data())}) ||
          !write_all("__\r\n")) {
        return 1;
      }
    }
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&event, 1, 100);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return 1;
    }
    if (polled > 0) {
      std::array<char, 16> input{};
      const auto count = ::read(STDIN_FILENO, input.data(), input.size());
      if (count <= 0) {
        return count == 0 ? 0 : 1;
      }
      if (std::string_view(input.data(), static_cast<std::size_t>(count)).contains('q')) {
        return 0;
      }
    }
  }
  return 1;
}

int winch_pipe_write = -1;

extern "C" void forward_winch(int signal_number) noexcept;

extern "C" void forward_winch([[maybe_unused]] const int signal_number) noexcept {
  const auto saved_errno = errno;
  static_cast<void>(::write(winch_pipe_write, "W", 1));
  errno = saved_errno;
}

struct PaintSockets final {
  int control{-1};
  int winch_read{-1};
  int winch_write{-1};

  PaintSockets() = default;
  PaintSockets(const PaintSockets&) = delete;
  PaintSockets(PaintSockets&&) = delete;
  auto operator=(const PaintSockets&) -> PaintSockets& = delete;
  auto operator=(PaintSockets&&) -> PaintSockets& = delete;
  ~PaintSockets() {
    for (const int descriptor : {control, winch_read, winch_write}) {
      if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
      }
    }
  }
};

[[nodiscard]] auto unix_address(const std::string_view path, sockaddr_un& address) noexcept
    -> bool {
  address = {};
  address.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(address.sun_path)) {
    return false;
  }
  std::memcpy(std::span(address.sun_path).data(), path.data(), path.size());
  return true;
}

[[nodiscard]] auto open_paint_control(const std::string_view receipt_path,
                                      const std::string_view control_path,
                                      PaintSockets& sockets) noexcept -> bool {
  sockaddr_un receipt{};
  sockaddr_un control{};
  if (!unix_address(receipt_path, receipt) || !unix_address(control_path, control)) {
    return false;
  }
  sockets.control = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sockets.control < 0 ||
      (::unlink(std::span(control.sun_path).data()) != 0 && errno != ENOENT)) {
    return false;
  }
  // The socket ABI intentionally erases the concrete address type.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  return ::bind(sockets.control, reinterpret_cast<const sockaddr*>(&control), sizeof(control)) ==
             0 &&
         ::connect(sockets.control, reinterpret_cast<const sockaddr*>(&receipt), sizeof(receipt)) ==
             0;
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

[[nodiscard]] auto install_winch_pipe(PaintSockets& sockets) noexcept -> bool {
  std::array<int, 2> descriptors{-1, -1};
  if (::pipe(descriptors.data()) != 0) {
    return false;
  }
  sockets.winch_read = descriptors.front();
  sockets.winch_write = descriptors.back();
  for (const int descriptor : descriptors) {
    // fcntl is variadic even when F_GETFL has no third argument.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const auto flags = ::fcntl(descriptor, F_GETFL);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
      return false;
    }
  }
  winch_pipe_write = sockets.winch_write;
  struct sigaction action{};
  action.sa_handler = &forward_winch;
  return sigemptyset(&action.sa_mask) == 0 && ::sigaction(SIGWINCH, &action, nullptr) == 0;
}

[[nodiscard]] auto send_geometry_receipt(const int control, const std::string_view marker,
                                         const winsize size) noexcept -> bool {
  std::array<char, 192> receipt{};
  std::span<char> remaining = receipt;
  if (!append_text(remaining, marker) || !append_text(remaining, " ") ||
      !append_number(remaining, size.ws_col) || !append_text(remaining, "x") ||
      !append_number(remaining, size.ws_row)) {
    return false;
  }
  const auto bytes = receipt.size() - remaining.size();
  const auto sent = ::send(control, receipt.data(), bytes, MSG_NOSIGNAL);
  return std::cmp_equal(sent, bytes);
}

// A painter owns one full-screen alternate buffer and receives unique markers over a datagram
// control socket, so the harness can prepare a screen that is not currently visible. In paint
// mode each marker is drawn immediately. In resize mode a marker only arms the painter; the next
// SIGWINCH repaints at the new geometry and reports that geometry before drawing.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_paint(const std::string_view receipt_path,
                             const std::string_view control_path, const bool resize) noexcept
    -> int {
  PaintSockets sockets;
  constexpr std::string_view paint_ready = "\x1B[?1049h\x1B[?2026h\x1B[2J\x1B[H"
                                           "__LEMMA_PAINT_READY__\x1B[?2026l";
  constexpr std::string_view resize_ready = "\x1B[?1049h\x1B[?2026h\x1B[2J\x1B[H"
                                            "__LEMMA_RESIZE_READY__\x1B[?2026l";
  constexpr std::string_view armed_receipt = "__LEMMA_RESIZE_ARMED__";
  if (!open_paint_control(receipt_path, control_path, sockets) ||
      (resize && !install_winch_pipe(sockets)) || !enter_raw_input() ||
      !write_all(resize ? resize_ready : paint_ready)) {
    return 1;
  }

  // Each control datagram is one fill letter followed by the marker. The harness chooses fills so
  // consecutive frames differ in every cell, preventing a differential renderer from retaining an
  // unchanged screen body in only some samples.
  std::array<char, 129> control{};
  std::array<char, 128> marker{};
  std::size_t marker_size = 0;
  std::size_t fill = 0;
  auto deadline = std::chrono::steady_clock::now() + 120s;
  while (std::chrono::steady_clock::now() < deadline) {
    std::array events{
        pollfd{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        pollfd{.fd = sockets.control, .events = POLLIN, .revents = 0},
        pollfd{.fd = sockets.winch_read, .events = POLLIN, .revents = 0},
    };
    const auto polled = ::poll(events.data(), resize ? 3U : 2U, 100);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return 1;
    }
    if ((events.front().revents & (POLLIN | POLLHUP)) != 0) {
      std::array<char, 256> input{};
      const auto count = ::read(STDIN_FILENO, input.data(), input.size());
      if (count == 0 || (count < 0 && errno == EIO)) {
        return 0;
      }
      if (count < 0 && errno != EINTR && errno != EAGAIN) {
        return 1;
      }
    }
    if ((events.at(1).revents & POLLIN) != 0) {
      const auto count = ::recv(sockets.control, control.data(), control.size(), 0);
      if (count < 2 || control.front() < 'a' || control.front() > 'z') {
        return 1;
      }
      fill = static_cast<std::size_t>(control.front() - 'a');
      marker_size = static_cast<std::size_t>(count) - 1U;
      std::ranges::copy(std::span(control).subspan(1, marker_size), marker.begin());
      deadline = std::chrono::steady_clock::now() + 120s;
      const auto received = std::string_view(marker.data(), marker_size);
      if (resize) {
        const auto sent =
            ::send(sockets.control, armed_receipt.data(), armed_receipt.size(), MSG_NOSIGNAL);
        if (sent < 0 || static_cast<std::size_t>(sent) != armed_receipt.size()) {
          return 1;
        }
        continue;
      }
      const auto geometry = tui_frame_geometry();
      if (!geometry.has_value() || !write_tui_frame(received, fill, *geometry)) {
        return 1;
      }
      const auto sent = ::send(sockets.control, received.data(), received.size(), MSG_NOSIGNAL);
      if (sent < 0 || static_cast<std::size_t>(sent) != received.size()) {
        return 1;
      }
      marker_size = 0;
    }
    if (resize && (events.back().revents & POLLIN) != 0) {
      std::array<char, 64> signals{};
      while (::read(sockets.winch_read, signals.data(), signals.size()) > 0) {
      }
      if (marker_size == 0) {
        continue;
      }
      winsize size{};
      const auto geometry = tui_frame_geometry();
      const auto received = std::string_view(marker.data(), marker_size);
      // ioctl is variadic because its third argument depends on the request.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || !geometry.has_value() ||
          !send_geometry_receipt(sockets.control, received, size) ||
          !write_tui_frame(received, fill, *geometry)) {
        return 1;
      }
      marker_size = 0;
    }
  }
  return 1;
}

[[nodiscard]] auto run_attach_visible(const std::string_view ready_path = {}) noexcept -> int {
  if (!write_all("__LEMMA_ATTACH_VISIBLE__\r\n")) {
    return 1;
  }
  if (!ready_path.empty()) {
    std::array<char, 1'024> path{};
    if (ready_path.size() >= path.size()) {
      return 1;
    }
    std::ranges::copy(ready_path, path.begin());
    // open is variadic when O_CREAT supplies a mode.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const auto ready = ::open(path.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (ready < 0) {
      return 1;
    }
    const bool announced = ::write(ready, "1", 1) == 1;
    const bool closed = ::close(ready) == 0;
    if (!announced || !closed) {
      return 1;
    }
  }
  // Keep the fixture in the foreground so a shell prompt cannot mutate the retained attach frame.
  std::this_thread::sleep_for(120s);
  return 0;
}

// The branches are the explicit bounded states of the logical-key peer.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_logical_keys() noexcept -> int {
  if (!enter_raw_input() || !write_all("__READY__")) {
    return 1;
  }
  std::array<char, 2> input{};
  std::size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (offset < input.size() && std::chrono::steady_clock::now() < deadline) {
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&event, 1, 10);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled <= 0) {
      continue;
    }
    auto available = std::span(input).subspan(offset);
    const auto count = ::read(STDIN_FILENO, available.data(), available.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return 1;
    }
    offset += static_cast<std::size_t>(count);
  }
  constexpr std::array expected{'A', '\x01'};
  const bool matched = offset == input.size() && std::ranges::equal(input, expected);
  if (!write_all(matched ? "\r\n__BYTES_4101__\r\n" : "\r\n__BYTES_FAILED__\r\n")) {
    return 1;
  }
  linger_for_render();
  return matched ? 0 : 1;
}

[[nodiscard]] auto run_delayed_exit() noexcept -> int {
  std::this_thread::sleep_for(250ms);
  return 4;
}

// The branches are the explicit bounded states of the quiescent peer.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_idle() noexcept -> int {
  if (!enter_raw_input() || !write_all("__LEMMA_IDLE_READY__\r\n")) {
    return 1;
  }
  while (true) {
    pollfd event{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&event, 1, -1);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return 1;
    }
    std::array<char, 256> input{};
    const auto count = ::read(STDIN_FILENO, input.data(), input.size());
    if (count == 0) {
      return 0;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return errno == EIO ? 0 : 1;
    }
  }
}

[[nodiscard]] auto
run_warm_scroll(const std::string_view marker = "__LEMMA_WARM_SCROLL_DONE__") noexcept -> int {
  std::array<char, 81> line{};
  line.fill('x');
  std::span(line).subspan(79, 1).front() = '\r';
  std::span(line).subspan(80, 1).front() = '\n';
  for (std::size_t index = 0; index < 25'000; ++index) {
    if (!write_all({line.data(), line.size()})) {
      return 1;
    }
  }
  return write_all(marker) && write_all("\r\n") ? 0 : 1;
}

[[nodiscard]] auto run_output_flood() noexcept -> int {
  // Fill the 500-column blocked-client screen without wrapping. Unlike a
  // repeated short line, dense changing rows cannot settle into an unchanged
  // screen or collapse into blank-tail erases before the socket saturates.
  if (!make_output_nonblocking()) {
    return 1;
  }
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  std::array<char, 501> line{};
  std::span(line).subspan(499, 1).front() = '\r';
  std::span(line).subspan(500, 1).front() = '\n';
  std::uint32_t state = 1;
  while (std::chrono::steady_clock::now() < deadline) {
    for (auto& cell : std::span(line).first(499)) {
      state = (state * 1'664'525U) + 1'013'904'223U;
      cell = static_cast<char>(33U + ((state >> 16U) % 94U));
    }
    if (!write_all_until({line.data(), line.size()}, deadline)) {
      return 1;
    }
  }
  return 0;
}

// NOLINTBEGIN(readability-function-cognitive-complexity)
[[nodiscard]] auto run_warm_scroll_loop() noexcept -> int {
  if (!write_all("__LEMMA_WARM_SCROLL_READY__\r\n")) {
    return 1;
  }
  std::size_t iteration = 0;
  std::array<char, 1> trigger{};
  while (true) {
    const auto count = ::read(STDIN_FILENO, trigger.data(), trigger.size());
    if (count > 0) {
      std::array<char, 64> marker{};
      constexpr std::string_view prefix = "__LEMMA_WARM_SCROLL_DONE__";
      std::ranges::copy(prefix, marker.begin());
      // std::to_chars expresses its bounded output range as a pointer pair.
      // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const auto encoded = std::to_chars(marker.begin() + prefix.size(), marker.end(), iteration++);
      if (encoded.ec != std::errc{} || encoded.ptr == marker.end()) {
        return 1;
      }
      *encoded.ptr = '|';
      // encoded.ptr is within marker's to_chars range and leaves room for the delimiter.
      const std::string_view marker_view(
          marker.data(), static_cast<std::size_t>(encoded.ptr + 1 - marker.begin()));
      // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      if (run_warm_scroll(marker_view) != 0) {
        return 1;
      }
      continue;
    }
    if (count == 0 || errno != EINTR) {
      return count == 0 || errno == EIO ? 0 : 1;
    }
  }
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace

// Command dispatch is deliberately explicit so invalid argument shapes remain rejected.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int main(const int argc, char** const argv) {
  const auto arguments = std::span(argv, static_cast<std::size_t>(argc));
  if (arguments.size() == 2 &&
      std::string_view(arguments.subspan(1, 1).front()) == "output-flood") {
    return run_output_flood();
  }
  if (arguments.size() == 2 &&
      std::string_view(arguments.subspan(1, 1).front()) == "attach-visible") {
    return run_attach_visible();
  }
  if (arguments.size() == 3 &&
      std::string_view(arguments.subspan(1, 1).front()) == "attach-visible") {
    return run_attach_visible(arguments.subspan(2, 1).front());
  }
  if (arguments.size() == 2 && std::string_view(arguments.subspan(1, 1).front()) == "warm-scroll") {
    return run_warm_scroll();
  }
  if (arguments.size() == 2 &&
      std::string_view(arguments.subspan(1, 1).front()) == "warm-scroll-loop") {
    return run_warm_scroll_loop();
  }
  if (arguments.size() == 2 &&
      std::string_view(arguments.subspan(1, 1).front()) == "logical-keys") {
    return run_logical_keys();
  }
  if (arguments.size() == 2 &&
      std::string_view(arguments.subspan(1, 1).front()) == "delayed-exit") {
    return run_delayed_exit();
  }
  if (arguments.size() == 2 && std::string_view(arguments.subspan(1, 1).front()) == "idle") {
    return run_idle();
  }
  if (arguments.size() == 2 && std::string_view(arguments.subspan(1, 1).front()) == "winch") {
    return run_winch();
  }
  if (arguments.size() == 2 &&
      std::string_view(arguments.subspan(1, 1).front()) == "resize-flood") {
    return run_resize_flood();
  }
  if (arguments.size() == 2 &&
      std::string_view(arguments.subspan(1, 1).front()) == "geometry-sync") {
    return run_geometry_sync();
  }
  if (arguments.size() == 3 && std::string_view(arguments.subspan(1, 1).front()) == "latency") {
    return run_latency(arguments.subspan(2, 1).front());
  }
  if (arguments.size() == 3 &&
      std::string_view(arguments.subspan(1, 1).front()) == "latency-output") {
    return run_latency(arguments.subspan(2, 1).front(), LatencyMode::autonomous_output);
  }
  if (arguments.size() == 3 && std::string_view(arguments.subspan(1, 1).front()) == "latency-tui") {
    return run_latency(arguments.subspan(2, 1).front(), LatencyMode::tui_redraw);
  }
  if (arguments.size() == 4 &&
      std::string_view(arguments.subspan(1, 1).front()) == "latency-tui-wheel") {
    return run_tui_wheel(arguments.subspan(2, 1).front(),
                         parse_size(arguments.subspan(3, 1).front()));
  }
  if ((arguments.size() == 4 || arguments.size() == 5) &&
      std::string_view(arguments.subspan(1, 1).front()) == "block") {
    auto idle_timeout = block_receive_idle_timeout;
    if (arguments.size() == 5) {
      // A shorter guard lets process tests exercise progress and stalling without
      // waiting thirty seconds. Callers cannot raise the normal fixture bound.
      const auto milliseconds = parse_size(arguments.subspan(4, 1).front());
      if (milliseconds == 0 ||
          milliseconds > static_cast<std::size_t>(block_receive_idle_timeout.count())) {
        return 2;
      }
      idle_timeout = std::chrono::milliseconds{static_cast<std::int64_t>(milliseconds)};
    }
    return run_block(arguments.subspan(2, 1).front(), parse_size(arguments.subspan(3, 1).front()),
                     idle_timeout);
  }
  if (arguments.size() == 4 && std::string_view(arguments.subspan(1, 1).front()) == "paint") {
    return run_paint(arguments.subspan(2, 1).front(), arguments.subspan(3, 1).front(), false);
  }
  if (arguments.size() == 4 &&
      std::string_view(arguments.subspan(1, 1).front()) == "resize-paint") {
    return run_paint(arguments.subspan(2, 1).front(), arguments.subspan(3, 1).front(), true);
  }
  if (arguments.size() == 4 && std::string_view(arguments.subspan(1, 1).front()) == "order") {
    return run_order(arguments.subspan(2, 1).front(), arguments.subspan(3, 1).front());
  }
  return 2;
}
