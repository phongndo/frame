#ifndef FRAME_CORE_PTY_WRITER_HPP
#define FRAME_CORE_PTY_WRITER_HPP

#include "core/input.hpp"

#include <cstddef>
#include <span>

namespace frame::core {

inline constexpr std::size_t pty_write_bytes_per_pane_turn_max = std::size_t{64} * 1'024U;
inline constexpr std::size_t pty_write_attempts_per_pane_turn_max = 32;

struct PtyWriteAttempt final {
  std::ptrdiff_t bytes{0};
  int error{0};
};

using PtyWriteOperation = PtyWriteAttempt (*)(void* context,
                                              std::span<const std::byte> bytes) noexcept;

enum class PtyFlushStatus : unsigned char {
  drained,
  pending,
  blocked,
  hard_error,
};

// Flushes retained bytes without consuming data until the writer reports a successful write.
// global_budget is reduced only by bytes actually written.
[[nodiscard]] auto flush_pty_write_queue(PanePtyWriteQueue& queue, std::size_t& global_budget,
                                         PtyWriteOperation write, void* context) noexcept
    -> PtyFlushStatus;

} // namespace frame::core

#endif // FRAME_CORE_PTY_WRITER_HPP
