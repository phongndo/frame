#include "diagnostic/latency_trace.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

namespace {

constexpr std::string_view latency_visible_ack = "__LEMMA_LATENCY_VISIBLE__";
constexpr std::string_view latency_next_ready = "__LEMMA_LATENCY_NEXT__";
constexpr std::string_view tui_wheel_armed = "__LEMMA_TUI_WHEEL_ARMED__";
constexpr std::string_view wheel_report = "\x1B[<64;10;10M";
constexpr std::size_t read_bytes_max = std::size_t{64} * 1'024U;
constexpr std::size_t repetitions_max = 10'000;
constexpr auto interaction_timeout = std::chrono::seconds(5);
constexpr auto drain_duration = std::chrono::milliseconds(10);
constexpr std::uint64_t open_loop_initial_delay_ns = 20'000'000U;

struct InteractionResult final {
  std::uint64_t key_to_pty_ns{0};
  std::uint64_t key_to_outer_bytes_ns{0};
  std::uint64_t outer_bytes{0};
};

[[nodiscard]] auto monotonic_ns() noexcept -> std::uint64_t {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

[[nodiscard]] constexpr auto open_loop_send_window(const std::size_t repetitions,
                                                   const std::uint64_t interval_ns) noexcept
    -> std::chrono::nanoseconds {
  const auto jitter_span = std::max<std::uint64_t>(1U, interval_ns / 4U);
  const auto maximum_interval_ns = interval_ns - (jitter_span / 2U) + jitter_span - 1U;
  const auto scheduled_intervals = repetitions == 0 ? 0U : repetitions - 1U;
  const auto maximum_window_ns =
      open_loop_initial_delay_ns +
      (maximum_interval_ns * static_cast<std::uint64_t>(scheduled_intervals));
  return std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(maximum_window_ns));
}

[[nodiscard]] auto parse_integer(const std::string_view encoded, const int minimum,
                                 const int maximum) noexcept -> int {
  int value = 0;
  const auto* const end = std::to_address(encoded.end());
  // from_chars consumes the explicit half-open range and never expects termination.
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const auto parsed = std::from_chars(encoded.data(), end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum || value > maximum) {
    return -1;
  }
  return value;
}

// Bounded descriptor progress deliberately keeps every write outcome explicit.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto write_all(const int descriptor, const std::string_view bytes,
                             const std::chrono::steady_clock::time_point deadline) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto remaining = std::span(bytes).subspan(offset);
    const auto written = ::write(descriptor, remaining.data(), remaining.size());
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      pollfd writable{.fd = descriptor, .events = POLLOUT, .revents = 0};
      const auto polled = ::poll(&writable, 1, 20);
      if (polled < 0 && errno == EINTR) {
        continue;
      }
      if (polled >= 0) {
        continue;
      }
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto interaction_label_index(const std::string_view label_code) noexcept
    -> std::size_t {
  constexpr std::size_t radix = 26U;
  constexpr std::size_t label_slots = radix * radix;
  if (label_code.size() != 3U) {
    return label_slots;
  }
  auto suffix = label_code;
  suffix.remove_prefix(1);
  const auto middle = suffix.front();
  const auto last = suffix.back();
  if (label_code.front() != 'L' || middle < 'A' || middle > 'Z' || last < 'A' || last > 'Z') {
    return label_slots;
  }
  return (static_cast<std::size_t>(middle - 'A') * radix) + static_cast<std::size_t>(last - 'A');
}

[[nodiscard]] auto encoded_index_token(const std::string_view label_code, const std::size_t index)
    -> std::string {
  constexpr std::size_t radix = 26U;
  // Six base-26 digits and the base-26 value 111111 respectively.
  constexpr std::size_t token_space = 308'915'776U;
  constexpr std::size_t redraw_multiplier = 12'356'631U;
  const auto label_index = interaction_label_index(label_code);
  if (index >= repetitions_max || label_index >= radix * radix) {
    return {};
  }
  std::array<char, 6> encoded_index{};
  const auto interaction_index = (label_index * repetitions_max) + index;
  auto remaining = (interaction_index * redraw_multiplier) % token_space;
  for (auto& position : std::views::reverse(encoded_index)) {
    position = static_cast<char>('A' + static_cast<char>(remaining % radix));
    remaining /= radix;
  }
  return {encoded_index.data(), encoded_index.size()};
}

[[nodiscard]] auto interaction_marker(const std::string_view label,
                                      const std::string_view label_code, const std::size_t index)
    -> std::string {
  const auto token = encoded_index_token(label_code, index);
  if (token.empty()) {
    return {};
  }
  std::array<char, 4> digits{};
  auto value = index;
  for (auto& digit : std::views::reverse(digits)) {
    digit = static_cast<char>('0' + static_cast<char>(value % 10U));
    value /= 10U;
  }
  if (value != 0) {
    return {};
  }
  std::string marker("__LEMMA_");
  marker.append(label);
  marker.push_back('_');
  marker.append(digits.data(), digits.size());
  marker.push_back('_');
  marker.append(token);
  marker.append("__");
  return marker;
}

class OuterTextDecoder final {
public:
  void append(const std::span<const char> bytes, std::string& output) {
    for (const auto byte : bytes) {
      consume(static_cast<unsigned char>(byte), output);
    }
  }

private:
  enum class State : std::uint8_t {
    text,
    escape,
    csi,
    control_string,
    control_string_escape,
  };

  void consume_text(const unsigned char byte, std::string& output) {
    if (byte == 0x1BU) {
      state_ = State::escape;
    } else if (byte >= 0x20U && byte != 0x7FU) {
      output.push_back(static_cast<char>(byte));
    }
  }

  void consume_escape(const unsigned char byte) noexcept {
    if (byte == '[') {
      state_ = State::csi;
    } else if (byte == ']' || byte == 'P' || byte == '_' || byte == '^' || byte == 'X') {
      state_ = State::control_string;
    } else if (byte >= 0x30U && byte <= 0x7EU) {
      state_ = State::text;
    }
  }

  void consume_control_string(const unsigned char byte) noexcept {
    if (byte == 0x07U) {
      state_ = State::text;
    } else if (byte == 0x1BU) {
      state_ = State::control_string_escape;
    }
  }

  void consume(const unsigned char byte, std::string& output) {
    switch (state_) {
    case State::text:
      consume_text(byte, output);
      return;
    case State::escape:
      consume_escape(byte);
      return;
    case State::csi:
      if (byte >= 0x40U && byte <= 0x7EU) {
        state_ = State::text;
      }
      return;
    case State::control_string:
      consume_control_string(byte);
      return;
    case State::control_string_escape:
      state_ = byte == '\\' ? State::text : State::control_string;
      return;
    }
  }

  State state_{State::text};
};

[[nodiscard]] auto receive_datagram(const int descriptor, std::string& output) -> bool {
  std::array<char, std::size_t{4} * 1'024U> bytes{};
  while (true) {
    const auto received = ::recv(descriptor, bytes.data(), bytes.size(), 0);
    if (received >= 0) {
      output.assign(bytes.data(), static_cast<std::size_t>(received));
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    return false;
  }
}

[[nodiscard]] auto wait_for_receipt(const int receipt_descriptor, const std::string_view expected,
                                    const std::chrono::steady_clock::time_point deadline) -> bool {
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd readable{.fd = receipt_descriptor, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&readable, 1, 20);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return false;
    }
    if (polled == 0) {
      continue;
    }
    std::string received;
    return receive_datagram(receipt_descriptor, received) && received == expected;
  }
  return false;
}

[[nodiscard]] auto acknowledge_visible(const int receipt_descriptor,
                                       const std::string_view peer_path) noexcept -> bool {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (peer_path.empty() || peer_path.size() >= sizeof(address.sun_path)) {
    return false;
  }
  std::ranges::copy(peer_path, std::span(address.sun_path).begin());
  // The socket ABI intentionally erases the concrete address type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* generic = reinterpret_cast<const sockaddr*>(&address);
  const auto sent = ::sendto(receipt_descriptor, latency_visible_ack.data(),
                             latency_visible_ack.size(), 0, generic, sizeof(address));
  return sent >= 0 && static_cast<std::size_t>(sent) == latency_visible_ack.size();
}

void drain_outer(const int descriptor) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + drain_duration;
  std::array<char, read_bytes_max> bytes{};
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd readable{.fd = descriptor, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&readable, 1, 1);
    if (polled <= 0) {
      if (polled < 0 && errno == EINTR) {
        continue;
      }
      continue;
    }
    while (::read(descriptor, bytes.data(), bytes.size()) > 0) {
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EIO) {
      return;
    }
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto read_outer_marker(const int outer_descriptor, const std::string_view marker,
                                     const std::uint64_t started_ns)
    -> std::pair<std::uint64_t, std::uint64_t> {
  const auto deadline = std::chrono::steady_clock::now() + interaction_timeout;
  std::string retained;
  retained.reserve(marker.size() + read_bytes_max);
  std::array<char, read_bytes_max> output{};
  OuterTextDecoder decoder;
  std::uint64_t outer_bytes = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd readable{.fd = outer_descriptor, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&readable, 1, 20);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return {};
    }
    if (polled == 0) {
      continue;
    }
    const auto ready_ns = monotonic_ns();
    while (true) {
      const auto count = ::read(outer_descriptor, output.data(), output.size());
      if (count > 0) {
        const auto size = static_cast<std::size_t>(count);
        outer_bytes += size;
        decoder.append(std::span(output).first(size), retained);
        if (retained.contains(marker)) {
          return {ready_ns - started_ns, outer_bytes};
        }
        if (retained.size() > marker.size() + read_bytes_max) {
          retained.erase(0, retained.size() - marker.size() - read_bytes_max);
        }
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      return {};
    }
  }
  constexpr std::size_t diagnostic_bytes = 512U;
  const auto tail =
      retained.substr(retained.size() > diagnostic_bytes ? retained.size() - diagnostic_bytes : 0U);
  std::cerr << "outer marker timeout: expected=" << std::quoted(std::string(marker))
            << " outer_bytes=" << outer_bytes << " decoded_tail=" << std::quoted(tail) << '\n';
  return {};
}

// A receipt suffix, when requested, carries fixture-observed state after "MARKER ".
// NOLINTBEGIN(readability-function-cognitive-complexity)
[[nodiscard]] auto
read_interaction(const int outer_descriptor, const int receipt_descriptor,
                 const std::string_view receipt_marker, const std::string_view output_token,
                 const std::uint64_t started_ns, std::string* const receipt_suffix = nullptr)
    -> InteractionResult {
  InteractionResult result;
  std::string retained;
  retained.reserve(output_token.size() + read_bytes_max);
  const auto deadline = std::chrono::steady_clock::now() + interaction_timeout;
  std::array<char, read_bytes_max> output{};
  OuterTextDecoder decoder;

  while (std::chrono::steady_clock::now() < deadline) {
    std::array events{
        pollfd{.fd = receipt_descriptor, .events = POLLIN, .revents = 0},
        pollfd{.fd = outer_descriptor, .events = POLLIN, .revents = 0},
    };
    const auto polled = ::poll(events.data(), events.size(), 20);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return {};
    }
    if (polled == 0) {
      continue;
    }
    const auto ready_ns = monotonic_ns();

    if ((events.front().revents & POLLIN) != 0) {
      std::string received;
      if (!receive_datagram(receipt_descriptor, received) || result.key_to_pty_ns != 0) {
        return {};
      }
      if (receipt_suffix == nullptr) {
        if (received != receipt_marker) {
          return {};
        }
      } else {
        const auto prefix_size = receipt_marker.size() + 1U;
        if (received.size() <= prefix_size || !received.starts_with(receipt_marker) ||
            received.at(receipt_marker.size()) != ' ') {
          return {};
        }
        receipt_suffix->assign(received, prefix_size);
      }
      result.key_to_pty_ns = ready_ns - started_ns;
    }

    if ((events.back().revents & (POLLIN | POLLHUP)) != 0) {
      while (true) {
        const auto count = ::read(outer_descriptor, output.data(), output.size());
        if (count > 0) {
          const auto size = static_cast<std::size_t>(count);
          result.outer_bytes += size;
          decoder.append(std::span(output).first(size), retained);
          if (result.key_to_outer_bytes_ns == 0 && retained.contains(output_token)) {
            result.key_to_outer_bytes_ns = ready_ns - started_ns;
          }
          if (retained.size() > output_token.size() + read_bytes_max) {
            retained.erase(0, retained.size() - output_token.size() - read_bytes_max);
          }
          continue;
        }
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          break;
        }
        return {};
      }
    }

    if (result.key_to_pty_ns != 0 && result.key_to_outer_bytes_ns != 0) {
      return result;
    }
  }
  return {};
}
// NOLINTEND(readability-function-cognitive-complexity)

[[nodiscard]] auto percentile(std::vector<std::uint64_t> samples, const double quantile)
    -> std::uint64_t {
  std::ranges::sort(samples);
  const auto rank = static_cast<std::size_t>(
      std::max(1.0, std::ceil(quantile * static_cast<double>(samples.size()))));
  return samples.at(std::min(samples.size() - 1U, rank - 1U));
}

void print_samples(const std::span<const std::uint64_t> samples) {
  std::cout << '[';
  for (std::size_t index = 0; index < samples.size(); ++index) {
    if (index != 0) {
      std::cout << ',';
    }
    std::cout << std::span(samples).subspan(index, 1).front();
  }
  std::cout << ']';
}

void print_summary(const std::string_view label, const std::vector<std::uint64_t>& samples) {
  std::cout << '"' << label << R"(":{"samples_ns":)";
  print_samples(samples);
  std::cout << R"(,"p50_ns":)" << percentile(samples, 0.50) << R"(,"p95_ns":)"
            << percentile(samples, 0.95) << R"(,"p99_ns":)" << percentile(samples, 0.99)
            << R"(,"p95_valid":)" << (samples.size() >= 20U ? "true" : "false")
            << R"(,"p99_valid":)" << (samples.size() >= 100U ? "true" : "false") << '}';
}

void stop_child(const int descriptor, const pid_t child) noexcept {
  static_cast<void>(::close(descriptor));
  if (::kill(child, SIGHUP) != 0 && errno != ESRCH) {
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child || (waited < 0 && errno == ECHILD)) {
      return;
    }
    if (waited < 0 && errno != EINTR) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (::kill(child, SIGKILL) != 0 && errno != ESRCH) {
    return;
  }
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
}

[[nodiscard]] auto decode_hex(const std::string_view encoded, std::string& output) -> bool {
  if (encoded.empty() || encoded.size() % 2U != 0) {
    return false;
  }
  output.clear();
  output.reserve(encoded.size() / 2U);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2U) {
    unsigned int value = 0;
    const auto pair = encoded.substr(offset, 2);
    const auto* const end = std::to_address(pair.end());
    // from_chars consumes the explicit half-open range and never expects termination.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto parsed = std::from_chars(pair.data(), end, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value > 0xFFU) {
      return false;
    }
    output.push_back(static_cast<char>(value));
  }
  return true;
}

[[nodiscard]] auto run_disconnect(const int socket_descriptor, const int ready_descriptor,
                                  const std::string_view encoded_frame,
                                  const std::chrono::milliseconds timeout) -> int {
  std::string frame;
  if (!decode_hex(encoded_frame, frame)) {
    return 1;
  }
  const auto started_ns = monotonic_ns();
  if (!write_all(socket_descriptor, frame, std::chrono::steady_clock::now() + timeout) ||
      ::write(ready_descriptor, "R", 1) != 1) {
    return 1;
  }
  static_cast<void>(::close(ready_descriptor));
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd event{.fd = socket_descriptor, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&event, 1, 10);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return 1;
    }
    if ((event.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      std::cout << R"({"schema":1,"clock":"steady_clock","observer":"native_poll",)"
                << R"("disconnect_latency_ns":)" << monotonic_ns() - started_ns << "}\n";
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 1;
}

[[nodiscard]] auto run_attach(const std::size_t repetitions, const std::string_view marker,
                              const std::span<char*> command) -> int {
  if (command.empty() || command.front() == nullptr) {
    return 1;
  }
  std::vector<std::uint64_t> latencies;
  std::vector<std::uint64_t> outer_bytes;
  latencies.reserve(repetitions);
  outer_bytes.reserve(repetitions);
  for (std::size_t index = 0; index < repetitions; ++index) {
    int descriptor = -1;
    winsize size{.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
    const auto started_ns = monotonic_ns();
    const auto child = ::forkpty(&descriptor, nullptr, nullptr, &size);
    if (child < 0) {
      return 1;
    }
    if (child == 0) {
      ::execvp(command.front(), command.data());
      ::_exit(127);
    }
    // fcntl is variadic because the final argument depends on the request.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const auto flags = ::fcntl(descriptor, F_GETFL);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
      stop_child(descriptor, child);
      return 1;
    }
    const auto [latency, bytes] = read_outer_marker(descriptor, marker, started_ns);
    stop_child(descriptor, child);
    if (latency == 0) {
      return 1;
    }
    latencies.push_back(latency);
    outer_bytes.push_back(bytes);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::cout << R"({"schema":1,"clock":"steady_clock","observer":"native_poll",)";
  print_summary("latency", latencies);
  std::cout << R"(,"outer_bytes":)";
  print_samples(outer_bytes);
  std::cout << "}\n";
  return 0;
}

[[nodiscard]] auto run_command(const int outer_descriptor, const std::size_t repetitions,
                               const std::string_view command, const std::string_view marker)
    -> int {
  std::vector<std::uint64_t> latencies;
  std::vector<std::uint64_t> outer_bytes;
  latencies.reserve(repetitions);
  outer_bytes.reserve(repetitions);
  for (std::size_t index = 0; index < repetitions; ++index) {
    // A redraw of an earlier completion must not acknowledge this command. The delimiter
    // also prevents iteration 1 from matching iteration 10.
    const auto completion = std::string(marker) + std::to_string(index) + "|";
    const auto started_ns = monotonic_ns();
    if (!write_all(outer_descriptor, command,
                   std::chrono::steady_clock::now() + interaction_timeout)) {
      std::cerr << "command probe write failed at iteration " << index << " errno=" << errno
                << '\n';
      return 1;
    }
    const auto [latency, bytes] = read_outer_marker(outer_descriptor, completion, started_ns);
    if (latency == 0) {
      std::cerr << "command probe marker failed at iteration " << index << " errno=" << errno
                << '\n';
      return 1;
    }
    latencies.push_back(latency);
    outer_bytes.push_back(bytes);
    drain_outer(outer_descriptor);
  }
  std::cout << R"({"schema":1,"clock":"steady_clock","observer":"native_poll",)";
  print_summary("latency", latencies);
  std::cout << R"(,"outer_bytes":)";
  print_samples(outer_bytes);
  std::cout << "}\n";
  return 0;
}

// One harness-prepared sample: the caller makes the completion token unique and absent from the
// visible screen, so neither a stale redraw nor a shared marker prefix can finish this sample.
[[nodiscard]] auto run_trigger(const int outer_descriptor, const std::string_view encoded_trigger,
                               const std::string_view token) -> int {
  std::string trigger;
  if (token.empty() || !decode_hex(encoded_trigger, trigger)) {
    return 2;
  }
  drain_outer(outer_descriptor);
  const auto started_ns = monotonic_ns();
  if (!write_all(outer_descriptor, trigger,
                 std::chrono::steady_clock::now() + interaction_timeout)) {
    std::cerr << "trigger probe write failed errno=" << errno << '\n';
    return 1;
  }
  const auto [latency, bytes] = read_outer_marker(outer_descriptor, token, started_ns);
  if (latency == 0) {
    return 1;
  }
  std::cout << R"({"schema":1,"clock":"steady_clock","observer":"native_poll","latency_ns":)"
            << latency << R"(,"outer_bytes":)" << bytes << "}\n";
  return 0;
}

// Resize the outer terminal as a host terminal would; the kernel delivers SIGWINCH to the
// attached client's foreground process group. The armed fixture reports its new pane geometry.
[[nodiscard]] auto run_resize(const int outer_descriptor, const int receipt_descriptor,
                              const int rows, const int columns, const std::string_view marker,
                              const std::string_view token) -> int {
  if (marker.empty() || token.empty()) {
    return 2;
  }
  drain_outer(outer_descriptor);
  const winsize size{.ws_row = static_cast<unsigned short>(rows),
                     .ws_col = static_cast<unsigned short>(columns),
                     .ws_xpixel = 0,
                     .ws_ypixel = 0};
  const auto started_ns = monotonic_ns();
  // ioctl is variadic because its third argument depends on the request.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  if (::ioctl(outer_descriptor, TIOCSWINSZ, &size) != 0) {
    std::cerr << "resize probe ioctl failed errno=" << errno << '\n';
    return 1;
  }
  std::string geometry;
  const auto result =
      read_interaction(outer_descriptor, receipt_descriptor, marker, token, started_ns, &geometry);
  if (result.key_to_pty_ns == 0 || result.key_to_outer_bytes_ns == 0) {
    std::cerr << "resize probe did not observe both endpoints\n";
    return 1;
  }
  if (std::ranges::count(geometry, 'x') != 1 ||
      !std::ranges::all_of(geometry, [](const char value) {
        return value == 'x' || (value >= '0' && value <= '9');
      })) {
    std::cerr << "resize probe received malformed pane geometry\n";
    return 1;
  }
  std::cout << R"({"schema":1,"clock":"steady_clock","observer":"native_poll",)"
            << R"("resize_to_pty_ns":)" << result.key_to_pty_ns << R"(,"resize_to_outer_bytes_ns":)"
            << result.key_to_outer_bytes_ns << R"(,"outer_bytes":)" << result.outer_bytes
            << R"(,"pane_geometry":")" << geometry << "\"}\n";
  return 0;
}

[[nodiscard]] auto run_wheel(const int outer_descriptor, const int receipt_descriptor,
                             const std::string_view label, const std::string_view label_code,
                             const std::size_t repetitions, const std::size_t burst_size) -> int {
  std::vector<std::uint64_t> key_to_pty;
  std::vector<std::uint64_t> key_to_outer_bytes;
  std::vector<std::uint64_t> outer_bytes;
  key_to_pty.reserve(repetitions);
  key_to_outer_bytes.reserve(repetitions);
  outer_bytes.reserve(repetitions);
  std::string wheel_input;
  wheel_input.reserve(wheel_report.size() * burst_size);
  for (std::size_t event = 0; event < burst_size; ++event) {
    wheel_input.append(wheel_report);
  }

  for (std::size_t index = 0; index < repetitions; ++index) {
    const auto marker = interaction_marker(label, label_code, index);
    const auto token = encoded_index_token(label_code, index);
    if (marker.empty() || token.empty() ||
        !write_all(outer_descriptor, marker + "\n",
                   std::chrono::steady_clock::now() + interaction_timeout) ||
        !wait_for_receipt(receipt_descriptor, tui_wheel_armed,
                          std::chrono::steady_clock::now() + interaction_timeout)) {
      return 1;
    }
    const auto started_ns = monotonic_ns();
    if (!write_all(outer_descriptor, wheel_input,
                   std::chrono::steady_clock::now() + interaction_timeout)) {
      return 1;
    }
    const auto result =
        read_interaction(outer_descriptor, receipt_descriptor, marker, token, started_ns);
    if (result.key_to_pty_ns == 0 || result.key_to_outer_bytes_ns == 0) {
      return 1;
    }
    key_to_pty.push_back(result.key_to_pty_ns);
    key_to_outer_bytes.push_back(result.key_to_outer_bytes_ns);
    outer_bytes.push_back(result.outer_bytes);
    drain_outer(outer_descriptor);
  }

  std::cout << R"({"schema":1,"clock":"steady_clock","observer":"native_poll",)";
  print_summary("key_to_pty", key_to_pty);
  std::cout << ',';
  print_summary("key_to_outer_bytes", key_to_outer_bytes);
  std::cout << R"(,"outer_bytes":)";
  print_samples(outer_bytes);
  std::cout << "}\n";
  return 0;
}

// One native event loop owns scheduling and both correlated completion endpoints.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_open_loop(const int outer_descriptor, const int receipt_descriptor,
                                 const std::string_view label, const std::string_view label_code,
                                 const std::size_t repetitions, const std::uint64_t interval_ns)
    -> int {
  std::vector<std::string> markers;
  std::vector<std::string> tokens;
  std::vector<std::uint64_t> started(repetitions, 0);
  std::vector<std::uint64_t> key_to_pty(repetitions, 0);
  std::vector<std::uint64_t> key_to_outer_bytes(repetitions, 0);
  std::vector<std::uint64_t> schedule_lateness(repetitions, 0);
  markers.reserve(repetitions);
  tokens.reserve(repetitions);
  std::unordered_map<std::string, std::size_t> marker_indices;
  for (std::size_t index = 0; index < repetitions; ++index) {
    markers.push_back(interaction_marker(label, label_code, index));
    tokens.push_back(encoded_index_token(label_code, index));
    if (markers.back().empty() || tokens.back().empty()) {
      return 1;
    }
    marker_indices.emplace(markers.back(), index);
  }

  std::string retained;
  retained.reserve(read_bytes_max * 2U);
  std::array<char, read_bytes_max> output{};
  OuterTextDecoder decoder;
  std::size_t sent = 0;
  std::size_t completed_pty = 0;
  std::size_t completed_outer = 0;
  std::uint64_t random_state = 0x9E37'79B9'7F4A'7C15ULL;
  const auto jitter_span = std::max<std::uint64_t>(1U, interval_ns / 4U);
  auto next_send_ns = monotonic_ns() + open_loop_initial_delay_ns;
  auto final_deadline = std::chrono::steady_clock::now() +
                        open_loop_send_window(repetitions, interval_ns) + interaction_timeout;

  while ((completed_pty < repetitions || completed_outer < repetitions) &&
         std::chrono::steady_clock::now() < final_deadline) {
    auto now_ns = monotonic_ns();
    while (sent < repetitions && now_ns >= next_send_ns) {
      schedule_lateness.at(sent) = now_ns - next_send_ns;
      started.at(sent) = now_ns;
      if (!write_all(outer_descriptor, markers.at(sent) + "\n",
                     std::chrono::steady_clock::now() + interaction_timeout)) {
        return 1;
      }
      ++sent;
      random_state ^= random_state << 13U;
      random_state ^= random_state >> 7U;
      random_state ^= random_state << 17U;
      const auto jitter = random_state % jitter_span;
      next_send_ns += interval_ns - (jitter_span / 2U) + jitter;
      now_ns = monotonic_ns();
      if (sent == repetitions) {
        final_deadline = std::chrono::steady_clock::now() + interaction_timeout;
      }
    }

    const auto until_send_ns =
        sent < repetitions && next_send_ns > now_ns ? next_send_ns - now_ns : std::uint64_t{0};
    const auto timeout_ms =
        sent < repetitions
            ? static_cast<int>(std::min<std::uint64_t>(20U, until_send_ns / 1'000'000U))
            : 20;
    std::array events{
        pollfd{.fd = receipt_descriptor, .events = POLLIN, .revents = 0},
        pollfd{.fd = outer_descriptor, .events = POLLIN, .revents = 0},
    };
    const auto polled = ::poll(events.data(), events.size(), timeout_ms);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return 1;
    }
    if (polled == 0) {
      continue;
    }
    const auto ready_ns = monotonic_ns();
    if ((events.front().revents & POLLIN) != 0) {
      std::string received;
      if (!receive_datagram(receipt_descriptor, received)) {
        return 1;
      }
      const auto found = marker_indices.find(received);
      if (found == marker_indices.end() || started.at(found->second) == 0 ||
          key_to_pty.at(found->second) != 0) {
        return 1;
      }
      key_to_pty.at(found->second) = ready_ns - started.at(found->second);
      ++completed_pty;
    }
    if ((events.back().revents & (POLLIN | POLLHUP)) != 0) {
      while (true) {
        const auto count = ::read(outer_descriptor, output.data(), output.size());
        if (count > 0) {
          decoder.append(std::span(output).first(static_cast<std::size_t>(count)), retained);
          for (std::size_t index = 0; index < sent; ++index) {
            if (key_to_outer_bytes.at(index) == 0 && retained.contains(tokens.at(index))) {
              key_to_outer_bytes.at(index) = ready_ns - started.at(index);
              ++completed_outer;
            }
          }
          if (retained.size() > read_bytes_max * 2U) {
            retained.erase(0, retained.size() - read_bytes_max);
          }
          continue;
        }
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          break;
        }
        return 1;
      }
    }
  }
  if (completed_pty != repetitions || completed_outer != repetitions) {
    const auto missing = [](const std::vector<std::uint64_t>& samples) {
      return std::ranges::find(samples, std::uint64_t{0}) - samples.begin();
    };
    constexpr std::size_t diagnostic_bytes = 512U;
    const auto tail = retained.substr(
        retained.size() > diagnostic_bytes ? retained.size() - diagnostic_bytes : 0U);
    std::cerr << "open-loop timeout: sent=" << sent << " pty=" << completed_pty
              << " outer=" << completed_outer << " first_missing_pty=" << missing(key_to_pty)
              << " first_missing_outer=" << missing(key_to_outer_bytes)
              << " decoded_tail=" << std::quoted(tail) << '\n';
    return 1;
  }
  std::cout << R"({"schema":1,"clock":"steady_clock","observer":"native_poll",)";
  print_summary("key_to_pty", key_to_pty);
  std::cout << ',';
  print_summary("key_to_outer_bytes", key_to_outer_bytes);
  std::cout << ',';
  print_summary("schedule_lateness", schedule_lateness);
  std::cout << R"(,"offered_interval_ns":)" << interval_ns << "}\n";
  return 0;
}

[[nodiscard]] auto run_latency(const int outer_descriptor, const int receipt_descriptor,
                               const std::string_view peer_path, const std::string_view label,
                               const std::string_view label_code, const std::size_t repetitions,
                               const bool wait_for_peer_ready) -> int {
  std::vector<std::uint64_t> key_to_pty;
  std::vector<std::uint64_t> key_to_outer_bytes;
  std::vector<std::uint64_t> outer_bytes;
  key_to_pty.reserve(repetitions);
  key_to_outer_bytes.reserve(repetitions);
  outer_bytes.reserve(repetitions);

  for (std::size_t index = 0; index < repetitions; ++index) {
    const auto marker = interaction_marker(label, label_code, index);
    const auto token = encoded_index_token(label_code, index);
    if (marker.empty() || token.empty()) {
      return 1;
    }
    const auto started_ns = monotonic_ns();
    if (!write_all(outer_descriptor, marker + "\n",
                   std::chrono::steady_clock::now() + interaction_timeout)) {
      return 1;
    }
    const auto result =
        read_interaction(outer_descriptor, receipt_descriptor, marker, token, started_ns);
    if (result.key_to_pty_ns == 0 || result.key_to_outer_bytes_ns == 0) {
      return 1;
    }
    key_to_pty.push_back(result.key_to_pty_ns);
    key_to_outer_bytes.push_back(result.key_to_outer_bytes_ns);
    outer_bytes.push_back(result.outer_bytes);

    if (wait_for_peer_ready &&
        (!acknowledge_visible(receipt_descriptor, peer_path) ||
         !wait_for_receipt(receipt_descriptor, latency_next_ready,
                           std::chrono::steady_clock::now() + interaction_timeout))) {
      return 1;
    }
    drain_outer(outer_descriptor);
  }

  std::cout << R"({"schema":1,"clock":"steady_clock","observer":"native_poll",)";
  print_summary("key_to_pty", key_to_pty);
  std::cout << ',';
  print_summary("key_to_outer_bytes", key_to_outer_bytes);
  std::cout << R"(,"outer_bytes":)";
  print_samples(outer_bytes);
  std::cout << "}\n";
  return 0;
}

[[nodiscard]] auto argument(const std::span<char*> arguments, const std::size_t index)
    -> std::string_view {
  return arguments.subspan(index, 1).front();
}

// Command decoding is deliberately explicit so malformed probe invocations fail closed.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto run_main(const std::span<char*> arguments) -> int {
  if (arguments.size() == 2U && std::string_view(argument(arguments, 1)) == "--self-test") {
    const auto first = interaction_marker("OUTPUT", "LAA", 0);
    const auto last = interaction_marker("OUTPUT", "LAA", 9'999);
    const auto generated_profile = interaction_marker("P2_IDLE", "LAK", 0);
    constexpr std::string_view encoded_outer =
        "__LEMMA_\x1B[23;1HOUTPUT\x1B]ignored title\x1B\\_DONE__";
    std::string decoded_outer;
    OuterTextDecoder decoder;
    decoder.append({encoded_outer.data(), encoded_outer.size()}, decoded_outer);
    const auto maximum_repetition_window = open_loop_send_window(10'000U, 8'333'000U);
    const auto first_token =
        lemma::diagnostic::latency_trace_marker_token(std::as_bytes(std::span(first)));
    const auto last_token =
        lemma::diagnostic::latency_trace_marker_token(std::as_bytes(std::span(last)));
    return first_token != 0 && last_token != 0 && first_token != last_token &&
                   first == "__LEMMA_OUTPUT_0000_AAAAAA__" &&
                   last == "__LEMMA_OUTPUT_9999_YYYYJP__" &&
                   generated_profile == "__LEMMA_P2_IDLE_0000_ZZZUCE__" &&
                   interaction_marker("OUTPUT", "OUT", 0).empty() &&
                   decoded_outer == "__LEMMA_OUTPUT_DONE__" &&
                   maximum_repetition_window > std::chrono::seconds(80)
               ? 0
               : 1;
  }
  if (arguments.size() == 6U && argument(arguments, 1) == "disconnect") {
    const auto socket_descriptor =
        parse_integer(argument(arguments, 2), 0, std::numeric_limits<int>::max());
    const auto ready_descriptor =
        parse_integer(argument(arguments, 3), 0, std::numeric_limits<int>::max());
    const auto timeout_ms = parse_integer(argument(arguments, 5), 1, 60'000);
    if (socket_descriptor < 0 || ready_descriptor < 0 || timeout_ms < 0) {
      return 2;
    }
    return run_disconnect(socket_descriptor, ready_descriptor, argument(arguments, 4),
                          std::chrono::milliseconds(timeout_ms));
  }
  if (arguments.size() >= 6U && argument(arguments, 1) == "attach") {
    const auto repetitions =
        parse_integer(argument(arguments, 2), 1, static_cast<int>(repetitions_max));
    if (repetitions < 0 || std::string_view(argument(arguments, 4)) != "--") {
      return 2;
    }
    return run_attach(static_cast<std::size_t>(repetitions), argument(arguments, 3),
                      arguments.subspan(5));
  }
  if (arguments.size() == 6U && std::string_view(argument(arguments, 1)) == "command") {
    const auto outer_descriptor =
        parse_integer(argument(arguments, 2), 0, std::numeric_limits<int>::max());
    const auto repetitions =
        parse_integer(argument(arguments, 3), 1, static_cast<int>(repetitions_max));
    if (outer_descriptor < 0 || repetitions < 0) {
      return 2;
    }
    return run_command(outer_descriptor, static_cast<std::size_t>(repetitions),
                       argument(arguments, 4), argument(arguments, 5));
  }
  if (arguments.size() == 5U && argument(arguments, 1) == "trigger") {
    const auto outer_descriptor =
        parse_integer(argument(arguments, 2), 0, std::numeric_limits<int>::max());
    if (outer_descriptor < 0) {
      return 2;
    }
    return run_trigger(outer_descriptor, argument(arguments, 3), argument(arguments, 4));
  }
  if (arguments.size() == 8U && argument(arguments, 1) == "resize") {
    const auto outer_descriptor =
        parse_integer(argument(arguments, 2), 0, std::numeric_limits<int>::max());
    const auto receipt_descriptor =
        parse_integer(argument(arguments, 3), 0, std::numeric_limits<int>::max());
    const auto rows = parse_integer(argument(arguments, 4), 2, 200);
    const auto columns = parse_integer(argument(arguments, 5), 2, 500);
    if (outer_descriptor < 0 || receipt_descriptor < 0 || rows < 0 || columns < 0) {
      return 2;
    }
    return run_resize(outer_descriptor, receipt_descriptor, rows, columns, argument(arguments, 6),
                      argument(arguments, 7));
  }
  if (arguments.size() == 9U && std::string_view(argument(arguments, 1)) == "open-loop") {
    const auto outer_descriptor =
        parse_integer(argument(arguments, 2), 0, std::numeric_limits<int>::max());
    const auto receipt_descriptor =
        parse_integer(argument(arguments, 3), 0, std::numeric_limits<int>::max());
    const auto repetitions =
        parse_integer(argument(arguments, 6), 1, static_cast<int>(repetitions_max));
    const auto interval_us = parse_integer(argument(arguments, 7), 100, 1'000'000);
    if (outer_descriptor < 0 || receipt_descriptor < 0 || repetitions < 0 || interval_us < 0 ||
        std::string_view(argument(arguments, 8)) != "bounded") {
      return 2;
    }
    return run_open_loop(outer_descriptor, receipt_descriptor, argument(arguments, 4),
                         argument(arguments, 5), static_cast<std::size_t>(repetitions),
                         static_cast<std::uint64_t>(interval_us) * 1'000U);
  }
  if (arguments.size() == 9U && std::string_view(argument(arguments, 1)) == "wheel") {
    const auto outer_descriptor =
        parse_integer(argument(arguments, 2), 0, std::numeric_limits<int>::max());
    const auto receipt_descriptor =
        parse_integer(argument(arguments, 3), 0, std::numeric_limits<int>::max());
    const auto repetitions =
        parse_integer(argument(arguments, 6), 1, static_cast<int>(repetitions_max));
    const auto burst_size = parse_integer(argument(arguments, 7), 1, 1'000);
    if (outer_descriptor < 0 || receipt_descriptor < 0 || repetitions < 0 || burst_size < 0 ||
        std::string_view(argument(arguments, 8)) != "bounded") {
      return 2;
    }
    return run_wheel(outer_descriptor, receipt_descriptor, argument(arguments, 4),
                     argument(arguments, 5), static_cast<std::size_t>(repetitions),
                     static_cast<std::size_t>(burst_size));
  }
  if (arguments.size() != 9U || std::string_view(argument(arguments, 1)) != "latency") {
    return 2;
  }
  const auto outer_descriptor =
      parse_integer(argument(arguments, 2), 0, std::numeric_limits<int>::max());
  const auto receipt_descriptor =
      parse_integer(argument(arguments, 3), 0, std::numeric_limits<int>::max());
  const std::string_view peer_path(argument(arguments, 4));
  const std::string_view label(argument(arguments, 5));
  const std::string_view label_code(argument(arguments, 6));
  const auto repetitions =
      parse_integer(argument(arguments, 7), 1, static_cast<int>(repetitions_max));
  const auto wait_for_peer_ready = parse_integer(argument(arguments, 8), 0, 1);
  if (outer_descriptor < 0 || receipt_descriptor < 0 || repetitions < 0 ||
      wait_for_peer_ready < 0) {
    return 2;
  }
  return run_latency(outer_descriptor, receipt_descriptor, peer_path, label, label_code,
                     static_cast<std::size_t>(repetitions), wait_for_peer_ready != 0);
}

} // namespace

int main(const int argc, char** argv) {
  try {
    return run_main({argv, static_cast<std::size_t>(argc)});
  } catch (...) {
    return 1;
  }
}
