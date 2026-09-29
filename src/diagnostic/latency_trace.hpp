#ifndef FRAME_DIAGNOSTIC_LATENCY_TRACE_HPP
#define FRAME_DIAGNOSTIC_LATENCY_TRACE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace frame::diagnostic {

enum class LatencyTraceRole : std::uint8_t {
  daemon = 1,
  attached_client = 2,
};

enum class LatencyTraceStage : std::uint8_t {
  client_physical_input_read = 1,
  daemon_input_message_received = 2,
  daemon_pty_write_progress = 3,
  daemon_pty_output_read = 4,
  ghostty_damage_reported = 5,
  frame_composition_started = 6,
  frame_composition_finished = 7,
  daemon_socket_write_progress = 8,
  client_socket_read = 9,
  client_outer_terminal_write_started = 10,
  client_outer_terminal_write_finished = 11,
};

// Controlled latency fixtures echo one bounded _XXXXXX__ token (six uppercase payload bytes).
// The same token is observed independently at the client, daemon input, actual PTY writes, and
// resulting PTY output; it is not inferred from a timestamp window.
class LatencyTraceMarkerMatcher final {
public:
  [[nodiscard]] auto observe(std::span<const std::byte> bytes) noexcept -> std::uint64_t;
  [[nodiscard]] auto observe_expected_visible(std::span<const std::byte> bytes,
                                              std::uint64_t expected) noexcept -> std::uint64_t;
  void reset() noexcept;

private:
  static constexpr std::size_t payload_bytes = 6;
  static constexpr std::size_t marker_bytes = payload_bytes + 3;

  std::array<std::byte, marker_bytes> marker_{};
  std::size_t marker_size_{0};
  std::array<std::byte, payload_bytes> visible_{};
  std::size_t visible_size_{0};
};

[[nodiscard]] auto latency_trace_marker_token(std::span<const std::byte> bytes) noexcept
    -> std::uint64_t;

class LatencyTraceEventHandle final {
public:
  constexpr LatencyTraceEventHandle() noexcept = default;
  [[nodiscard]] constexpr auto valid() const noexcept -> bool { return sequence_ != 0; }

private:
  explicit constexpr LatencyTraceEventHandle(const std::uint64_t sequence) noexcept
      : sequence_(sequence) {}

  friend auto record_client_socket_read_latency_trace(std::uint32_t subject,
                                                      std::uint64_t value) noexcept
      -> LatencyTraceEventHandle;
  friend auto correlate_client_socket_read_latency_trace(LatencyTraceEventHandle event,
                                                         std::uint64_t correlation) noexcept
      -> bool;

  std::uint64_t sequence_{0};
};

#ifdef FRAME_ENABLE_LATENCY_TRACE

// Diagnostic trace files are enabled only when the build-time option and the
// FRAME_LATENCY_TRACE directory are both present. Each process owns one bounded mmap file.
void set_latency_trace_role(LatencyTraceRole role) noexcept;
void set_latency_trace_correlation(std::uint64_t correlation) noexcept;
[[nodiscard]] auto latency_trace_correlation() noexcept -> std::uint64_t;
void record_latency_trace(LatencyTraceStage stage, std::uint32_t subject = 0,
                          std::uint64_t value = 0, std::uint64_t correlation = 0) noexcept;
// TraceState owns the append-only event. The receive call retains its exact timestamp handle until
// the decoder either correlates that event once or lets it remain zero.
[[nodiscard]] auto record_client_socket_read_latency_trace(std::uint32_t subject,
                                                           std::uint64_t value) noexcept
    -> LatencyTraceEventHandle;
[[nodiscard]] auto correlate_client_socket_read_latency_trace(LatencyTraceEventHandle event,
                                                              std::uint64_t correlation) noexcept
    -> bool;

#else

namespace detail {
void latency_trace_disabled_translation_unit() noexcept;
} // namespace detail

inline void set_latency_trace_role([[maybe_unused]] const LatencyTraceRole role) noexcept {}
inline void
set_latency_trace_correlation([[maybe_unused]] const std::uint64_t correlation) noexcept {}
[[nodiscard]] inline auto latency_trace_correlation() noexcept -> std::uint64_t { return 0; }
inline void record_latency_trace([[maybe_unused]] const LatencyTraceStage stage,
                                 [[maybe_unused]] const std::uint32_t subject = 0,
                                 [[maybe_unused]] const std::uint64_t value = 0,
                                 [[maybe_unused]] const std::uint64_t correlation = 0) noexcept {}
[[nodiscard]] inline auto
record_client_socket_read_latency_trace([[maybe_unused]] const std::uint32_t subject,
                                        [[maybe_unused]] const std::uint64_t value) noexcept
    -> LatencyTraceEventHandle {
  return {};
}
[[nodiscard]] inline auto correlate_client_socket_read_latency_trace(
    [[maybe_unused]] const LatencyTraceEventHandle event,
    [[maybe_unused]] const std::uint64_t correlation) noexcept -> bool {
  return false;
}

#endif

} // namespace frame::diagnostic

#endif // FRAME_DIAGNOSTIC_LATENCY_TRACE_HPP
