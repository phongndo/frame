#ifndef FRAME_CORE_CONNECTION_OUTPUT_HPP
#define FRAME_CORE_CONNECTION_OUTPUT_HPP

#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>
#include <system_error>

namespace frame::core {

class ConnectionOutput final {
public:
  [[nodiscard]] auto append(const std::span<const std::byte> bytes) noexcept -> bool {
    if (bytes.size() > storage_.size() - size_) {
      return false;
    }
    std::ranges::copy(bytes, std::span(storage_).subspan(size_).begin());
    size_ += bytes.size();
    return true;
  }

  [[nodiscard]] auto append_text(const std::string_view text) noexcept -> bool {
    return append(std::as_bytes(std::span(text.data(), text.size())));
  }

  [[nodiscard]] auto append_safe(const std::string_view text, const std::size_t maximum) noexcept
      -> bool {
    const auto size = std::min(text.size(), maximum);
    if (size > storage_.size() - size_) {
      return false;
    }
    for (const char character : std::span(text).first(size)) {
      const auto value = static_cast<unsigned char>(character);
      std::span(storage_).subspan(size_, 1).front() =
          static_cast<std::byte>(value < 0x20U || value == 0x7FU ? '?' : character);
      ++size_;
    }
    return true;
  }

  [[nodiscard]] auto append_title(const std::string_view title) noexcept -> bool {
    constexpr std::size_t title_bytes_max = 256;
    return append_safe(title, title_bytes_max);
  }

  [[nodiscard]] auto append_number(const std::uint64_t value) noexcept -> bool {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.begin(), buffer.end(), value);
    if (result.ec != std::errc{}) {
      return false;
    }
    const auto size = static_cast<std::size_t>(std::distance(buffer.begin(), result.ptr));
    return append_text(std::string_view(buffer.data(), size));
  }

  [[nodiscard]] auto readable() const noexcept -> std::span<const std::byte> {
    return std::span(storage_).first(size_).subspan(offset_);
  }
  [[nodiscard]] auto busy() const noexcept -> bool { return offset_ < size_; }
  [[nodiscard]] auto consume(const std::size_t bytes) noexcept -> bool {
    if (bytes > size_ - offset_) {
      return false;
    }
    offset_ += bytes;
    return true;
  }
  void reset() noexcept {
    size_ = 0;
    offset_ = 0;
  }

private:
  std::array<std::byte, limits::pending_connection_output_bytes_max> storage_{};
  std::size_t size_{0};
  std::size_t offset_{0};
};

struct ConnectionWriteAttempt final {
  std::ptrdiff_t bytes{0};
  int error{0};
};

using ConnectionWriteOperation =
    ConnectionWriteAttempt (*)(void* context, std::span<const std::byte> bytes) noexcept;

enum class ConnectionFlushStatus : unsigned char {
  drained,
  pending,
  blocked,
  hard_error,
};

inline constexpr std::size_t connection_write_bytes_per_turn_max = std::size_t{16} * 1'024U;
inline constexpr std::size_t connection_write_attempts_per_turn_max = 16;

// Flushes retained control output without consuming bytes until the writer reports progress.
// global_budget is reduced only by bytes actually written.
template <typename Output>
[[nodiscard]] inline auto
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
flush_connection_output(Output& output, std::size_t& global_budget,
                        const ConnectionWriteOperation write, void* const context) noexcept
    -> ConnectionFlushStatus {
  if (!output.busy()) {
    return ConnectionFlushStatus::drained;
  }
  if (write == nullptr || global_budget == 0) {
    return ConnectionFlushStatus::pending;
  }

  std::size_t budget = std::min(connection_write_bytes_per_turn_max, global_budget);
  std::size_t attempts = 0;
  while (output.busy() && budget > 0 && attempts < connection_write_attempts_per_turn_max) {
    ++attempts;
    const auto readable = output.readable();
    const auto bytes = readable.first(std::min(readable.size(), budget));
    const auto result = write(context, bytes);
    if (result.bytes > 0) {
      const auto size = static_cast<std::size_t>(result.bytes);
      if (size > bytes.size() || !output.consume(size)) {
        return ConnectionFlushStatus::hard_error;
      }
      budget -= size;
      global_budget -= size;
      continue;
    }
    if (result.bytes == 0) {
      return ConnectionFlushStatus::hard_error;
    }
    if (result.error == EINTR) {
      continue;
    }
    if (result.error == EAGAIN || result.error == EWOULDBLOCK) {
      return ConnectionFlushStatus::blocked;
    }
    return ConnectionFlushStatus::hard_error;
  }
  return output.busy() ? ConnectionFlushStatus::pending : ConnectionFlushStatus::drained;
}

} // namespace frame::core

#endif // FRAME_CORE_CONNECTION_OUTPUT_HPP
