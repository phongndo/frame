#include "core/pty_writer.hpp"

#include "core/input.hpp"
#include "frame/assert.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>

namespace frame::core {

// The branches are the explicit bounded outcomes of one nonblocking queue flush.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto flush_pty_write_queue(PanePtyWriteQueue& queue, std::size_t& global_budget,
                                         const PtyWriteOperation write,
                                         void* const context) noexcept -> PtyFlushStatus {
  if (queue.empty()) {
    return PtyFlushStatus::drained;
  }
  if (write == nullptr || global_budget == 0) {
    return PtyFlushStatus::pending;
  }

  std::size_t budget = std::min(pty_write_bytes_per_pane_turn_max, global_budget);
  std::size_t attempts = 0;
  while (!queue.empty() && budget > 0 && attempts < pty_write_attempts_per_pane_turn_max) {
    ++attempts;
    const auto readable = queue.readable_span();
    const auto bytes = readable.first(std::min(readable.size(), budget));
    const auto result = write(context, bytes);
    if (result.bytes > 0) {
      const auto size = static_cast<std::size_t>(result.bytes);
      if (size > bytes.size()) {
        return PtyFlushStatus::hard_error;
      }
      const bool consumed = queue.consume(size);
      FRAME_ASSERT(consumed);
      budget -= size;
      global_budget -= size;
      continue;
    }
    if (result.bytes == 0) {
      return PtyFlushStatus::hard_error;
    }
    if (result.error == EINTR) {
      continue;
    }
    if (result.error == EAGAIN || result.error == EWOULDBLOCK) {
      return PtyFlushStatus::blocked;
    }
    return PtyFlushStatus::hard_error;
  }
  return queue.empty() ? PtyFlushStatus::drained : PtyFlushStatus::pending;
}

} // namespace frame::core
