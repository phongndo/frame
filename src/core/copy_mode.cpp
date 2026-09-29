#include "core/copy_mode.hpp"

#include <algorithm>
#include <cstdint>

namespace frame::core {

// Search placement is Core UI policy over Runtime-observed canonical terminal coordinates. Small
// panes naturally retain a wider safe zone because they cannot represent quarter-pane margins.
auto copy_search_viewport_offset(const std::uint64_t match_row, const std::uint64_t current_offset,
                                 const std::uint64_t visible_rows,
                                 const std::uint64_t total_rows) noexcept -> std::uint64_t {
  if (visible_rows == 0 || total_rows <= visible_rows) {
    return 0;
  }
  const auto maximum_offset = total_rows - visible_rows;
  const auto offset = std::min(current_offset, maximum_offset);
  const auto row = std::min(match_row, total_rows - 1U);
  const auto safe_margin = visible_rows / 4U;
  const auto safe_begin = offset + safe_margin;
  const auto safe_end = offset + visible_rows - safe_margin;
  if (row >= safe_begin && row < safe_end) {
    return offset;
  }
  const auto half_viewport = visible_rows / 2U;
  const auto centered = row > half_viewport ? row - half_viewport : 0U;
  return std::min(centered, maximum_offset);
}

} // namespace frame::core
