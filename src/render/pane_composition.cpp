#include "render/pane_composition.hpp"
#include "render/graphics.hpp"
#include "render/status_line.hpp"

#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "render/grid.hpp"
#include "render/scene.hpp"
#include "render/ui.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace lemma::render {
namespace {

[[nodiscard]] auto append(std::span<std::byte> output, std::size_t& used,
                          const std::string_view text) noexcept -> bool {
  if (text.size() > output.size() - used) {
    return false;
  }
  std::memcpy(output.subspan(used).data(), text.data(), text.size());
  used += text.size();
  return true;
}

[[nodiscard]] auto append_integer(const std::span<std::byte> output, std::size_t& used,
                                  const std::uint16_t value) noexcept -> bool {
  std::array<char, 8> encoded{};
  const auto result = std::to_chars(encoded.begin(), encoded.end(), value);
  if (result.ec != std::errc{}) {
    return false;
  }
  const auto size = static_cast<std::size_t>(std::distance(encoded.begin(), result.ptr));
  return append(output, used, std::string_view(encoded.data(), size));
}

[[nodiscard]] auto append_position(const std::span<std::byte> output, std::size_t& used,
                                   const std::uint16_t row, const std::uint16_t column) noexcept
    -> bool {
  return append(output, used, "\x1B[") && append_integer(output, used, row) &&
         append(output, used, ";") && append_integer(output, used, column) &&
         append(output, used, "H");
}

[[nodiscard]] auto render_status_line(const StatusLine status, const Viewport viewport,
                                      const std::span<std::byte> output, std::size_t& used) noexcept
    -> bool {
  if (status.tabs.empty() || viewport.rows < 2) {
    return true;
  }
  std::array<ui::Cell, limits::terminal_columns_hard_max> storage{};
  const auto cells = std::span(storage).first(viewport.columns);
  std::uint16_t cursor = 0;
  return project_status_cells(status, viewport, cells, cursor) &&
         ui::paint_cells({.columns = viewport.columns, .rows = 1}, viewport.columns, 1, cells,
                         output, used);
}

void build_message_view_cells(const MessageViewLine line,
                              const std::span<ui::Cell> cells) noexcept {
  const auto text = std::span(line.text).first(std::min(line.text.size(), cells.size()));
  auto destination = cells.begin();
  for (const char character : text) {
    auto& cell = *destination;
    ++destination;
    cell.text.front() = character;
    cell.text_size = 1;
    cell.painted = true;
    cell.style.attributes = line.error ? ui::attribute_bold : 0;
  }
}

[[nodiscard]] auto message_view_line_at_row(const MessageView message_view, const std::uint16_t row,
                                            const std::size_t first_line_row) noexcept
    -> std::optional<MessageViewLine> {
  if (row < first_line_row) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(row) - first_line_row;
  return index < message_view.lines.size()
             ? std::optional{message_view.lines.subspan(index, 1U).front()}
             : std::nullopt;
}

[[nodiscard]] auto render_message_view(const MessageView message_view, const PaneRectangle content,
                                       const std::span<std::byte> output,
                                       std::size_t& used) noexcept -> bool {
  if (!message_view.active) {
    return true;
  }
  std::array<ui::Cell, limits::terminal_columns_hard_max> storage{};
  const auto cells = std::span(storage).first(content.columns);
  const auto first_line_row =
      content.rows > message_view.lines.size() ? content.rows - message_view.lines.size() : 0U;
  for (std::uint16_t row = 0; row < content.rows; ++row) {
    std::ranges::fill(cells, ui::Cell{});
    if (const auto line = message_view_line_at_row(message_view, row, first_line_row);
        line.has_value()) {
      build_message_view_cells(*line, cells);
    }
    if (!ui::paint_cells({.row = static_cast<std::uint16_t>(content.row + row),
                          .columns = content.columns,
                          .rows = 1},
                         content.columns, 1, cells, output, used)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto valid_viewport(const Viewport viewport) noexcept -> bool {
  return viewport.columns > 0 && viewport.rows > 0 &&
         viewport.columns <= limits::terminal_columns_hard_max &&
         viewport.rows <= limits::terminal_rows_hard_max;
}

void invalidate_panes(const std::span<const PaneSurface> panes) noexcept {
  for (const auto& pane : panes) {
    pane.terminal->invalidate_ansi_render_state();
  }
}

void invalidate_scene(const Scene scene) noexcept {
  invalidate_panes(scene.panes);
  for (const auto& surface : scene.grids) {
    if (surface.grid != nullptr) {
      surface.grid->invalidate_render_state();
    }
  }
}

void invalidate_pane_mode_projections(const std::span<const PaneSurface> panes) noexcept {
  for (const auto& pane : panes) {
    pane.terminal->invalidate_ansi_mode_projection();
  }
}

void invalidate_pane_cursor_projections(const std::span<const PaneSurface> panes) noexcept {
  for (const auto& pane : panes) {
    pane.terminal->invalidate_ansi_cursor_projection();
  }
}

void invalidate_focused_cursor_projection(const std::span<const PaneSurface> panes) noexcept {
  const auto focused = std::ranges::find(panes, true, &PaneSurface::focused);
  if (focused != panes.end()) {
    focused->terminal->invalidate_ansi_cursor_projection();
  }
}

[[nodiscard]] auto valid_pane(const PaneSurface& pane, const Viewport viewport) noexcept -> bool {
  if (pane.terminal == nullptr || pane.rectangle.columns == 0 || pane.rectangle.rows == 0) {
    return false;
  }
  const auto right = static_cast<std::uint32_t>(pane.rectangle.column) + pane.rectangle.columns;
  const auto bottom = static_cast<std::uint32_t>(pane.rectangle.row) + pane.rectangle.rows;
  if (right > viewport.columns || bottom > viewport.rows ||
      (pane.border_right && right >= viewport.columns) ||
      (pane.border_bottom && bottom >= viewport.rows)) {
    return false;
  }
  const auto terminal_size = pane.terminal->size();
  return terminal_size.columns == pane.rectangle.columns &&
         terminal_size.rows == pane.rectangle.rows &&
         (!pane.cursor_override ||
          (pane.focused && pane.cursor_override_column < terminal_size.columns &&
           pane.cursor_override_row < terminal_size.rows));
}

[[nodiscard]] auto rectangles_overlap(const PaneRectangle first,
                                      const PaneRectangle second) noexcept -> bool {
  const auto first_right = static_cast<std::uint32_t>(first.column) + first.columns;
  const auto first_bottom = static_cast<std::uint32_t>(first.row) + first.rows;
  const auto second_right = static_cast<std::uint32_t>(second.column) + second.columns;
  const auto second_bottom = static_cast<std::uint32_t>(second.row) + second.rows;
  return first.column < second_right && second.column < first_right && first.row < second_bottom &&
         second.row < first_bottom;
}

[[nodiscard]] auto panes_overlap(const PaneSurface& first, const PaneSurface& second) noexcept
    -> bool {
  auto first_rectangle = first.rectangle;
  auto second_rectangle = second.rectangle;
  first_rectangle.columns =
      static_cast<std::uint16_t>(first_rectangle.columns + (first.border_right ? 1U : 0U));
  first_rectangle.rows =
      static_cast<std::uint16_t>(first_rectangle.rows + (first.border_bottom ? 1U : 0U));
  second_rectangle.columns =
      static_cast<std::uint16_t>(second_rectangle.columns + (second.border_right ? 1U : 0U));
  second_rectangle.rows =
      static_cast<std::uint16_t>(second_rectangle.rows + (second.border_bottom ? 1U : 0U));
  return rectangles_overlap(first_rectangle, second_rectangle);
}

[[nodiscard]] auto valid_grid(const GridSurface& surface, const Viewport viewport) noexcept
    -> bool {
  if (surface.grid == nullptr || surface.rectangle.columns == 0 || surface.rectangle.rows == 0) {
    return false;
  }
  const auto right =
      static_cast<std::uint32_t>(surface.rectangle.column) + surface.rectangle.columns;
  const auto bottom = static_cast<std::uint32_t>(surface.rectangle.row) + surface.rectangle.rows;
  return right <= viewport.columns && bottom <= viewport.rows &&
         surface.grid->columns() == surface.rectangle.columns &&
         surface.grid->rows() == surface.rectangle.rows;
}

[[nodiscard]] auto fully_covered(const PaneRectangle rectangle,
                                 const std::span<const GridSurface> covering) noexcept -> bool {
  return std::ranges::any_of(covering, [rectangle](const GridSurface& surface) {
    const auto right =
        static_cast<std::uint32_t>(surface.rectangle.column) + surface.rectangle.columns;
    const auto bottom = static_cast<std::uint32_t>(surface.rectangle.row) + surface.rectangle.rows;
    const auto rectangle_right = static_cast<std::uint32_t>(rectangle.column) + rectangle.columns;
    const auto rectangle_bottom = static_cast<std::uint32_t>(rectangle.row) + rectangle.rows;
    return surface.opaque && surface.rectangle.column <= rectangle.column &&
           surface.rectangle.row <= rectangle.row && right >= rectangle_right &&
           bottom >= rectangle_bottom;
  });
}

[[nodiscard]] constexpr auto has_visible_status(const Viewport viewport,
                                                const StatusLine status) noexcept -> bool {
  return !status.tabs.empty() && viewport.rows >= 2;
}

[[nodiscard]] auto valid_message_view(const MessageView message_view) noexcept -> bool {
  const auto printable = [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x20U && byte <= 0x7eU;
  };
  return message_view.lines.size() <= limits::status_message_history_max &&
         (message_view.active || message_view.lines.empty()) &&
         std::ranges::all_of(message_view.lines, [&](const MessageViewLine& line) {
           return !line.text.empty() && line.text.size() <= message_view_line_bytes_max &&
                  std::ranges::all_of(line.text, printable);
         });
}

// Composition validation deliberately keeps geometry, overlap, and focus checks in one pass.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto validate_composition(const Scene scene, const Viewport viewport,
                                        const Viewport content_viewport, const StatusLine status,
                                        const MessageView message_view) noexcept
    -> std::expected<bool, CompositionError> {
  if (!valid_viewport(viewport)) {
    return std::unexpected(CompositionError::invalid_viewport);
  }
  if (scene.panes.size() > limits::panes_hard_max ||
      scene.grids.size() > limits::extension_surfaces_hard_max) {
    return std::unexpected(CompositionError::too_many_panes);
  }
  if (!valid_status(status) || !valid_message_view(message_view)) {
    return std::unexpected(CompositionError::invalid_status);
  }
  bool has_focus = false;
  bool has_presented_pane_focus = false;
  for (auto current = scene.panes.begin(); current != scene.panes.end(); ++current) {
    if (!valid_pane(*current, content_viewport)) {
      return std::unexpected(CompositionError::invalid_pane);
    }
    for (auto previous = scene.panes.begin(); previous != current; ++previous) {
      if (panes_overlap(*previous, *current)) {
        return std::unexpected(CompositionError::invalid_pane);
      }
    }
    if (current->focused && has_focus) {
      return std::unexpected(CompositionError::multiple_focused_panes);
    }
    has_focus = has_focus || current->focused;
    has_presented_pane_focus =
        has_presented_pane_focus || (current->focused && !current->presentation_suppressed &&
                                     !fully_covered(current->rectangle, scene.grids));
  }
  for (const auto& current : scene.grids) {
    if (!valid_grid(current, content_viewport) || (current.focused && has_focus)) {
      return std::unexpected(current.focused && has_focus ? CompositionError::multiple_focused_panes
                                                          : CompositionError::invalid_pane);
    }
    has_focus = has_focus || current.focused;
  }
  return has_presented_pane_focus;
}

[[nodiscard]] auto begin_frame(const std::span<std::byte> output, std::size_t& used,
                               const bool force_full) noexcept -> bool {
  return append(output, used, "\x1B[?2026h\x1B[?25l\x1B[?7l") &&
         (!force_full || append(output, used, "\x1B[2J\x1B[H"));
}

[[nodiscard]] auto is_single_full_viewport(const std::span<const PaneSurface> panes,
                                           const Viewport viewport) noexcept -> bool {
  return panes.size() == 1 && panes.front().rectangle.column == 0 &&
         panes.front().rectangle.row == 0 && panes.front().rectangle.columns == viewport.columns &&
         panes.front().rectangle.rows == viewport.rows;
}

[[nodiscard]] auto render_surface(const PaneSurface& pane, const std::span<std::byte> output,
                                  std::size_t& used, const bool force_full,
                                  const bool allow_terminal_scroll,
                                  const std::uint16_t column_offset, const std::uint16_t row_offset,
                                  CompositionResult& composition,
                                  std::optional<vt::AnsiCursorPosition>& pane_cursor) noexcept
    -> std::expected<void, CompositionError> {
  const vt::PaneRenderOptions options{
      .column = static_cast<std::uint16_t>(pane.rectangle.column + column_offset),
      .row = static_cast<std::uint16_t>(pane.rectangle.row + row_offset),
      .cursor_override_column = pane.cursor_override_column,
      .cursor_override_row = pane.cursor_override_row,
      .force_full = force_full,
      .focused = pane.focused,
      .cursor_override = pane.cursor_override,
      .allow_terminal_scroll = allow_terminal_scroll,
      .hyperlinks = pane.hyperlinks,
  };
  const auto rendered = pane.terminal->render_pane_ansi(output.subspan(used), options);
  if (!rendered.has_value()) {
    return std::unexpected(rendered.error() == vt::Error::out_of_space
                               ? CompositionError::output_exhausted
                               : CompositionError::terminal_error);
  }
  used += rendered->bytes;
  composition.rows += rendered->rows;
  composition.full = composition.full || rendered->full;
  if (pane.focused) {
    pane_cursor = rendered->cursor;
  }
  return {};
}

[[nodiscard]] auto render_panes(const Scene scene, const Viewport viewport,
                                const std::span<std::byte> output, std::size_t& used,
                                const bool force_full, const std::uint16_t column_offset,
                                const std::uint16_t row_offset, CompositionResult& composition,
                                std::optional<vt::AnsiCursorPosition>& pane_cursor) noexcept
    -> std::expected<void, CompositionError> {
  const bool allow_terminal_scroll = column_offset == 0 && row_offset == 0 && scene.grids.empty() &&
                                     is_single_full_viewport(scene.panes, viewport);
  const auto render_pass = [&](const bool focused) -> std::expected<void, CompositionError> {
    for (const auto& pane : scene.panes) {
      if (pane.focused != focused || pane.presentation_suppressed ||
          fully_covered(pane.rectangle, scene.grids)) {
        continue;
      }
      const bool repair_transparency =
          std::ranges::any_of(scene.grids, [&](const GridSurface& grid) {
            return !grid.opaque && grid.grid->damaged() &&
                   rectangles_overlap(pane.rectangle, grid.rectangle);
          });
      const auto rendered = render_surface(pane, output, used, force_full || repair_transparency,
                                           allow_terminal_scroll, column_offset, row_offset,
                                           composition, pane_cursor);
      if (!rendered.has_value()) {
        invalidate_scene(scene);
        return rendered;
      }
    }
    return {};
  };
  const auto background = render_pass(false);
  return background.has_value() ? render_pass(true) : background;
}

// Array indexes are bounded by the validated Scene span and fixed Surface maximum.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
[[nodiscard]] auto render_grids(const Scene scene, const std::span<std::byte> output,
                                std::size_t& used, const bool force_full,
                                const std::uint16_t column_offset, const std::uint16_t row_offset,
                                const std::size_t pane_rows_rendered,
                                CompositionResult& composition) noexcept
    -> std::expected<void, CompositionError> {
  std::array<bool, limits::extension_surfaces_hard_max> rendered_lower{};
  for (std::size_t index = 0; index < scene.grids.size(); ++index) {
    const auto& surface = scene.grids.subspan(index, 1).front();
    if (fully_covered(surface.rectangle, scene.grids.subspan(index + 1U))) {
      continue;
    }
    bool repair = force_full;
    if (!repair) {
      repair =
          std::ranges::any_of(scene.grids.subspan(index + 1U), [&](const GridSurface& covering) {
            return !covering.opaque && covering.grid->damaged() &&
                   rectangles_overlap(surface.rectangle, covering.rectangle);
          });
    }
    if (!repair && pane_rows_rendered > 0) {
      repair = std::ranges::any_of(scene.panes, [&](const PaneSurface& pane) {
        return rectangles_overlap(pane.rectangle, surface.rectangle);
      });
    }
    if (!repair) {
      for (std::size_t lower = 0; lower < index; ++lower) {
        if (rendered_lower.at(lower) &&
            rectangles_overlap(scene.grids.subspan(lower, 1).front().rectangle,
                               surface.rectangle)) {
          repair = true;
          break;
        }
      }
    }
    const PaneRectangle physical{
        .column = static_cast<std::uint16_t>(surface.rectangle.column + column_offset),
        .row = static_cast<std::uint16_t>(surface.rectangle.row + row_offset),
        .columns = surface.rectangle.columns,
        .rows = surface.rectangle.rows,
    };
    const auto rendered = surface.grid->render_ansi(
        output.subspan(used),
        {.rectangle = physical, .force_full = repair, .opaque = surface.opaque});
    if (!rendered.has_value()) {
      invalidate_scene(scene);
      return std::unexpected(rendered.error() == GridError::output_exhausted
                                 ? CompositionError::output_exhausted
                                 : CompositionError::invalid_pane);
    }
    used += rendered->bytes;
    composition.rows += rendered->rows;
    rendered_lower.at(index) = rendered->rows > 0;
  }
  return {};
}

[[nodiscard]] auto cursor_covered(const PaneRectangle point,
                                  const std::span<const GridSurface> higher) noexcept -> bool {
  return std::ranges::any_of(higher, [point](const GridSurface& surface) {
    return rectangles_overlap(point, surface.rectangle) &&
           (surface.opaque ||
            surface.grid->paints_cell(
                static_cast<std::uint16_t>(point.column - surface.rectangle.column),
                static_cast<std::uint16_t>(point.row - surface.rectangle.row)));
  });
}

[[nodiscard]] auto
project_scene_cursor(const Scene scene, const std::span<std::byte> output, std::size_t& used,
                     const std::uint16_t column_offset, const std::uint16_t row_offset,
                     const bool grids_rendered,
                     const std::optional<vt::AnsiCursorPosition> pane_cursor) noexcept
    -> std::expected<void, CompositionError> {
  const auto focused_grid = std::ranges::find(scene.grids, true, &GridSurface::focused);
  if (focused_grid != scene.grids.end()) {
    const PaneRectangle physical{
        .column = static_cast<std::uint16_t>(focused_grid->rectangle.column + column_offset),
        .row = static_cast<std::uint16_t>(focused_grid->rectangle.row + row_offset),
        .columns = focused_grid->rectangle.columns,
        .rows = focused_grid->rectangle.rows,
    };
    const auto cursor = focused_grid->grid->cursor();
    const PaneRectangle point{
        .column = static_cast<std::uint16_t>(focused_grid->rectangle.column + cursor.column),
        .row = static_cast<std::uint16_t>(focused_grid->rectangle.row + cursor.row),
        .columns = 1,
        .rows = 1};
    const auto higher =
        scene.grids.subspan(static_cast<std::size_t>(focused_grid - scene.grids.begin()) + 1U);
    const auto rendered = focused_grid->grid->render_cursor_ansi(output.subspan(used), physical,
                                                                 !cursor_covered(point, higher));
    if (!rendered.has_value()) {
      invalidate_scene(scene);
      return std::unexpected(rendered.error() == GridError::output_exhausted
                                 ? CompositionError::output_exhausted
                                 : CompositionError::invalid_pane);
    }
    used += rendered->bytes;
    // The focused Surface projects Lemma's steady block while every Pane is unfocused. Returning
    // focus to a Pane must restore its canonical shape even when the terminal has no damage.
    invalidate_pane_cursor_projections(scene.panes);
    return {};
  }
  if (scene.grids.empty() || !pane_cursor.has_value()) {
    return {};
  }
  // Coverage is retained Scene state, not frame damage. The Pane has already projected its
  // actual cursor (including copy-mode overrides) and its color/blink policy. Grids only move
  // the output position; do not render the terminal again merely to restore it.
  const PaneRectangle point{.column =
                                static_cast<std::uint16_t>(pane_cursor->column - column_offset),
                            .row = static_cast<std::uint16_t>(pane_cursor->row - row_offset),
                            .columns = 1,
                            .rows = 1};
  const bool covered = cursor_covered(point, scene.grids);
  if ((covered && !append(output, used, "\x1B[?25l")) ||
      (!covered && grids_rendered &&
       !append_position(output, used, static_cast<std::uint16_t>(pane_cursor->row + 1U),
                        static_cast<std::uint16_t>(pane_cursor->column + 1U)))) {
    invalidate_scene(scene);
    return std::unexpected(CompositionError::output_exhausted);
  }
  return {};
}

[[nodiscard]] auto border_cell(const std::span<const PaneSurface> panes, const std::uint16_t row,
                               const std::uint16_t column) noexcept -> bool {
  return std::ranges::any_of(panes, [row, column](const PaneSurface& pane) {
    const auto right = static_cast<std::uint16_t>(pane.rectangle.column + pane.rectangle.columns);
    const auto bottom = static_cast<std::uint16_t>(pane.rectangle.row + pane.rectangle.rows);
    const bool on_right =
        pane.border_right && column == right && row >= pane.rectangle.row && row < bottom;
    const bool on_bottom =
        pane.border_bottom && row == bottom && column >= pane.rectangle.column && column < right;
    return on_right || on_bottom;
  });
}

// The branches map the four neighboring separator segments to one box-drawing junction.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto border_glyph(const std::span<const PaneSurface> panes, const std::uint16_t row,
                                const std::uint16_t column) noexcept -> std::string_view {
  const bool left = column > 0 && border_cell(panes, row, static_cast<std::uint16_t>(column - 1U));
  const bool right = border_cell(panes, row, static_cast<std::uint16_t>(column + 1U));
  const bool up = row > 0 && border_cell(panes, static_cast<std::uint16_t>(row - 1U), column);
  const bool down = border_cell(panes, static_cast<std::uint16_t>(row + 1U), column);
  if (left && right && up && down) {
    return "┼";
  }
  if (left && right && down) {
    return "┬";
  }
  if (left && right && up) {
    return "┴";
  }
  if (up && down && right) {
    return "├";
  }
  if (up && down && left) {
    return "┤";
  }
  if (right && down) {
    return "┌";
  }
  if (left && down) {
    return "┐";
  }
  if (right && up) {
    return "└";
  }
  if (left && up) {
    return "┘";
  }
  return left || right ? std::string_view{"─"} : std::string_view{"│"};
}

[[nodiscard]] auto draw_right_border(const PaneSurface& pane,
                                     const std::span<const PaneSurface> panes,
                                     const std::span<std::byte> output, std::size_t& used,
                                     const std::uint16_t column_offset,
                                     const std::uint16_t row_offset) noexcept -> bool {
  if (!pane.border_right) {
    return true;
  }
  const auto column = static_cast<std::uint16_t>(pane.rectangle.column + pane.rectangle.columns);
  const auto bottom = static_cast<std::uint16_t>(pane.rectangle.row + pane.rectangle.rows);
  for (std::uint16_t row = pane.rectangle.row; row < bottom; ++row) {
    if (!append_position(output, used, static_cast<std::uint16_t>(row + row_offset + 1U),
                         static_cast<std::uint16_t>(column + column_offset + 1U)) ||
        !append(output, used, border_glyph(panes, row, column))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto draw_bottom_border(const PaneSurface& pane,
                                      const std::span<const PaneSurface> panes,
                                      const std::span<std::byte> output, std::size_t& used,
                                      const std::uint16_t column_offset,
                                      const std::uint16_t row_offset) noexcept -> bool {
  if (!pane.border_bottom) {
    return true;
  }
  const auto row = static_cast<std::uint16_t>(pane.rectangle.row + pane.rectangle.rows);
  const auto right = static_cast<std::uint16_t>(pane.rectangle.column + pane.rectangle.columns);
  for (std::uint16_t column = pane.rectangle.column; column < right; ++column) {
    if (!append_position(output, used, static_cast<std::uint16_t>(row + row_offset + 1U),
                         static_cast<std::uint16_t>(column + column_offset + 1U)) ||
        !append(output, used, border_glyph(panes, row, column))) {
      return false;
    }
  }
  return true;
}

// Junction candidates are bounded by the visible pane count squared.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto draw_junctions(const std::span<const PaneSurface> panes,
                                  const std::span<std::byte> output, std::size_t& used,
                                  const std::uint16_t column_offset,
                                  const std::uint16_t row_offset) noexcept -> bool {
  for (const auto& vertical : panes) {
    if (vertical.border_right) {
      const auto column =
          static_cast<std::uint16_t>(vertical.rectangle.column + vertical.rectangle.columns);
      for (const auto& horizontal : panes) {
        if (horizontal.border_bottom) {
          const auto row =
              static_cast<std::uint16_t>(horizontal.rectangle.row + horizontal.rectangle.rows);
          const bool horizontal_neighbor =
              (column > 0 && border_cell(panes, row, static_cast<std::uint16_t>(column - 1U))) ||
              border_cell(panes, row, static_cast<std::uint16_t>(column + 1U));
          const bool vertical_neighbor =
              (row > 0 && border_cell(panes, static_cast<std::uint16_t>(row - 1U), column)) ||
              border_cell(panes, static_cast<std::uint16_t>(row + 1U), column);
          if (horizontal_neighbor && vertical_neighbor &&
              (!append_position(output, used, static_cast<std::uint16_t>(row + row_offset + 1U),
                                static_cast<std::uint16_t>(column + column_offset + 1U)) ||
               !append(output, used, border_glyph(panes, row, column)))) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

[[nodiscard]] auto draw_borders(const std::span<const PaneSurface> panes,
                                const std::span<std::byte> output, std::size_t& used,
                                const std::uint16_t column_offset,
                                const std::uint16_t row_offset) noexcept -> bool {
  if (!append(output, used, "\x1B[90m")) {
    return false;
  }
  for (const auto& pane : panes) {
    if (!draw_right_border(pane, panes, output, used, column_offset, row_offset) ||
        !draw_bottom_border(pane, panes, output, used, column_offset, row_offset)) {
      return false;
    }
  }
  return draw_junctions(panes, output, used, column_offset, row_offset) &&
         append(output, used, "\x1B[0m");
}

struct CompositionPolicy final {
  OuterModeProjection outer_modes{OuterModeProjection::neutral};
};

[[nodiscard]] auto composition_policy(const Scene scene, const Viewport viewport,
                                      const Viewport content_viewport, const StatusLine status,
                                      const MessageView message_view) noexcept
    -> std::expected<CompositionPolicy, CompositionError> {
  const auto validation =
      validate_composition(scene, viewport, content_viewport, status, message_view);
  if (!validation.has_value()) {
    return std::unexpected(validation.error());
  }
  if (message_view.active) {
    return CompositionPolicy{};
  }
  if (std::ranges::any_of(scene.grids, &GridSurface::focused)) {
    return CompositionPolicy{.outer_modes = OuterModeProjection::button_mouse};
  }
  if (!*validation) {
    return CompositionPolicy{};
  }
  const auto focused = std::ranges::find(scene.panes, true, &PaneSurface::focused);
  if (focused == scene.panes.end()) {
    return CompositionPolicy{};
  }
  const auto tracking = focused->terminal->mouse_tracking();
  if (!tracking.has_value()) {
    return std::unexpected(CompositionError::terminal_error);
  }
  return CompositionPolicy{
      .outer_modes = tracking->unbuttoned_motion ? OuterModeProjection::any_mouse
                                                 : OuterModeProjection::button_mouse,
  };
}

[[nodiscard]] auto render_status_prompt_cursor(const StatusLine status, const Viewport viewport,
                                               const std::span<std::byte> output,
                                               std::size_t& used) noexcept -> bool {
  if (!status.prompting() || viewport.rows < 2) {
    return true;
  }
  const auto cursor_column = status_cursor_column(status, viewport);
  // A steady block marks the insertion point without styling the cursor cell.
  return append_position(output, used, 1, cursor_column) &&
         append(output, used, "\x1B[2 q\x1B[?25h");
}

[[nodiscard]] auto finish_frame(const std::span<std::byte> output, std::size_t& used,
                                const OuterModeProjection outer_modes,
                                const bool project_outer_modes) noexcept -> bool {
  constexpr std::string_view neutral_modes =
      "\x1B[?1l\x1B[?9l\x1B[?1000l\x1B[?1002l\x1B[?1003l\x1B[?1005l"
      "\x1B[?1006l\x1B[?1007l\x1B[?1015l\x1B[?1016l\x1B[?2004l";
  // Mouse event and encoding modes are each mutually exclusive. Disable competing modes before
  // enabling the desired one: a later reset would otherwise replace the active mode with `none`
  // or the legacy X10 encoding in terminals such as Ghostty.
  constexpr std::string_view button_mouse_capture =
      "\x1B[?9l\x1B[?1000l\x1B[?1003l\x1B[?1002h\x1B[?1005l\x1B[?1015l"
      "\x1B[?1016l\x1B[?1006h";
  constexpr std::string_view any_mouse_capture =
      "\x1B[?9l\x1B[?1000l\x1B[?1002l\x1B[?1003h\x1B[?1005l\x1B[?1015l"
      "\x1B[?1016l\x1B[?1006h";
  const auto projected = [=, &output, &used]() noexcept {
    switch (outer_modes) {
    case OuterModeProjection::neutral:
      return append(output, used, neutral_modes) && append(output, used, button_mouse_capture);
    case OuterModeProjection::button_mouse:
      return append(output, used, button_mouse_capture);
    case OuterModeProjection::any_mouse:
      return append(output, used, any_mouse_capture);
    }
    return false;
  };
  return append(output, used, "\x1B[0m\x1B[?7h") &&
         (outer_modes != OuterModeProjection::neutral || append(output, used, "\x1B[?25l")) &&
         (!project_outer_modes || projected()) && append(output, used, "\x1B[?2026l");
}

[[nodiscard]] auto finish_composition(const Scene scene, const StatusLine status,
                                      const Viewport viewport, const std::span<std::byte> output,
                                      std::size_t used, const bool force_full,
                                      const bool complete_frame,
                                      const std::optional<OuterModeProjection> previous_outer_modes,
                                      CompositionResult composition) noexcept
    -> std::expected<CompositionResult, CompositionError> {
  if (status.prompting() && !render_status_prompt_cursor(status, viewport, output, used)) {
    invalidate_scene(scene);
    return std::unexpected(CompositionError::output_exhausted);
  }
  // A pane-level repair does not make the protocol frame complete when suppression omitted a
  // surface. In that case no full-screen clear was emitted and the full-redraw generation must not
  // advance.
  composition.full = composition.full && complete_frame;
  const bool project_outer_modes = force_full || previous_outer_modes != composition.outer_modes;
  if (!finish_frame(output, used, composition.outer_modes, project_outer_modes)) {
    invalidate_scene(scene);
    return std::unexpected(CompositionError::output_exhausted);
  }
  // Neutral projection resets child-owned non-mouse modes after pane rendering. Invalidate those
  // physical shadows so a normally released synchronized pane restores its canonical modes.
  if (project_outer_modes && composition.outer_modes == OuterModeProjection::neutral) {
    invalidate_pane_mode_projections(scene.panes);
  }
  // The status editor projects a steady block after pane rendering. The next frame must restore
  // the child's canonical blink mode even when no terminal damage occurred.
  if (status.prompting() && viewport.rows >= 2) {
    invalidate_focused_cursor_projection(scene.panes);
  }
  composition.bytes = used;
  return composition;
}

} // namespace

// Validation is a separate pass so malformed composition input cannot partially consume terminal
// damage or alter retained pane state. The bounded branches preserve all-or-nothing composition.
[[nodiscard]] auto
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
compose_scene(const Scene scene, const Viewport viewport, const std::span<std::byte> output,
              const bool force_full, const StatusLine status,
              const std::optional<OuterModeProjection> previous_outer_modes,
              const MessageView message_view, GraphicsProjection* const graphics) noexcept
    -> std::expected<CompositionResult, CompositionError> {
  const auto status_rows =
      has_visible_status(viewport, status) ? std::uint16_t{1} : std::uint16_t{0};
  const PaneRectangle content{
      .row = status_rows,
      .columns = viewport.columns,
      .rows = static_cast<std::uint16_t>(viewport.rows - status_rows),
  };
  const Viewport content_viewport{.columns = content.columns, .rows = content.rows};
  const auto policy = composition_policy(scene, viewport, content_viewport, status, message_view);
  if (!policy.has_value()) {
    return std::unexpected(policy.error());
  }

  const bool complete_frame =
      message_view.active || std::ranges::all_of(scene.panes, [&](const PaneSurface& pane) {
        return !pane.presentation_suppressed || fully_covered(pane.rectangle, scene.grids);
      });
  const bool complete_full = force_full && complete_frame;
  std::size_t used = 0;
  if (!begin_frame(output, used, complete_full)) {
    return std::unexpected(CompositionError::output_exhausted);
  }
  CompositionResult composition{
      .panes = message_view.active ? 0U : scene.panes.size(),
      .outer_modes = status.prompting() ? OuterModeProjection::button_mouse : policy->outer_modes,
      .full = complete_full,
  };
  // Tiled separators remain Pane-owned Scene decoration and change only with layout/full redraw.
  if (force_full && !message_view.active &&
      !draw_borders(scene.panes, output, used, content.column, content.row)) {
    invalidate_scene(scene);
    return std::unexpected(CompositionError::output_exhausted);
  }
  if (has_visible_status(viewport, status) && (force_full || status.dirty) &&
      !render_status_line(status, viewport, output, used)) {
    invalidate_scene(scene);
    return std::unexpected(CompositionError::output_exhausted);
  }
  composition.status = has_visible_status(viewport, status) && (force_full || status.dirty);
  if (message_view.active) {
    if (!render_message_view(message_view, content, output, used)) {
      invalidate_scene(scene);
      return std::unexpected(CompositionError::output_exhausted);
    }
    // Native message recovery replaces Pane contents; retained extension Surfaces keep their
    // normal z-order and must be restored over the newly painted message background.
    const auto grids = render_grids(scene, output, used, true, content.column, content.row,
                                    content.rows, composition);
    if (!grids.has_value()) {
      return std::unexpected(grids.error());
    }
  } else {
    std::optional<vt::AnsiCursorPosition> pane_cursor;
    const auto rendered = render_panes(scene, content_viewport, output, used, force_full,
                                       content.column, content.row, composition, pane_cursor);
    if (!rendered.has_value()) {
      return std::unexpected(rendered.error());
    }
    const auto pane_rows_rendered = composition.rows;
    const auto before_grids = used;
    const auto grids = render_grids(scene, output, used, force_full, content.column, content.row,
                                    pane_rows_rendered, composition);
    if (!grids.has_value()) {
      return std::unexpected(grids.error());
    }
    const auto cursor = project_scene_cursor(scene, output, used, content.column, content.row,
                                             used != before_grids, pane_cursor);
    if (!cursor.has_value()) {
      return std::unexpected(cursor.error());
    }
  }
  if (graphics != nullptr) {
    constexpr std::size_t finish_reserve = 512;
    const auto available = output.size() - used;
    const auto encoded = graphics->append(
        message_view.active ? Scene{} : scene, status_rows,
        output.subspan(used, available > finish_reserve ? available - finish_reserve : 0),
        complete_full, composition.rows > 0);
    if (!encoded) {
      invalidate_scene(scene);
      return std::unexpected(CompositionError::terminal_error);
    }
    used += *encoded;
  }
  return finish_composition(scene, status, viewport, output, used, force_full, complete_frame,
                            previous_outer_modes, composition);
}

auto compose_frame(const std::span<const PaneSurface> panes, const Viewport viewport,
                   const std::span<std::byte> output, const bool force_full,
                   const StatusLine status,
                   const std::optional<OuterModeProjection> previous_outer_modes,
                   const MessageView message_view) noexcept
    -> std::expected<CompositionResult, CompositionError> {
  return compose_scene(Scene{.panes = panes, .grids = {}}, viewport, output, force_full, status,
                       previous_outer_modes, message_view);
}

} // namespace lemma::render
