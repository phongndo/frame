#include "diagnostic/latency_trace.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

namespace frame::diagnostic {
namespace {

[[nodiscard]] auto marker_token(const std::span<const std::byte> marker) noexcept -> std::uint64_t {
  std::uint64_t token = 0;
  for (const auto byte : marker) {
    token =
        (token * 26U) + static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(byte) -
                                                   std::to_integer<std::uint8_t>(std::byte{'A'}));
  }
  // There are only 26^6 valid payloads, so this base-26 encoding is collision-free and bounded.
  return token + 1U;
}

} // namespace

// This is a bounded byte-state machine for one fixed fixture-token grammar.
// NOLINTBEGIN(readability-function-cognitive-complexity)
[[nodiscard]] auto
LatencyTraceMarkerMatcher::observe(const std::span<const std::byte> bytes) noexcept
    -> std::uint64_t {
  std::uint64_t observed = 0;
  for (const auto byte : bytes) {
    if (marker_size_ == 0) {
      if (byte == std::byte{'_'}) {
        marker_.front() = byte;
        marker_size_ = 1;
      }
    } else {
      const bool payload_position = marker_size_ >= 1U && marker_size_ <= payload_bytes;
      const bool terminator_position =
          marker_size_ == payload_bytes + 1U || marker_size_ == payload_bytes + 2U;
      const bool valid = (payload_position && byte >= std::byte{'A'} && byte <= std::byte{'Z'}) ||
                         (terminator_position && byte == std::byte{'_'});
      if (!valid) {
        reset();
        if (byte == std::byte{'_'}) {
          marker_.front() = byte;
          marker_size_ = 1;
        }
      } else {
        std::span(marker_).subspan(marker_size_, 1).front() = byte;
        ++marker_size_;
        if (marker_size_ == marker_.size()) {
          if (observed == 0) {
            observed = marker_token(std::span(marker_).subspan<1, payload_bytes>());
          }
          reset();
        }
      }
    }
  }
  return observed;
}
// NOLINTEND(readability-function-cognitive-complexity)

[[nodiscard]] auto
LatencyTraceMarkerMatcher::observe_expected_visible(const std::span<const std::byte> bytes,
                                                    const std::uint64_t expected) noexcept
    -> std::uint64_t {
  if (expected == 0) {
    visible_size_ = 0;
    return 0;
  }
  for (const auto byte : bytes) {
    if (byte < std::byte{'A'} || byte > std::byte{'Z'}) {
      visible_size_ = 0;
    } else {
      if (visible_size_ < visible_.size()) {
        std::span(visible_).subspan(visible_size_, 1).front() = byte;
        ++visible_size_;
      } else {
        std::ranges::rotate(visible_, std::next(visible_.begin()));
        visible_.back() = byte;
      }
      if (visible_size_ == visible_.size() && marker_token(visible_) == expected) {
        visible_size_ = 0;
        return expected;
      }
    }
  }
  return 0;
}

void LatencyTraceMarkerMatcher::reset() noexcept {
  marker_size_ = 0;
  visible_size_ = 0;
}

[[nodiscard]] auto latency_trace_marker_token(const std::span<const std::byte> bytes) noexcept
    -> std::uint64_t {
  LatencyTraceMarkerMatcher matcher;
  return matcher.observe(bytes);
}

} // namespace frame::diagnostic

#ifdef FRAME_ENABLE_LATENCY_TRACE

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace frame::diagnostic {
namespace {

constexpr std::uint64_t trace_magic = 0x3145'4341'5254'4D46ULL;
constexpr std::uint32_t trace_version = 2;
constexpr std::size_t trace_events_max = 524'288;
constexpr std::size_t trace_path_bytes_max = 1'024;

struct TraceHeader final {
  std::uint64_t magic{trace_magic};
  std::uint32_t version{trace_version};
  std::uint16_t role{0};
  std::uint16_t event_size{0};
  std::uint32_t capacity{trace_events_max};
  std::uint32_t process{0};
  std::uint64_t count{0};
  std::uint64_t dropped{0};
  std::array<std::uint64_t, 3> reserved{};
};

struct TraceEvent final {
  std::uint64_t timestamp_ns{0};
  std::uint64_t sequence{0};
  std::uint64_t correlation{0};
  std::uint64_t value{0};
  std::uint32_t subject{0};
  std::uint16_t stage{0};
  std::uint16_t reserved{0};
};

static_assert(sizeof(TraceHeader) == 64);
static_assert(sizeof(TraceEvent) == 40);
static_assert(trace_events_max <= std::numeric_limits<std::uint32_t>::max());
constexpr std::size_t trace_file_bytes =
    sizeof(TraceHeader) + (trace_events_max * sizeof(TraceEvent));

[[nodiscard]] constexpr auto role_name(const LatencyTraceRole role) noexcept -> const char* {
  switch (role) {
  case LatencyTraceRole::daemon:
    return "daemon";
  case LatencyTraceRole::attached_client:
    return "client";
  }
  return "unknown";
}

class TraceState final {
public:
  TraceState() = default;
  TraceState(const TraceState&) = delete;
  auto operator=(const TraceState&) -> TraceState& = delete;
  TraceState(TraceState&&) = delete;
  auto operator=(TraceState&&) -> TraceState& = delete;

  ~TraceState() {
    if (mapping_ != MAP_FAILED) {
      static_cast<void>(::msync(mapping_, trace_file_bytes, MS_ASYNC));
      static_cast<void>(::munmap(mapping_, trace_file_bytes));
    }
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  void initialize(const LatencyTraceRole role) noexcept {
    if (initialized_) {
      return;
    }
    initialized_ = true;
    const char* const directory = std::getenv("FRAME_LATENCY_TRACE");
    if (directory == nullptr || *directory != '/') {
      return;
    }
    const auto directory_size = std::strlen(directory);
    if (directory_size == 0 || directory_size > trace_path_bytes_max - 64U) {
      return;
    }
    struct stat directory_status{};
    if (::stat(directory, &directory_status) != 0) {
      return;
    }
    const bool is_directory = (directory_status.st_mode & S_IFMT) == S_IFDIR;
    if (!is_directory || directory_status.st_uid != ::getuid()) {
      return;
    }

    std::array<char, trace_path_bytes_max> path{};
    // snprintf is variadic because the bounded path contains runtime fields.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const auto written = std::snprintf(path.data(), path.size(), "%s/%s-%ld.ltrace", directory,
                                       role_name(role), static_cast<long>(::getpid()));
    if (written <= 0 || static_cast<std::size_t>(written) >= path.size()) {
      return;
    }
    // open is variadic because O_CREAT supplies the final mode argument.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    descriptor_ = ::open(path.data(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor_ < 0 || ::ftruncate(descriptor_, static_cast<off_t>(trace_file_bytes)) != 0) {
      close_failed_descriptor();
      return;
    }
    mapping_ =
        ::mmap(nullptr, trace_file_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor_, 0);
    if (mapping_ == MAP_FAILED) {
      close_failed_descriptor();
      return;
    }

    header_ = static_cast<TraceHeader*>(mapping_);
    *header_ = {
        .role = static_cast<std::uint16_t>(role),
        .event_size = static_cast<std::uint16_t>(sizeof(TraceEvent)),
        .process = static_cast<std::uint32_t>(::getpid()),
    };
    // mmap storage is aligned for every object type; the fixed header preserves event alignment.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    events_ = reinterpret_cast<TraceEvent*>(
        std::span<std::byte>(static_cast<std::byte*>(mapping_), trace_file_bytes)
            .subspan(sizeof(TraceHeader))
            .data());
  }

  void set_correlation(const std::uint64_t correlation) noexcept {
    current_correlation_ = correlation;
  }

  [[nodiscard]] auto correlation() const noexcept -> std::uint64_t { return current_correlation_; }

  void record(const LatencyTraceStage stage, const std::uint32_t subject, const std::uint64_t value,
              const std::uint64_t correlation) noexcept {
    static_cast<void>(
        append(stage, subject, value, correlation == 0 ? current_correlation_ : correlation));
  }

  [[nodiscard]] auto record_pending(const LatencyTraceStage stage, const std::uint32_t subject,
                                    const std::uint64_t value) noexcept -> std::uint64_t {
    return append(stage, subject, value, 0);
  }

  [[nodiscard]] auto correlate(const std::uint64_t sequence,
                               const std::uint64_t correlation) noexcept -> bool {
    if (header_ == nullptr || sequence == 0 || correlation == 0 || sequence > header_->count) {
      return false;
    }
    const auto index = static_cast<std::size_t>(sequence - 1U);
    auto& event = std::span(events_, trace_events_max).subspan(index, 1).front();
    if (event.sequence != sequence || event.correlation != 0) {
      return false;
    }
    event.correlation = correlation;
    return true;
  }

private:
  [[nodiscard]] auto append(const LatencyTraceStage stage, const std::uint32_t subject,
                            const std::uint64_t value, const std::uint64_t correlation) noexcept
      -> std::uint64_t {
    if (header_ == nullptr) {
      return {};
    }
    if (header_->count >= trace_events_max) {
      if (header_->dropped < std::numeric_limits<std::uint64_t>::max()) {
        ++header_->dropped;
      }
      return {};
    }
    timespec timestamp{};
    if (::clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0 || timestamp.tv_sec < 0 ||
        timestamp.tv_nsec < 0) {
      return {};
    }
    const auto seconds = static_cast<std::uint64_t>(timestamp.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(timestamp.tv_nsec);
    if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) / 1'000'000'000U) {
      return {};
    }
    const auto sequence = header_->count + 1U;
    const auto index = static_cast<std::size_t>(header_->count);
    std::span(events_, trace_events_max).subspan(index, 1).front() = {
        .timestamp_ns = (seconds * 1'000'000'000U) + nanoseconds,
        .sequence = sequence,
        .correlation = correlation,
        .value = value,
        .subject = subject,
        .stage = static_cast<std::uint16_t>(stage),
    };
    // Publish the count only after the complete fixed-size event is stored. Correlation is the one
    // field its pending handle may set once; append-only storage never reuses the event slot.
    ++header_->count;
    return sequence;
  }

  void close_failed_descriptor() noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
      descriptor_ = -1;
    }
  }

  bool initialized_{false};
  int descriptor_{-1};
  void* mapping_{MAP_FAILED};
  TraceHeader* header_{nullptr};
  TraceEvent* events_{nullptr};
  std::uint64_t current_correlation_{0};
};

[[nodiscard]] auto trace_state() noexcept -> TraceState& {
  static TraceState state;
  return state;
}

} // namespace

void set_latency_trace_role(const LatencyTraceRole role) noexcept {
  trace_state().initialize(role);
}

void set_latency_trace_correlation(const std::uint64_t correlation) noexcept {
  trace_state().set_correlation(correlation);
}

[[nodiscard]] auto latency_trace_correlation() noexcept -> std::uint64_t {
  return trace_state().correlation();
}

void record_latency_trace(const LatencyTraceStage stage, const std::uint32_t subject,
                          const std::uint64_t value, const std::uint64_t correlation) noexcept {
  trace_state().record(stage, subject, value, correlation);
}

[[nodiscard]] auto record_client_socket_read_latency_trace(const std::uint32_t subject,
                                                           const std::uint64_t value) noexcept
    -> LatencyTraceEventHandle {
  return LatencyTraceEventHandle(
      trace_state().record_pending(LatencyTraceStage::client_socket_read, subject, value));
}

[[nodiscard]] auto
correlate_client_socket_read_latency_trace(const LatencyTraceEventHandle event,
                                           const std::uint64_t correlation) noexcept -> bool {
  return trace_state().correlate(event.sequence_, correlation);
}

} // namespace frame::diagnostic

#else

namespace frame::diagnostic::detail {

void latency_trace_disabled_translation_unit() noexcept {}

} // namespace frame::diagnostic::detail

#endif // FRAME_ENABLE_LATENCY_TRACE
