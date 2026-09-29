#include "terminal/terminal_impl.hpp"

#include "frame/assert.hpp"
#include "frame/limits.hpp"
#include "frame/terminal/terminal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace frame::vt {
namespace {

[[nodiscard]] auto formatter_format(const ScreenFormat format) noexcept -> GhosttyFormatterFormat {
  switch (format) {
  case ScreenFormat::plain:
    return GHOSTTY_FORMATTER_FORMAT_PLAIN;
  case ScreenFormat::vt:
  case ScreenFormat::vt_full:
    return GHOSTTY_FORMATTER_FORMAT_VT;
  }
  return GHOSTTY_FORMATTER_FORMAT_PLAIN;
}

[[nodiscard]] constexpr auto point_tag(const PointSpace space) noexcept -> GhosttyPointTag {
  switch (space) {
  case PointSpace::active:
    return GHOSTTY_POINT_TAG_ACTIVE;
  case PointSpace::viewport:
    return GHOSTTY_POINT_TAG_VIEWPORT;
  case PointSpace::screen:
    return GHOSTTY_POINT_TAG_SCREEN;
  case PointSpace::history:
    return GHOSTTY_POINT_TAG_HISTORY;
  }
  return GHOSTTY_POINT_TAG_VIEWPORT;
}

[[nodiscard]] constexpr auto ghostty_point(const TerminalPoint point) noexcept -> GhosttyPoint {
  return {
      .tag = point_tag(point.space),
      .value = {.coordinate = {.x = point.column, .y = point.row}},
  };
}

[[nodiscard]] auto grid_ref(const GhosttyTerminal terminal, const TerminalPoint point) noexcept
    -> std::expected<GhosttyGridRef, Error> {
  GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
  const auto result = ghostty_terminal_grid_ref(terminal, ghostty_point(point), &ref);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return ref;
}

[[nodiscard]] auto active_selection(const GhosttyTerminal terminal) noexcept
    -> std::expected<std::optional<GhosttySelection>, Error> {
  GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
  const auto result = ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &selection);
  if (result == GHOSTTY_NO_VALUE) {
    return std::optional<GhosttySelection>{};
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return std::optional<GhosttySelection>{selection};
}

[[nodiscard]] auto install_selection(const GhosttyTerminal terminal,
                                     const GhosttySelection& selection) noexcept
    -> std::expected<void, Error> {
  const auto result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return {};
}

[[nodiscard]] auto total_rows(const GhosttyTerminal terminal) noexcept
    -> std::expected<std::size_t, Error> {
  std::size_t rows = 0;
  const auto result = ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TOTAL_ROWS, &rows);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return rows;
}

[[nodiscard]] auto point_has_text(const GhosttyTerminal terminal,
                                  const TerminalPoint point) noexcept
    -> std::expected<bool, Error> {
  const auto ref = grid_ref(terminal, point);
  if (!ref.has_value()) {
    return std::unexpected(ref.error());
  }
  GhosttyCell cell = 0;
  auto result = ghostty_grid_ref_cell(&*ref, &cell);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  bool has_text = false;
  result = ghostty_cell_get(cell, GHOSTTY_CELL_DATA_HAS_TEXT, &has_text);
  return result == GHOSTTY_SUCCESS ? std::expected<bool, Error>{has_text}
                                   : std::unexpected(detail::map_error(result));
}

[[nodiscard]] auto row_has_content(const GhosttyTerminal terminal, const PointSpace space,
                                   const std::uint32_t row, const std::uint16_t columns) noexcept
    -> std::expected<bool, Error> {
  const auto first = grid_ref(terminal, {.space = space, .column = 0, .row = row});
  if (!first.has_value()) {
    return std::unexpected(first.error());
  }
  GhosttyRow value = 0;
  auto result = ghostty_grid_ref_row(&*first, &value);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  bool styled = false;
  GhosttyRowSemanticPrompt prompt = GHOSTTY_ROW_SEMANTIC_NONE;
  const std::array keys{GHOSTTY_ROW_DATA_STYLED, GHOSTTY_ROW_DATA_SEMANTIC_PROMPT};
  std::array<void*, keys.size()> values{&styled, &prompt};
  std::size_t written = 0;
  result = ghostty_row_get_multi(value, keys.size(), keys.data(), values.data(), &written);
  if (result != GHOSTTY_SUCCESS || written != keys.size()) {
    return std::unexpected(detail::map_error(result));
  }
  if (styled || prompt != GHOSTTY_ROW_SEMANTIC_NONE) {
    return true;
  }
  for (std::uint16_t column = 0; column < columns; ++column) {
    const auto has_text = point_has_text(terminal, {.space = space, .column = column, .row = row});
    if (!has_text.has_value()) {
      return std::unexpected(has_text.error());
    }
    if (*has_text) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] auto
format_selection_buffer(const GhosttyTerminal terminal, const GhosttySelection& selection,
                        const ScreenFormat format, const std::span<std::byte> output,
                        const bool unwrap) noexcept -> std::expected<std::size_t, Error> {
  GhosttyTerminalSelectionFormatOptions options{};
  options.size = sizeof(options);
  options.emit = formatter_format(format);
  options.unwrap = unwrap;
  options.trim = format != ScreenFormat::vt_full;
  options.selection = &selection;
  // std::byte and uint8_t are both byte views; Ghostty's C ABI uses the latter.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* output_data = reinterpret_cast<std::uint8_t*>(output.data());
  std::size_t bytes_written = 0;
  const auto result = ghostty_terminal_selection_format_buf(terminal, options, output_data,
                                                            output.size(), &bytes_written);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  FRAME_ASSERT(bytes_written <= output.size());
  return bytes_written;
}

[[nodiscard]] auto point_from_ref(const GhosttyTerminal terminal, const GhosttyGridRef& ref,
                                  const PointSpace space) noexcept
    -> std::expected<TerminalPoint, Error> {
  GhosttyPointCoordinate coordinate{};
  const auto result =
      ghostty_terminal_point_from_grid_ref(terminal, &ref, point_tag(space), &coordinate);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return TerminalPoint{.space = space, .column = coordinate.x, .row = coordinate.y};
}

[[nodiscard]] auto word_candidate(const GhosttyGridRef& ref) noexcept
    -> std::expected<bool, Error> {
  GhosttyCell cell = 0;
  auto result = ghostty_grid_ref_cell(&ref, &cell);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  bool has_text = false;
  std::uint32_t codepoint = 0;
  const std::array keys{GHOSTTY_CELL_DATA_HAS_TEXT, GHOSTTY_CELL_DATA_CODEPOINT};
  std::array<void*, keys.size()> values{&has_text, &codepoint};
  std::size_t written = 0;
  result = ghostty_cell_get_multi(cell, keys.size(), keys.data(), values.data(), &written);
  if (result != GHOSTTY_SUCCESS || written != keys.size()) {
    return std::unexpected(detail::map_error(result));
  }
  const bool whitespace = codepoint == static_cast<std::uint32_t>(' ') ||
                          codepoint == static_cast<std::uint32_t>('\t') ||
                          codepoint == static_cast<std::uint32_t>('\r') ||
                          codepoint == static_cast<std::uint32_t>('\n');
  return has_text && !whitespace;
}

struct CellText final {
  std::uint32_t codepoint{0};
  GhosttyCellWide wide{GHOSTTY_CELL_WIDE_NARROW};
  bool has_text{false};
};

[[nodiscard]] auto cell_text(const GhosttyGridRef& ref) noexcept -> std::expected<CellText, Error> {
  GhosttyCell cell = 0;
  auto result = ghostty_grid_ref_cell(&ref, &cell);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  CellText text;
  const std::array keys{GHOSTTY_CELL_DATA_HAS_TEXT, GHOSTTY_CELL_DATA_CODEPOINT,
                        GHOSTTY_CELL_DATA_WIDE};
  std::array<void*, keys.size()> values{&text.has_text, &text.codepoint, &text.wide};
  std::size_t written = 0;
  result = ghostty_cell_get_multi(cell, keys.size(), keys.data(), values.data(), &written);
  if (result != GHOSTTY_SUCCESS || written != keys.size()) {
    return std::unexpected(detail::map_error(result));
  }
  return text;
}

[[nodiscard]] auto row_last_text_column(const GhosttyTerminal terminal, const std::uint32_t row,
                                        const std::uint16_t columns) noexcept
    -> std::expected<std::uint16_t, Error> {
  for (std::uint16_t remaining = columns; remaining > 0; --remaining) {
    const auto column = static_cast<std::uint16_t>(remaining - 1U);
    const auto ref =
        grid_ref(terminal, {.space = PointSpace::screen, .column = column, .row = row});
    if (!ref.has_value()) {
      return std::unexpected(ref.error());
    }
    const auto text = cell_text(*ref);
    if (!text.has_value()) {
      return std::unexpected(text.error());
    }
    if (text->has_text) {
      return column;
    }
  }
  return std::uint16_t{0};
}

[[nodiscard]] auto row_first_nonblank_column(const GhosttyTerminal terminal,
                                             const std::uint32_t row,
                                             const std::uint16_t columns) noexcept
    -> std::expected<std::uint16_t, Error> {
  for (std::uint16_t column = 0; column < columns; ++column) {
    const auto ref =
        grid_ref(terminal, {.space = PointSpace::screen, .column = column, .row = row});
    if (!ref.has_value()) {
      return std::unexpected(ref.error());
    }
    const auto text = cell_text(*ref);
    if (!text.has_value()) {
      return std::unexpected(text.error());
    }
    if (text->has_text && text->codepoint != static_cast<std::uint32_t>(' ') &&
        text->codepoint != static_cast<std::uint32_t>('\t')) {
      return column;
    }
  }
  return std::uint16_t{0};
}

[[nodiscard]] auto normalize_wide_tail(const GhosttyTerminal terminal, TerminalPoint point,
                                       const std::uint16_t columns,
                                       const SelectionAdjustment adjustment) noexcept
    -> std::expected<TerminalPoint, Error> {
  const auto ref = grid_ref(terminal, point);
  if (!ref.has_value()) {
    return std::unexpected(ref.error());
  }
  const auto text = cell_text(*ref);
  if (!text.has_value()) {
    return std::unexpected(text.error());
  }
  if (text->wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
    if (adjustment == SelectionAdjustment::right && point.column + 1U < columns) {
      ++point.column;
    } else if (point.column > 0) {
      --point.column;
    }
    return point;
  }
  if (text->wide != GHOSTTY_CELL_WIDE_SPACER_HEAD) {
    return point;
  }
  if (adjustment != SelectionAdjustment::right) {
    if (point.column > 0) {
      --point.column;
    }
    return point;
  }
  const auto rows = total_rows(terminal);
  if (!rows.has_value()) {
    return std::unexpected(rows.error());
  }
  if (static_cast<std::size_t>(point.row) + 1U < *rows) {
    ++point.row;
    point.column = 0;
  }
  return point;
}

[[nodiscard]] auto viewport_rows(const GhosttyTerminal terminal) noexcept
    -> std::expected<std::pair<std::uint64_t, std::uint64_t>, Error> {
  GhosttyTerminalScrollbar scrollbar{};
  const auto result = ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return std::pair{static_cast<std::uint64_t>(scrollbar.offset),
                   static_cast<std::uint64_t>(scrollbar.len)};
}

// Vim-shaped cursor motion is expressed in terminal coordinates while Ghostty remains responsible
// for grid references, wide-cell identity, and tracked endpoint installation.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto adjusted_point(const GhosttyTerminal terminal, const TerminalPoint current,
                                  const std::uint16_t columns,
                                  const SelectionAdjustment adjustment) noexcept
    -> std::expected<TerminalPoint, Error> {
  const auto row_count = total_rows(terminal);
  if (!row_count.has_value()) {
    return std::unexpected(row_count.error());
  }
  if (*row_count == 0 || columns == 0) {
    return current;
  }
  auto point = current;
  const auto last_row = static_cast<std::uint32_t>(*row_count - 1U);
  switch (adjustment) {
  case SelectionAdjustment::left:
    if (point.column > 0) {
      --point.column;
    }
    break;
  case SelectionAdjustment::right:
    if (point.column + 1U < columns) {
      ++point.column;
    }
    break;
  case SelectionAdjustment::up:
    if (point.row > 0) {
      --point.row;
    }
    break;
  case SelectionAdjustment::down:
    if (point.row < last_row) {
      ++point.row;
    }
    break;
  case SelectionAdjustment::history_top:
    point.row = 0;
    break;
  case SelectionAdjustment::history_bottom:
    point.row = last_row;
    break;
  case SelectionAdjustment::viewport_top:
  case SelectionAdjustment::viewport_middle:
  case SelectionAdjustment::viewport_bottom: {
    const auto viewport = viewport_rows(terminal);
    if (!viewport.has_value()) {
      return std::unexpected(viewport.error());
    }
    auto row = viewport->first;
    if (adjustment == SelectionAdjustment::viewport_middle && viewport->second > 0) {
      row += (viewport->second - 1U) / 2U;
    } else if (adjustment == SelectionAdjustment::viewport_bottom && viewport->second > 0) {
      row += viewport->second - 1U;
    }
    point.row = static_cast<std::uint32_t>(std::min<std::uint64_t>(row, last_row));
    break;
  }
  case SelectionAdjustment::half_page_up:
  case SelectionAdjustment::half_page_down:
  case SelectionAdjustment::page_up:
  case SelectionAdjustment::page_down: {
    const auto viewport = viewport_rows(terminal);
    if (!viewport.has_value()) {
      return std::unexpected(viewport.error());
    }
    const bool half = adjustment == SelectionAdjustment::half_page_up ||
                      adjustment == SelectionAdjustment::half_page_down;
    const auto distance =
        std::max<std::uint64_t>(1, half ? viewport->second / 2U : viewport->second);
    const bool upward = adjustment == SelectionAdjustment::half_page_up ||
                        adjustment == SelectionAdjustment::page_up;
    auto row = std::min<std::uint64_t>(last_row, point.row + distance);
    if (upward) {
      row = distance > point.row ? 0U : point.row - distance;
    }
    point.row = static_cast<std::uint32_t>(row);
    break;
  }
  case SelectionAdjustment::beginning_of_line:
    point.column = 0;
    break;
  case SelectionAdjustment::first_nonblank: {
    const auto column = row_first_nonblank_column(terminal, point.row, columns);
    if (!column.has_value()) {
      return std::unexpected(column.error());
    }
    point.column = *column;
    break;
  }
  case SelectionAdjustment::end_of_line: {
    const auto column = row_last_text_column(terminal, point.row, columns);
    if (!column.has_value()) {
      return std::unexpected(column.error());
    }
    point.column = *column;
    break;
  }
  case SelectionAdjustment::word_left:
  case SelectionAdjustment::word_right:
  case SelectionAdjustment::word_end:
    break;
  }
  return normalize_wide_tail(terminal, point, columns, adjustment);
}

[[nodiscard]] auto select_word_at(const GhosttyTerminal terminal,
                                  const GhosttyGridRef& ref) noexcept
    -> std::expected<std::optional<GhosttySelection>, Error> {
  const auto selectable = word_candidate(ref);
  if (!selectable.has_value()) {
    return std::unexpected(selectable.error());
  }
  if (!*selectable) {
    return std::optional<GhosttySelection>{};
  }
  GhosttyTerminalSelectWordOptions options = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
  options.ref = ref;
  GhosttySelection word = GHOSTTY_INIT_SIZED(GhosttySelection);
  const auto result = ghostty_terminal_select_word(terminal, &options, &word);
  if (result == GHOSTTY_NO_VALUE) {
    return std::optional<GhosttySelection>{};
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return std::optional<GhosttySelection>{word};
}

[[nodiscard]] auto
install_selection_endpoint(const GhosttyTerminal terminal, GhosttySelection& selection,
                           const GhosttyGridRef& endpoint, const bool extend) noexcept
    -> std::expected<bool, Error> {
  selection.end = endpoint;
  if (!extend) {
    selection.start = endpoint;
  }
  const auto installed = install_selection(terminal, selection);
  if (!installed.has_value()) {
    return std::unexpected(installed.error());
  }
  return true;
}

// Word movement validates tracked endpoint state, directional boundaries, and Ghostty outcomes.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto select_word_from_endpoint(const GhosttyTerminal terminal,
                                             GhosttySelection& selection,
                                             const std::uint16_t columns, const bool forward,
                                             const bool extend) noexcept
    -> std::expected<bool, Error> {
  const auto endpoint = point_from_ref(terminal, selection.end, PointSpace::screen);
  if (!endpoint.has_value()) {
    return std::unexpected(endpoint.error());
  }
  const auto rows = total_rows(terminal);
  if (!rows.has_value()) {
    return std::unexpected(rows.error());
  }
  if (*rows == 0) {
    return false;
  }

  const auto current_result = select_word_at(terminal, selection.end);
  if (!current_result.has_value()) {
    return std::unexpected(current_result.error());
  }
  const auto current_optional = current_result.value_or(std::optional<GhosttySelection>{});
  const auto current_word = current_optional.value_or(GHOSTTY_INIT_SIZED(GhosttySelection));
  std::optional<TerminalPoint> current_start;
  if (current_optional.has_value()) {
    const auto start = point_from_ref(terminal, current_word.start, PointSpace::screen);
    if (!start.has_value()) {
      return std::unexpected(start.error());
    }
    current_start = *start;
    if (!forward && *endpoint != *start) {
      return install_selection_endpoint(terminal, selection, current_word.start, extend);
    }
  }

  auto candidate = *endpoint;
  const auto advance = [columns, total_row_count = *rows, forward](TerminalPoint& point) noexcept {
    if (forward) {
      if (point.column + 1U < columns) {
        ++point.column;
        return true;
      }
      if (static_cast<std::size_t>(point.row) + 1U >= total_row_count) {
        return false;
      }
      point.column = 0;
      ++point.row;
      return true;
    }
    if (point.column > 0) {
      --point.column;
      return true;
    }
    if (point.row == 0) {
      return false;
    }
    point.column = static_cast<std::uint16_t>(columns - 1U);
    --point.row;
    return true;
  };
  if (!advance(candidate)) {
    return false;
  }

  for (std::size_t inspected = 0; inspected < limits::search_candidates_per_step; ++inspected) {
    const auto candidate_ref = grid_ref(terminal, candidate);
    if (!candidate_ref.has_value()) {
      return std::unexpected(candidate_ref.error());
    }
    const auto word_result = select_word_at(terminal, *candidate_ref);
    if (!word_result.has_value()) {
      return std::unexpected(word_result.error());
    }
    const auto word_optional = word_result.value_or(std::optional<GhosttySelection>{});
    if (word_optional.has_value()) {
      const auto word = word_optional.value_or(GHOSTTY_INIT_SIZED(GhosttySelection));
      const auto start = point_from_ref(terminal, word.start, PointSpace::screen);
      if (!start.has_value()) {
        return std::unexpected(start.error());
      }
      if (!current_start.has_value() || *start != *current_start) {
        return install_selection_endpoint(terminal, selection, word.start, extend);
      }
    }
    // A navigation key remains bounded, but installs its progress so another key can continue
    // across a blank run instead of restarting from the same endpoint forever.
    if (inspected + 1U == limits::search_candidates_per_step) {
      return install_selection_endpoint(terminal, selection, *candidate_ref, extend);
    }
    if (!advance(candidate)) {
      return false;
    }
  }
  return false;
}

// Word-end traversal shares Ghostty word selection and remains bounded per key.
// NOLINTBEGIN(readability-function-cognitive-complexity)
[[nodiscard]] auto
select_word_end_from_endpoint(const GhosttyTerminal terminal, GhosttySelection& selection,
                              const std::uint16_t columns, const bool extend) noexcept
    -> std::expected<bool, Error> {
  const auto endpoint = point_from_ref(terminal, selection.end, PointSpace::screen);
  if (!endpoint.has_value()) {
    return std::unexpected(endpoint.error());
  }
  const auto rows = total_rows(terminal);
  if (!rows.has_value()) {
    return std::unexpected(rows.error());
  }
  if (*rows == 0) {
    return false;
  }

  const auto current_result = select_word_at(terminal, selection.end);
  if (!current_result.has_value()) {
    return std::unexpected(current_result.error());
  }
  const auto current = current_result.value_or(std::optional<GhosttySelection>{});
  std::optional<TerminalPoint> current_start;
  if (current.has_value()) {
    const auto start = point_from_ref(terminal, current->start, PointSpace::screen);
    const auto end = point_from_ref(terminal, current->end, PointSpace::screen);
    if (!start.has_value() || !end.has_value()) {
      return std::unexpected(!start.has_value() ? start.error() : end.error());
    }
    current_start = *start;
    if (*endpoint != *end) {
      return install_selection_endpoint(terminal, selection, current->end, extend);
    }
  }

  auto candidate = *endpoint;
  const auto advance = [columns, total_row_count = *rows](TerminalPoint& point) noexcept {
    if (point.column + 1U < columns) {
      ++point.column;
      return true;
    }
    if (static_cast<std::size_t>(point.row) + 1U >= total_row_count) {
      return false;
    }
    point.column = 0;
    ++point.row;
    return true;
  };
  if (!advance(candidate)) {
    return false;
  }

  for (std::size_t inspected = 0; inspected < limits::search_candidates_per_step; ++inspected) {
    const auto candidate_ref = grid_ref(terminal, candidate);
    if (!candidate_ref.has_value()) {
      return std::unexpected(candidate_ref.error());
    }
    const auto word_result = select_word_at(terminal, *candidate_ref);
    if (!word_result.has_value()) {
      return std::unexpected(word_result.error());
    }
    const auto word = word_result.value_or(std::optional<GhosttySelection>{});
    if (word.has_value()) {
      const auto start = point_from_ref(terminal, word->start, PointSpace::screen);
      if (!start.has_value()) {
        return std::unexpected(start.error());
      }
      if (!current_start.has_value() || *start != *current_start) {
        return install_selection_endpoint(terminal, selection, word->end, extend);
      }
    }
    if (inspected + 1U == limits::search_candidates_per_step) {
      return install_selection_endpoint(terminal, selection, *candidate_ref, extend);
    }
    if (!advance(candidate)) {
      return false;
    }
  }
  return false;
}
// NOLINTEND(readability-function-cognitive-complexity)

[[nodiscard]] constexpr auto gesture_event_index(const SelectionGesturePhase phase) noexcept
    -> std::size_t {
  switch (phase) {
  case SelectionGesturePhase::press:
    return 0;
  case SelectionGesturePhase::drag:
    return 1;
  case SelectionGesturePhase::release:
    return 2;
  case SelectionGesturePhase::autoscroll_tick:
    return 3;
  case SelectionGesturePhase::deep_press:
    return 4;
  }
  return 0;
}

[[nodiscard]] auto set_gesture_option(const GhosttySelectionGestureEvent event,
                                      const GhosttySelectionGestureEventOption option,
                                      const void* value) noexcept -> std::expected<void, Error> {
  const auto result = ghostty_selection_gesture_event_set(event, option, value);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return {};
}

[[nodiscard]] auto gesture_autoscroll(const GhosttySelectionGestureAutoscroll value) noexcept
    -> std::expected<SelectionAutoscroll, Error> {
  switch (value) {
  case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE:
    return SelectionAutoscroll::none;
  case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_UP:
    return SelectionAutoscroll::up;
  case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_DOWN:
    return SelectionAutoscroll::down;
  case GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_MAX_VALUE:
    return std::unexpected(Error::invalid_state);
  }
  return std::unexpected(Error::invalid_state);
}

[[nodiscard]] constexpr auto ascii_fold(const std::uint8_t value) noexcept -> std::uint8_t {
  return value >= static_cast<std::uint8_t>('A') && value <= static_cast<std::uint8_t>('Z')
             ? static_cast<std::uint8_t>(value + ('a' - 'A'))
             : value;
}

struct SearchTextCursor final {
  TerminalPoint point{.space = PointSpace::screen};
  std::array<std::uint8_t, pane_ansi_grapheme_bytes_max> grapheme{};
  std::size_t grapheme_size{0};
};

[[nodiscard]] auto append_utf8(const std::uint32_t codepoint,
                               const std::span<std::uint8_t> output) noexcept
    -> std::optional<std::size_t> {
  if (codepoint <= 0x7FU && !output.empty()) {
    output.front() = static_cast<std::uint8_t>(codepoint);
    return 1;
  }
  if (codepoint <= 0x7FFU && output.size() >= 2) {
    output.subspan(0, 1).front() = static_cast<std::uint8_t>(0xC0U | (codepoint >> 6U));
    output.subspan(1, 1).front() = static_cast<std::uint8_t>(0x80U | (codepoint & 0x3FU));
    return 2;
  }
  if (codepoint <= 0xFFFFU && output.size() >= 3) {
    output.subspan(0, 1).front() = static_cast<std::uint8_t>(0xE0U | (codepoint >> 12U));
    output.subspan(1, 1).front() = static_cast<std::uint8_t>(0x80U | ((codepoint >> 6U) & 0x3FU));
    output.subspan(2, 1).front() = static_cast<std::uint8_t>(0x80U | (codepoint & 0x3FU));
    return 3;
  }
  if (codepoint <= 0x10FFFFU && output.size() >= 4) {
    output.subspan(0, 1).front() = static_cast<std::uint8_t>(0xF0U | (codepoint >> 18U));
    output.subspan(1, 1).front() = static_cast<std::uint8_t>(0x80U | ((codepoint >> 12U) & 0x3FU));
    output.subspan(2, 1).front() = static_cast<std::uint8_t>(0x80U | ((codepoint >> 6U) & 0x3FU));
    output.subspan(3, 1).front() = static_cast<std::uint8_t>(0x80U | (codepoint & 0x3FU));
    return 4;
  }
  return std::nullopt;
}

[[nodiscard]] auto load_grapheme(const GhosttyTerminal terminal, SearchTextCursor& cursor) noexcept
    -> std::expected<void, Error> {
  const auto ref = grid_ref(terminal, cursor.point);
  if (!ref.has_value()) {
    return std::unexpected(ref.error());
  }
  std::array<std::uint32_t, pane_grapheme_codepoints_max> codepoints{};
  std::size_t codepoint_count = 0;
  const auto result =
      ghostty_grid_ref_graphemes(&*ref, codepoints.data(), codepoints.size(), &codepoint_count);
  if (result == GHOSTTY_OUT_OF_SPACE) {
    return std::unexpected(Error::limit_exceeded);
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  cursor.grapheme_size = 0;
  for (const auto codepoint : std::span(codepoints).first(codepoint_count)) {
    const auto encoded =
        append_utf8(codepoint, std::span(cursor.grapheme).subspan(cursor.grapheme_size));
    if (!encoded.has_value()) {
      return std::unexpected(Error::limit_exceeded);
    }
    cursor.grapheme_size += *encoded;
  }
  return {};
}

[[nodiscard]] auto advance_search_candidate(TerminalPoint& point, const SearchDirection direction,
                                            const std::size_t rows,
                                            const std::uint16_t columns) noexcept -> bool {
  if (direction == SearchDirection::forward) {
    if (point.column + 1U < columns) {
      ++point.column;
      return true;
    }
    if (static_cast<std::size_t>(point.row) + 1U >= rows) {
      return false;
    }
    point.column = 0;
    ++point.row;
    return true;
  }
  if (point.column > 0) {
    --point.column;
    return true;
  }
  if (point.row == 0) {
    return false;
  }
  point.column = static_cast<std::uint16_t>(columns - 1U);
  --point.row;
  return true;
}

} // namespace

auto Terminal::format_screen(const ScreenFormat format, const std::span<std::byte> output,
                             const bool unwrap) noexcept -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);

  GhosttyFormatterTerminalOptions options{};
  options.size = sizeof(options);
  options.emit = formatter_format(format);
  options.unwrap = unwrap;
  options.trim = format != ScreenFormat::vt_full;
  options.extra.size = sizeof(options.extra);
  options.extra.screen.size = sizeof(options.extra.screen);
  const bool styled = format != ScreenFormat::plain;
  options.extra.screen.cursor = styled;
  options.extra.screen.style = styled;
  options.extra.screen.hyperlink = styled;

  GhosttyFormatter formatter = nullptr;
  auto result = ghostty_formatter_terminal_new(impl_->allocator.native(), &formatter,
                                               impl_->terminal, options);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  // std::byte and uint8_t are both byte views; Ghostty's C ABI uses the latter.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* output_data = reinterpret_cast<std::uint8_t*>(output.data());
  std::size_t bytes_written = 0;
  result = ghostty_formatter_format_buf(formatter, output_data, output.size(), &bytes_written);
  ghostty_formatter_free(formatter);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  FRAME_ASSERT(bytes_written <= output.size());
  return bytes_written;
}

// Selection construction and caller-buffer formatting have independent bounded failures.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::format_visible_tail(const ScreenFormat format, const std::size_t rows,
                                   const std::span<std::byte> output, const bool unwrap) noexcept
    -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  const auto terminal_size = size();
  if (rows == 0 || terminal_size.columns == 0 || terminal_size.rows == 0) {
    return std::unexpected(Error::invalid_options);
  }
  std::size_t relevant_rows = 0;
  for (std::size_t remaining = terminal_size.rows; remaining > 0; --remaining) {
    const auto row = remaining - 1U;
    const auto has_content =
        row_has_content(impl_->terminal, PointSpace::viewport, static_cast<std::uint32_t>(row),
                        terminal_size.columns);
    if (!has_content.has_value()) {
      return std::unexpected(has_content.error());
    }
    if (*has_content) {
      relevant_rows = remaining;
      break;
    }
  }
  if (relevant_rows == 0) {
    return std::size_t{0};
  }
  const auto first_row = relevant_rows > rows ? relevant_rows - rows : 0U;
  const auto start = grid_ref(
      impl_->terminal,
      {.space = PointSpace::viewport, .column = 0, .row = static_cast<std::uint32_t>(first_row)});
  const auto end =
      grid_ref(impl_->terminal, {.space = PointSpace::viewport,
                                 .column = static_cast<std::uint16_t>(terminal_size.columns - 1U),
                                 .row = static_cast<std::uint32_t>(relevant_rows - 1U)});
  if (!start.has_value() || !end.has_value()) {
    return std::unexpected(!start.has_value() ? start.error() : end.error());
  }
  const GhosttySelection selection{
      .size = sizeof(GhosttySelection), .start = *start, .end = *end, .rectangle = false};
  return format_selection_buffer(impl_->terminal, selection, format, output, unwrap);
}

// Selection construction and caller-buffer formatting have independent bounded failures.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::format_recent(const ScreenFormat format, const std::size_t rows,
                             const std::span<std::byte> output, const bool unwrap) noexcept
    -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  if (rows == 0) {
    return std::unexpected(Error::invalid_options);
  }
  const auto available = total_rows(impl_->terminal);
  if (!available.has_value() || *available == 0) {
    return std::unexpected(available.has_value() ? Error::invalid_state : available.error());
  }
  const auto inspected = inspection();
  if (!inspected.has_value()) {
    return std::unexpected(inspected.error());
  }
  const auto terminal_size = size();
  const auto columns = terminal_size.columns;
  const auto cursor_end = inspected->scrollback_rows + inspected->cursor_row + 1U;
  auto relevant_rows = std::min(*available, cursor_end);
  const auto active_start =
      *available > terminal_size.rows ? *available - terminal_size.rows : std::size_t{0};
  for (std::size_t remaining = *available; remaining > active_start; --remaining) {
    const auto row = remaining - 1U;
    if (row > std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(Error::limit_exceeded);
    }
    const auto has_content = row_has_content(impl_->terminal, PointSpace::screen,
                                             static_cast<std::uint32_t>(row), columns);
    if (!has_content.has_value()) {
      return std::unexpected(has_content.error());
    }
    if (*has_content) {
      relevant_rows = std::max(relevant_rows, remaining);
      break;
    }
  }
  const auto first_row = relevant_rows > rows ? relevant_rows - rows : 0U;
  if (relevant_rows == 0 || first_row > std::numeric_limits<std::uint32_t>::max() ||
      relevant_rows - 1U > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(Error::limit_exceeded);
  }
  const auto start = grid_ref(
      impl_->terminal,
      {.space = PointSpace::screen, .column = 0, .row = static_cast<std::uint32_t>(first_row)});
  const auto end =
      grid_ref(impl_->terminal, {.space = PointSpace::screen,
                                 .column = static_cast<std::uint16_t>(columns - 1U),
                                 .row = static_cast<std::uint32_t>(relevant_rows - 1U)});
  if (!start.has_value() || !end.has_value()) {
    return std::unexpected(!start.has_value() ? start.error() : end.error());
  }
  const GhosttySelection selection{
      .size = sizeof(GhosttySelection), .start = *start, .end = *end, .rectangle = false};
  return format_selection_buffer(impl_->terminal, selection, format, output, unwrap);
}

// Bounded reverse traversal qualifies candidate cells before asking Ghostty for semantic output.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::format_last_command(const ScreenFormat format, const std::span<std::byte> output,
                                   const bool unwrap) noexcept
    -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  const auto available = total_rows(impl_->terminal);
  if (!available.has_value() || *available == 0) {
    return std::unexpected(available.has_value() ? Error::invalid_state : available.error());
  }
  const auto columns = size().columns;
  constexpr std::size_t inspected_cells_max = std::size_t{64} * 1'024U;
  std::size_t inspected = 0;
  for (std::size_t remaining_rows = *available;
       remaining_rows > 0 && inspected < inspected_cells_max; --remaining_rows) {
    const auto row = static_cast<std::uint32_t>(remaining_rows - 1U);
    for (std::uint16_t remaining_columns = columns;
         remaining_columns > 0 && inspected < inspected_cells_max; --remaining_columns) {
      const auto column = static_cast<std::uint16_t>(remaining_columns - 1U);
      const auto ref =
          grid_ref(impl_->terminal, {.space = PointSpace::screen, .column = column, .row = row});
      ++inspected;
      if (!ref.has_value()) {
        return std::unexpected(ref.error());
      }
      GhosttyCell cell = 0;
      auto result = ghostty_grid_ref_cell(&*ref, &cell);
      if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(detail::map_error(result));
      }
      bool has_text = false;
      GhosttyCellSemanticContent semantic = GHOSTTY_CELL_SEMANTIC_OUTPUT;
      const std::array keys{GHOSTTY_CELL_DATA_HAS_TEXT, GHOSTTY_CELL_DATA_SEMANTIC_CONTENT};
      std::array<void*, keys.size()> values{&has_text, &semantic};
      std::size_t written = 0;
      result = ghostty_cell_get_multi(cell, keys.size(), keys.data(), values.data(), &written);
      if (result != GHOSTTY_SUCCESS || written != keys.size()) {
        return std::unexpected(detail::map_error(result));
      }
      if (has_text && semantic == GHOSTTY_CELL_SEMANTIC_INPUT) {
        return std::size_t{0};
      }
      if (!has_text || semantic != GHOSTTY_CELL_SEMANTIC_OUTPUT) {
        continue;
      }
      GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
      result = ghostty_terminal_select_output(impl_->terminal, *ref, &selection);
      if (result == GHOSTTY_NO_VALUE) {
        continue;
      }
      if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(detail::map_error(result));
      }
      return format_selection_buffer(impl_->terminal, selection, format, output, unwrap);
    }
  }
  return std::unexpected(Error::invalid_state);
}

// Event phases have intentionally distinct Ghostty option contracts.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::selection_gesture(const SelectionGestureEvent& event) noexcept
    -> std::expected<SelectionGestureResult, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);

  if (event.cell_width == 0 || event.screen_height == 0 ||
      ((!event.has_point) && (event.phase == SelectionGesturePhase::press ||
                              event.phase == SelectionGesturePhase::drag)) ||
      (event.phase == SelectionGesturePhase::autoscroll_tick &&
       event.point.space != PointSpace::viewport)) {
    return std::unexpected(Error::invalid_options);
  }

  if (impl_->selection_gesture == nullptr) {
    const auto result =
        ghostty_selection_gesture_new(impl_->allocator.native(), &impl_->selection_gesture);
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
  }
  constexpr std::array event_types{
      GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS,
      GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG,
      GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE,
      GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_AUTOSCROLL_TICK,
      GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DEEP_PRESS,
  };
  for (std::size_t index = 0; index < event_types.size(); ++index) {
    auto& native_event = std::span(impl_->selection_events).subspan(index, 1).front();
    if (native_event != nullptr) {
      continue;
    }
    const auto result = ghostty_selection_gesture_event_new(
        impl_->allocator.native(), &native_event, std::span(event_types).subspan(index, 1).front());
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
  }

  auto* const native_event =
      std::span(impl_->selection_events).subspan(gesture_event_index(event.phase), 1).front();
  GhosttySelectionGestureGeometry geometry{
      .columns = impl_->options.size.columns,
      .cell_width = event.cell_width,
      .padding_left = event.padding_left,
      .screen_height = event.screen_height,
  };
  GhosttySurfacePosition position{
      .x = event.pointer_x,
      .y = event.pointer_y,
  };
  const auto ref = event.has_point && event.phase != SelectionGesturePhase::autoscroll_tick &&
                           event.phase != SelectionGesturePhase::deep_press
                       ? grid_ref(impl_->terminal, event.point)
                       : std::expected<GhosttyGridRef, Error>{GHOSTTY_INIT_SIZED(GhosttyGridRef)};
  if (!ref.has_value()) {
    return std::unexpected(ref.error());
  }

  auto configured = std::expected<void, Error>{};
  switch (event.phase) {
  case SelectionGesturePhase::press:
    configured = set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &*ref);
    if (configured.has_value()) {
      configured = set_gesture_option(
          native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION,
          event.has_pointer_position ? static_cast<const void*>(&position) : nullptr);
    }
    if (configured.has_value()) {
      configured = set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS,
                                      event.time_ns != 0 ? static_cast<const void*>(&event.time_ns)
                                                         : nullptr);
    }
    if (configured.has_value()) {
      configured = set_gesture_option(
          native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS,
          event.repeat_interval_ns != 0 ? static_cast<const void*>(&event.repeat_interval_ns)
                                        : nullptr);
    }
    if (configured.has_value()) {
      configured = set_gesture_option(
          native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_DISTANCE,
          event.repeat_distance > 0 ? static_cast<const void*>(&event.repeat_distance) : nullptr);
    }
    break;
  case SelectionGesturePhase::drag:
    configured = set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &*ref);
    if (configured.has_value()) {
      configured =
          set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry);
    }
    if (configured.has_value()) {
      configured = set_gesture_option(
          native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION,
          event.has_pointer_position ? static_cast<const void*>(&position) : nullptr);
    }
    if (configured.has_value()) {
      configured = set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE,
                                      &event.rectangle);
    }
    break;
  case SelectionGesturePhase::release:
    configured = set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF,
                                    event.has_point ? static_cast<const void*>(&*ref) : nullptr);
    break;
  case SelectionGesturePhase::autoscroll_tick: {
    const GhosttyPointCoordinate viewport{.x = event.point.column, .y = event.point.row};
    configured =
        set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_VIEWPORT, &viewport);
    if (configured.has_value()) {
      configured =
          set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geometry);
    }
    if (configured.has_value()) {
      configured = set_gesture_option(
          native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION,
          event.has_pointer_position ? static_cast<const void*>(&position) : nullptr);
    }
    if (configured.has_value()) {
      configured = set_gesture_option(native_event, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE,
                                      &event.rectangle);
    }
    break;
  }
  case SelectionGesturePhase::deep_press:
    break;
  }
  if (!configured.has_value()) {
    return std::unexpected(configured.error());
  }

  GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
  const auto result = ghostty_selection_gesture_event(impl_->selection_gesture, impl_->terminal,
                                                      native_event, &selection);
  bool selection_changed = false;
  if (result == GHOSTTY_SUCCESS) {
    const auto installed = install_selection(impl_->terminal, selection);
    if (!installed.has_value()) {
      return std::unexpected(installed.error());
    }
    selection_changed = true;
  } else if (result != GHOSTTY_NO_VALUE) {
    return std::unexpected(detail::map_error(result));
  }

  bool dragged = false;
  GhosttySelectionGestureAutoscroll autoscroll = GHOSTTY_SELECTION_GESTURE_AUTOSCROLL_NONE;
  const std::array keys{GHOSTTY_SELECTION_GESTURE_DATA_DRAGGED,
                        GHOSTTY_SELECTION_GESTURE_DATA_AUTOSCROLL};
  std::array<void*, keys.size()> values{&dragged, &autoscroll};
  std::size_t written = 0;
  const auto state_result = ghostty_selection_gesture_get_multi(
      impl_->selection_gesture, impl_->terminal, keys.size(), keys.data(), values.data(), &written);
  if (state_result != GHOSTTY_SUCCESS || written != keys.size()) {
    return std::unexpected(detail::map_error(state_result));
  }
  const auto mapped_autoscroll = gesture_autoscroll(autoscroll);
  if (!mapped_autoscroll.has_value()) {
    return std::unexpected(mapped_autoscroll.error());
  }
  return SelectionGestureResult{
      .autoscroll = *mapped_autoscroll,
      .selection_changed = selection_changed,
      .dragged = dragged,
  };
}

void Terminal::reset_selection_gesture() noexcept {
  FRAME_ASSERT(impl_ != nullptr);
  ghostty_selection_gesture_reset(impl_->selection_gesture, impl_->terminal);
}

auto Terminal::select(const SelectionUnit unit, const TerminalPoint point) noexcept
    -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);

  GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
  GhosttyResult result = GHOSTTY_INVALID_VALUE;
  if (unit == SelectionUnit::all) {
    result = ghostty_terminal_select_all(impl_->terminal, &selection);
  } else {
    const auto ref = grid_ref(impl_->terminal, point);
    if (!ref.has_value()) {
      return std::unexpected(ref.error());
    }
    switch (unit) {
    case SelectionUnit::cell:
      selection.start = *ref;
      selection.end = *ref;
      result = GHOSTTY_SUCCESS;
      break;
    case SelectionUnit::block:
      selection.start = *ref;
      selection.end = *ref;
      selection.rectangle = true;
      result = GHOSTTY_SUCCESS;
      break;
    case SelectionUnit::word: {
      GhosttyTerminalSelectWordOptions options =
          GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
      options.ref = *ref;
      result = ghostty_terminal_select_word(impl_->terminal, &options, &selection);
      break;
    }
    case SelectionUnit::line: {
      GhosttyTerminalSelectLineOptions options =
          GHOSTTY_INIT_SIZED(GhosttyTerminalSelectLineOptions);
      options.ref = *ref;
      options.semantic_prompt_boundary = true;
      result = ghostty_terminal_select_line(impl_->terminal, &options, &selection);
      break;
    }
    case SelectionUnit::output:
      result = ghostty_terminal_select_output(impl_->terminal, *ref, &selection);
      break;
    case SelectionUnit::all:
      break;
    }
  }
  if (result == GHOSTTY_NO_VALUE) {
    return false;
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  const auto installed = install_selection(impl_->terminal, selection);
  if (!installed.has_value()) {
    return std::unexpected(installed.error());
  }
  return true;
}

auto Terminal::selection_adjust(const SelectionAdjustment adjustment, const bool extend) noexcept
    -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto current = active_selection(impl_->terminal);
  if (!current.has_value()) {
    return std::unexpected(current.error());
  }
  if (!current->has_value()) {
    return false;
  }
  auto selection = **current;
  if (adjustment == SelectionAdjustment::word_left ||
      adjustment == SelectionAdjustment::word_right) {
    return select_word_from_endpoint(impl_->terminal, selection, impl_->options.size.columns,
                                     adjustment == SelectionAdjustment::word_right, extend);
  }
  if (adjustment == SelectionAdjustment::word_end) {
    return select_word_end_from_endpoint(impl_->terminal, selection, impl_->options.size.columns,
                                         extend);
  }
  const auto current_point = point_from_ref(impl_->terminal, selection.end, PointSpace::screen);
  if (!current_point.has_value()) {
    return std::unexpected(current_point.error());
  }
  const auto target =
      adjusted_point(impl_->terminal, *current_point, impl_->options.size.columns, adjustment);
  if (!target.has_value()) {
    return std::unexpected(target.error());
  }
  if (*target == *current_point) {
    return false;
  }
  const auto endpoint = grid_ref(impl_->terminal, *target);
  if (!endpoint.has_value()) {
    return std::unexpected(endpoint.error());
  }
  selection.end = *endpoint;
  if (!extend) {
    selection.start = selection.end;
    selection.rectangle = false;
  }
  const auto installed = install_selection(impl_->terminal, selection);
  if (!installed.has_value()) {
    return std::unexpected(installed.error());
  }
  return true;
}

auto Terminal::selection_set_unit(const SelectionUnit unit) noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto endpoint = selection_endpoint(PointSpace::screen);
  if (!endpoint.has_value()) {
    return std::unexpected(endpoint.error());
  }
  if (!endpoint->has_value()) {
    return false;
  }
  if (unit == SelectionUnit::word || unit == SelectionUnit::output || unit == SelectionUnit::all) {
    return std::unexpected(Error::invalid_options);
  }
  const auto selected = select(unit, **endpoint);
  if (!selected.has_value() || !*selected) {
    return selected;
  }
  return unit == SelectionUnit::cell ? std::expected<bool, Error>{true}
                                     : selection_normalize_unit(unit);
}

// Unit normalization validates and installs the complete tracked range in one transaction.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::selection_normalize_unit(const SelectionUnit unit) noexcept
    -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto current = active_selection(impl_->terminal);
  if (!current.has_value()) {
    return std::unexpected(current.error());
  }
  if (!current->has_value()) {
    return false;
  }
  if (unit == SelectionUnit::word || unit == SelectionUnit::output || unit == SelectionUnit::all) {
    return std::unexpected(Error::invalid_options);
  }
  auto selection = **current;
  if (unit == SelectionUnit::cell) {
    selection.rectangle = false;
  } else if (unit == SelectionUnit::line) {
    const auto start = point_from_ref(impl_->terminal, selection.start, PointSpace::screen);
    const auto end = point_from_ref(impl_->terminal, selection.end, PointSpace::screen);
    if (!start.has_value() || !end.has_value()) {
      return std::unexpected(!start.has_value() ? start.error() : end.error());
    }
    auto anchor = *start;
    auto endpoint = *end;
    if (endpoint.row >= anchor.row) {
      anchor.column = 0;
      endpoint.column = static_cast<std::uint16_t>(impl_->options.size.columns - 1U);
    } else {
      anchor.column = static_cast<std::uint16_t>(impl_->options.size.columns - 1U);
      endpoint.column = 0;
    }
    const auto anchor_ref = grid_ref(impl_->terminal, anchor);
    const auto endpoint_ref = grid_ref(impl_->terminal, endpoint);
    if (!anchor_ref.has_value() || !endpoint_ref.has_value()) {
      return std::unexpected(!anchor_ref.has_value() ? anchor_ref.error() : endpoint_ref.error());
    }
    selection.start = *anchor_ref;
    selection.end = *endpoint_ref;
    selection.rectangle = false;
  } else {
    selection.rectangle = true;
  }
  const auto installed = install_selection(impl_->terminal, selection);
  if (!installed.has_value()) {
    return std::unexpected(installed.error());
  }
  return true;
}

auto Terminal::swap_selection_endpoints() noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto current = active_selection(impl_->terminal);
  if (!current.has_value()) {
    return std::unexpected(current.error());
  }
  if (!current->has_value()) {
    return false;
  }
  auto selection = **current;
  std::swap(selection.start, selection.end);
  const auto installed = install_selection(impl_->terminal, selection);
  if (!installed.has_value()) {
    return std::unexpected(installed.error());
  }
  return true;
}

auto Terminal::collapse_selection_to_endpoint() noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto current = active_selection(impl_->terminal);
  if (!current.has_value()) {
    return std::unexpected(current.error());
  }
  if (!current->has_value()) {
    return false;
  }
  auto selection = **current;
  selection.start = selection.end;
  selection.rectangle = false;
  const auto installed = install_selection(impl_->terminal, selection);
  if (!installed.has_value()) {
    return std::unexpected(installed.error());
  }
  return true;
}

auto Terminal::selection_active() const noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto selection = active_selection(impl_->terminal);
  if (!selection.has_value()) {
    return std::unexpected(selection.error());
  }
  return selection->has_value();
}

auto Terminal::selection_endpoint(const PointSpace space) const noexcept
    -> std::expected<std::optional<TerminalPoint>, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto selection = active_selection(impl_->terminal);
  if (!selection.has_value()) {
    return std::unexpected(selection.error());
  }
  if (!selection->has_value()) {
    return std::optional<TerminalPoint>{};
  }
  GhosttyPointCoordinate coordinate{};
  const auto result = ghostty_terminal_point_from_grid_ref(impl_->terminal, &(**selection).end,
                                                           point_tag(space), &coordinate);
  if (result == GHOSTTY_NO_VALUE) {
    return std::optional<TerminalPoint>{};
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return std::optional<TerminalPoint>{
      {.space = space, .column = coordinate.x, .row = coordinate.y}};
}

auto Terminal::selection_range(const PointSpace space) const noexcept
    -> std::expected<std::optional<SelectionRange>, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto selection = active_selection(impl_->terminal);
  if (!selection.has_value()) {
    return std::unexpected(selection.error());
  }
  if (!selection->has_value()) {
    return std::optional<SelectionRange>{};
  }
  const auto start = point_from_ref(impl_->terminal, (**selection).start, space);
  const auto end = point_from_ref(impl_->terminal, (**selection).end, space);
  if (!start.has_value() || !end.has_value()) {
    return std::unexpected(!start.has_value() ? start.error() : end.error());
  }
  return std::optional<SelectionRange>{
      {.start = *start, .end = *end, .rectangular = (**selection).rectangle}};
}

auto Terminal::checkpoint_selection() noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto current = active_selection(impl_->terminal);
  if (!current.has_value()) {
    return std::unexpected(current.error());
  }
  if (!current->has_value()) {
    return false;
  }
  const auto start = point_from_ref(impl_->terminal, (**current).start, PointSpace::screen);
  const auto end = point_from_ref(impl_->terminal, (**current).end, PointSpace::screen);
  if (!start.has_value() || !end.has_value()) {
    return std::unexpected(!start.has_value() ? start.error() : end.error());
  }

  GhosttyTrackedGridRef replacement_start = nullptr;
  GhosttyTrackedGridRef replacement_end = nullptr;
  auto result =
      ghostty_terminal_grid_ref_track(impl_->terminal, ghostty_point(*start), &replacement_start);
  if (result == GHOSTTY_SUCCESS) {
    result =
        ghostty_terminal_grid_ref_track(impl_->terminal, ghostty_point(*end), &replacement_end);
  }
  if (result != GHOSTTY_SUCCESS) {
    ghostty_tracked_grid_ref_free(replacement_start);
    ghostty_tracked_grid_ref_free(replacement_end);
    return std::unexpected(detail::map_error(result));
  }

  ghostty_tracked_grid_ref_free(impl_->selection_checkpoint_start);
  ghostty_tracked_grid_ref_free(impl_->selection_checkpoint_end);
  impl_->selection_checkpoint_start = replacement_start;
  impl_->selection_checkpoint_end = replacement_end;
  impl_->selection_checkpoint_rectangle = (**current).rectangle;
  return true;
}

auto Terminal::selection_checkpoint_endpoint(const PointSpace space) const noexcept
    -> std::expected<std::optional<TerminalPoint>, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  if (impl_->selection_checkpoint_end == nullptr) {
    return std::optional<TerminalPoint>{};
  }
  GhosttyGridRef endpoint = GHOSTTY_INIT_SIZED(GhosttyGridRef);
  const auto result = ghostty_tracked_grid_ref_snapshot(impl_->selection_checkpoint_end, &endpoint);
  if (result == GHOSTTY_NO_VALUE) {
    return std::optional<TerminalPoint>{};
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  const auto point = point_from_ref(impl_->terminal, endpoint, space);
  if (!point.has_value()) {
    return std::unexpected(point.error());
  }
  return std::optional<TerminalPoint>{*point};
}

auto Terminal::restore_selection_checkpoint() noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  if (impl_->selection_checkpoint_start == nullptr || impl_->selection_checkpoint_end == nullptr) {
    return false;
  }
  GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
  auto result =
      ghostty_tracked_grid_ref_snapshot(impl_->selection_checkpoint_start, &selection.start);
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_tracked_grid_ref_snapshot(impl_->selection_checkpoint_end, &selection.end);
  }
  if (result == GHOSTTY_NO_VALUE) {
    return false;
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  selection.rectangle = impl_->selection_checkpoint_rectangle;
  const auto installed = install_selection(impl_->terminal, selection);
  if (!installed.has_value()) {
    return std::unexpected(installed.error());
  }
  return true;
}

void Terminal::clear_selection_checkpoint() noexcept {
  FRAME_ASSERT(impl_ != nullptr);
  ghostty_tracked_grid_ref_free(impl_->selection_checkpoint_start);
  ghostty_tracked_grid_ref_free(impl_->selection_checkpoint_end);
  impl_->selection_checkpoint_start = nullptr;
  impl_->selection_checkpoint_end = nullptr;
  impl_->selection_checkpoint_rectangle = false;
}

auto Terminal::refresh_selection() noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto selection = active_selection(impl_->terminal);
  if (!selection.has_value()) {
    return std::unexpected(selection.error());
  }
  if (!selection->has_value()) {
    return false;
  }
  const auto start = point_from_ref(impl_->terminal, (**selection).start, PointSpace::screen);
  const auto end = point_from_ref(impl_->terminal, (**selection).end, PointSpace::screen);
  if (!start.has_value() || !end.has_value()) {
    return std::unexpected(!start.has_value() ? start.error() : end.error());
  }
  const auto start_ref = grid_ref(impl_->terminal, *start);
  const auto end_ref = grid_ref(impl_->terminal, *end);
  if (!start_ref.has_value() || !end_ref.has_value()) {
    return std::unexpected(!start_ref.has_value() ? start_ref.error() : end_ref.error());
  }
  auto refreshed = **selection;
  refreshed.start = *start_ref;
  refreshed.end = *end_ref;
  const auto installed = install_selection(impl_->terminal, refreshed);
  if (!installed.has_value()) {
    return std::unexpected(installed.error());
  }
  return true;
}

void Terminal::clear_selection() noexcept {
  FRAME_ASSERT(impl_ != nullptr);
  static_cast<void>(ghostty_terminal_set(impl_->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr));
}

auto Terminal::format_selection(const ScreenFormat format,
                                const std::span<std::byte> output) noexcept
    -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  if (output.size() > limits::selection_format_bytes_max) {
    return std::unexpected(Error::limit_exceeded);
  }
  GhosttyTerminalSelectionFormatOptions options =
      GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
  options.emit = formatter_format(format);
  options.unwrap = true;
  options.trim = true;
  options.selection = nullptr;
  // std::byte and uint8_t are both byte views; Ghostty's C ABI uses the latter.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* output_data = reinterpret_cast<std::uint8_t*>(output.data());
  std::size_t written = 0;
  const auto result = ghostty_terminal_selection_format_buf(impl_->terminal, options, output_data,
                                                            output.size(), &written);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  FRAME_ASSERT(written <= output.size());
  return written;
}

void Terminal::scroll_viewport(const ViewportScroll behavior, const std::int64_t value) noexcept {
  FRAME_ASSERT(impl_ != nullptr);
  GhosttyTerminalScrollViewport scroll{};
  switch (behavior) {
  case ViewportScroll::top:
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_TOP;
    break;
  case ViewportScroll::bottom:
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
    break;
  case ViewportScroll::delta:
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    scroll.value.delta = static_cast<std::intptr_t>(
        std::clamp<std::int64_t>(value, std::numeric_limits<std::intptr_t>::min(),
                                 std::numeric_limits<std::intptr_t>::max()));
    break;
  case ViewportScroll::row:
    scroll.tag = GHOSTTY_SCROLL_VIEWPORT_ROW;
    scroll.value.row = value <= 0 ? 0 : static_cast<std::size_t>(value);
    break;
  }
  ghostty_terminal_scroll_viewport(impl_->terminal, scroll);
}

auto Terminal::scroll_viewport_to_bottom() noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  bool active = true;
  const auto result =
      ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &active);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  if (active) {
    return false;
  }
  scroll_viewport(ViewportScroll::bottom);
  return true;
}

auto Terminal::viewport_state() const noexcept -> std::expected<ViewportState, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  GhosttyTerminalScrollbar scrollbar{};
  bool active = true;
  const std::array keys{GHOSTTY_TERMINAL_DATA_SCROLLBAR, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE};
  std::array<void*, keys.size()> values{&scrollbar, &active};
  std::size_t written = 0;
  const auto result = ghostty_terminal_get_multi(impl_->terminal, keys.size(), keys.data(),
                                                 values.data(), &written);
  if (result != GHOSTTY_SUCCESS || written != keys.size()) {
    return std::unexpected(detail::map_error(result));
  }
  return ViewportState{
      .total_rows = scrollbar.total,
      .offset = scrollbar.offset,
      .visible_rows = scrollbar.len,
      .follows_output = active,
  };
}

auto Terminal::scroll_selection_into_view() noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  const auto selection = active_selection(impl_->terminal);
  const auto viewport = viewport_state();
  if (!selection.has_value() || !viewport.has_value()) {
    return std::unexpected(!selection.has_value() ? selection.error() : viewport.error());
  }
  if (!selection->has_value()) {
    return false;
  }
  const auto endpoint = point_from_ref(impl_->terminal, (**selection).end, PointSpace::screen);
  if (!endpoint.has_value()) {
    return std::unexpected(endpoint.error());
  }
  if (endpoint->row < viewport->offset) {
    scroll_viewport(ViewportScroll::row, static_cast<std::int64_t>(endpoint->row));
    return true;
  }
  if (viewport->visible_rows > 0 && endpoint->row >= viewport->offset + viewport->visible_rows) {
    const auto target = static_cast<std::uint64_t>(endpoint->row) - viewport->visible_rows + 1U;
    scroll_viewport(ViewportScroll::row, static_cast<std::int64_t>(target));
    return true;
  }
  return false;
}

auto Terminal::compression_activity() const noexcept -> std::expected<std::uint64_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  std::uint64_t activity = 0;
  const auto result = ghostty_terminal_compression_activity(impl_->terminal, &activity);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return activity;
}

auto Terminal::compress_scrollback() noexcept -> std::expected<CompressionResult, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  GhosttyTerminalCompressionResult compression = GHOSTTY_TERMINAL_COMPRESSION_RESULT_COMPLETE;
  const auto result = ghostty_terminal_compress(
      impl_->terminal, GHOSTTY_TERMINAL_COMPRESSION_MODE_INCREMENTAL, &compression);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  switch (compression) {
  case GHOSTTY_TERMINAL_COMPRESSION_RESULT_UNSUPPORTED:
    return CompressionResult::unsupported;
  case GHOSTTY_TERMINAL_COMPRESSION_RESULT_PENDING:
    return CompressionResult::pending;
  case GHOSTTY_TERMINAL_COMPRESSION_RESULT_COMPLETE:
    return CompressionResult::complete;
  case GHOSTTY_TERMINAL_COMPRESSION_RESULT_MAX_VALUE:
    return std::unexpected(Error::invalid_state);
  }
  return std::unexpected(Error::invalid_state);
}

// One search slice validates bounds, scans candidates, and returns explicit continuation state.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::search_literal_step(const std::string_view query, const SearchDirection direction,
                                   const std::optional<SearchCursor> start,
                                   const std::size_t work_limit,
                                   const std::optional<TerminalPoint> stop_before) const noexcept
    -> std::expected<SearchStepResult, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  if (query.empty() || query.size() > limits::search_query_bytes_max || work_limit == 0 ||
      work_limit > limits::search_candidates_per_step) {
    return std::unexpected(Error::invalid_options);
  }
  const auto rows = total_rows(impl_->terminal);
  if (!rows.has_value()) {
    return std::unexpected(rows.error());
  }
  const auto columns = impl_->options.size.columns;
  if (*rows == 0 || columns == 0) {
    return SearchStepResult{.status = SearchStepStatus::not_found};
  }

  const TerminalPoint boundary{
      .space = PointSpace::screen,
      .column = direction == SearchDirection::forward ? std::uint16_t{0}
                                                      : static_cast<std::uint16_t>(columns - 1U),
      .row = direction == SearchDirection::forward ? std::uint32_t{0}
                                                   : static_cast<std::uint32_t>(*rows - 1U),
  };
  auto cursor = start.value_or(SearchCursor{.candidate = boundary});
  const auto point_in_grid = [row_count = *rows, columns](const TerminalPoint point) noexcept {
    return point.space == PointSpace::screen && point.column < columns &&
           static_cast<std::size_t>(point.row) < row_count;
  };
  if (!point_in_grid(cursor.candidate) ||
      (stop_before.has_value() && !point_in_grid(*stop_before)) ||
      (cursor.matching &&
       (!point_in_grid(cursor.match_end) || cursor.text.space != PointSpace::screen ||
        cursor.text.column > columns || static_cast<std::size_t>(cursor.text.row) >= *rows ||
        cursor.query_offset >= query.size()))) {
    return std::unexpected(Error::invalid_options);
  }

  const auto query_byte = [query](const std::size_t offset) noexcept {
    return static_cast<std::uint8_t>(static_cast<unsigned char>(
        std::span<const char>(query.data(), query.size()).subspan(offset, 1).front()));
  };
  std::size_t work = 0;
  for (;;) {
    if (!cursor.matching && stop_before.has_value() && cursor.candidate == *stop_before) {
      return SearchStepResult{.status = SearchStepStatus::not_found};
    }
    if (!cursor.matching) {
      cursor.text = cursor.candidate;
      cursor.match_end = cursor.candidate;
      cursor.query_offset = 0;
      cursor.matching = true;
    }

    bool mismatch = false;
    while (!mismatch && static_cast<std::size_t>(cursor.text.row) < *rows) {
      if (cursor.text.column < columns) {
        if (work >= work_limit) {
          return SearchStepResult{.status = SearchStepStatus::pending, .next = cursor};
        }
        SearchTextCursor text{.point = cursor.text};
        const auto loaded = load_grapheme(impl_->terminal, text);
        ++work;
        if (!loaded.has_value()) {
          return std::unexpected(loaded.error());
        }
        if (text.grapheme_size == 0) {
          // An empty starting cell cannot begin a literal, but empty cells after matched text are
          // skipped so wrapped visual padding does not split a logical line.
          if (cursor.query_offset == 0 && cursor.text == cursor.candidate) {
            mismatch = true;
          } else {
            ++cursor.text.column;
          }
          continue;
        }
        for (const auto value : std::span(text.grapheme).first(text.grapheme_size)) {
          if (ascii_fold(value) != ascii_fold(query_byte(cursor.query_offset))) {
            mismatch = true;
            break;
          }
          cursor.match_end = cursor.text;
          ++cursor.query_offset;
          if (cursor.query_offset == query.size()) {
            return SearchStepResult{
                .status = SearchStepStatus::found,
                .match = {.start = cursor.candidate, .end = cursor.match_end},
                .next = cursor,
            };
          }
        }
        ++cursor.text.column;
        continue;
      }

      if (work >= work_limit) {
        return SearchStepResult{.status = SearchStepStatus::pending, .next = cursor};
      }
      const TerminalPoint row_point{
          .space = PointSpace::screen,
          .column = static_cast<std::uint16_t>(columns - 1U),
          .row = cursor.text.row,
      };
      const auto ref = grid_ref(impl_->terminal, row_point);
      ++work;
      if (!ref.has_value()) {
        return std::unexpected(ref.error());
      }
      GhosttyRow row = 0;
      auto result = ghostty_grid_ref_row(&*ref, &row);
      if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(detail::map_error(result));
      }
      bool wrapped = false;
      result = ghostty_row_get(row, GHOSTTY_ROW_DATA_WRAP, &wrapped);
      if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(detail::map_error(result));
      }
      ++cursor.text.row;
      cursor.text.column = 0;
      if (!wrapped) {
        if (ascii_fold(static_cast<std::uint8_t>('\n')) !=
            ascii_fold(query_byte(cursor.query_offset))) {
          mismatch = true;
        } else {
          cursor.match_end = row_point;
          ++cursor.query_offset;
          if (cursor.query_offset == query.size()) {
            return SearchStepResult{
                .status = SearchStepStatus::found,
                .match = {.start = cursor.candidate, .end = cursor.match_end},
                .next = cursor,
            };
          }
        }
      }
    }

    auto next_candidate = cursor.candidate;
    if (!advance_search_candidate(next_candidate, direction, *rows, columns)) {
      return SearchStepResult{.status = SearchStepStatus::not_found};
    }
    cursor = SearchCursor{.candidate = next_candidate};
  }
}

auto Terminal::select_search_match(const SearchMatch& match) noexcept
    -> std::expected<void, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  if (match.start.space != PointSpace::screen || match.end.space != PointSpace::screen) {
    return std::unexpected(Error::invalid_options);
  }
  const auto start = grid_ref(impl_->terminal, match.start);
  const auto end = grid_ref(impl_->terminal, match.end);
  if (!start.has_value() || !end.has_value()) {
    return std::unexpected(!start.has_value() ? start.error() : end.error());
  }
  GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
  selection.start = *start;
  selection.end = *end;
  return install_selection(impl_->terminal, selection);
}

} // namespace frame::vt
