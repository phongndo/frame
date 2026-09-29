#include "render/grid.hpp"

#include "frame/geometry.hpp"
#include "frame/limits.hpp"
#include "frame/terminal/terminal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace frame::render {

// Dimension and retained-byte checks make vector sizes finite; allocating operations catch locally.
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

[[nodiscard]] constexpr auto next_generation(const std::uint64_t generation) noexcept
    -> std::uint64_t {
  return generation == std::numeric_limits<std::uint64_t>::max() ? 1U : generation + 1U;
}

[[nodiscard]] auto append(const std::span<std::byte> output, std::size_t& used,
                          const std::string_view text) noexcept -> bool {
  if (text.size() > output.size() - used) {
    return false;
  }
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    output.subspan(used, text.size()).begin());
  used += text.size();
  return true;
}

[[nodiscard]] auto append_number(const std::span<std::byte> output, std::size_t& used,
                                 const std::uint64_t value) noexcept -> bool {
  std::array<char, 32> encoded{};
  const auto result = std::to_chars(encoded.begin(), encoded.end(), value);
  return result.ec == std::errc{} &&
         append(output, used,
                std::string_view(encoded.data(),
                                 static_cast<std::size_t>(result.ptr - encoded.data())));
}

[[nodiscard]] auto append_position(const std::span<std::byte> output, std::size_t& used,
                                   const std::uint16_t row, const std::uint16_t column) noexcept
    -> bool {
  return append(output, used, "\x1b[") && append_number(output, used, row) &&
         append(output, used, ";") && append_number(output, used, column) &&
         append(output, used, "H");
}

[[nodiscard]] auto append_color(const std::span<std::byte> output, std::size_t& used,
                                const std::string_view prefix, const GridColor color) noexcept
    -> bool {
  return append(output, used, prefix) && append_number(output, used, color.red) &&
         append(output, used, ";") && append_number(output, used, color.green) &&
         append(output, used, ";") && append_number(output, used, color.blue);
}

[[nodiscard]] auto append_style(const std::span<std::byte> output, std::size_t& used,
                                const GridStyle& style) noexcept -> bool {
  if (!append(output, used, "\x1b[0")) {
    return false;
  }
  if (style.bold && !append(output, used, ";1")) {
    return false;
  }
  if (style.faint && !append(output, used, ";2")) {
    return false;
  }
  if (style.italic && !append(output, used, ";3")) {
    return false;
  }
  if (style.underline && !append(output, used, ";4")) {
    return false;
  }
  if (style.inverse && !append(output, used, ";7")) {
    return false;
  }
  if (style.foreground.has_value() && !append_color(output, used, ";38;2;", *style.foreground)) {
    return false;
  }
  if (style.background.has_value() && !append_color(output, used, ";48;2;", *style.background)) {
    return false;
  }
  return append(output, used, "m");
}

[[nodiscard]] auto same_run(const GridRun& first, const GridRun& second) noexcept -> bool {
  return first.column == second.column && first.columns == second.columns &&
         first.style == second.style && first.text == second.text;
}

[[nodiscard]] auto same_runs(const std::span<const GridRun> first,
                             const std::span<const GridRun> second) noexcept -> bool {
  return first.size() == second.size() && std::ranges::equal(first, second, &same_run);
}

[[nodiscard]] auto row_bytes(const std::span<const GridRun> runs) noexcept -> std::size_t {
  std::size_t result = runs.size() * sizeof(GridRun);
  for (const auto& run : runs) {
    if (run.text.size() > std::numeric_limits<std::size_t>::max() - result) {
      return std::numeric_limits<std::size_t>::max();
    }
    result += run.text.size();
  }
  return result;
}

} // namespace

Grid::Grid(const std::uint16_t columns, const std::uint16_t rows, std::vector<Row> storage,
           std::vector<GridStyle> styles) noexcept
    : rows_(std::move(storage)), styles_(std::move(styles)),
      retained_bytes_((rows_.size() * sizeof(Row)) + (styles_.size() * sizeof(GridStyle))),
      columns_(columns), rows_count_(rows) {}

auto Grid::row_runs(const std::uint16_t row) const noexcept -> std::span<const GridRun> {
  return row < rows_.size() ? std::span<const GridRun>(rows_.at(row).runs)
                            : std::span<const GridRun>{};
}

auto Grid::create(const std::uint16_t columns, const std::uint16_t rows) noexcept
    -> std::expected<Grid, GridError> {
  if (columns == 0 || rows == 0 || columns > limits::terminal_columns_hard_max ||
      rows > limits::terminal_rows_hard_max) {
    return std::unexpected(GridError::invalid_dimensions);
  }
  try {
    std::vector<Row> storage(rows);
    std::vector<GridStyle> styles(1);
    Grid grid(columns, rows, std::move(storage), std::move(styles));
    if (grid.retained_bytes_ > limits::surface_retained_bytes_max) {
      return std::unexpected(GridError::resource_limit);
    }
    return grid;
  } catch (const std::bad_alloc&) {
    return std::unexpected(GridError::out_of_memory);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Grid::apply(GridPatch patch, const std::size_t retained_bytes_max) noexcept
    -> std::expected<GridApplyResult, GridError> {
  try {
    std::optional<std::vector<GridStyle>> proposed_styles;
    if (patch.styles.has_value()) {
      if (patch.styles->empty() || patch.styles->size() > limits::surface_styles_max) {
        return std::unexpected(GridError::invalid_style);
      }
      proposed_styles = std::move(*patch.styles);
    }
    const auto style_table = proposed_styles.has_value()
                                 ? std::span<const GridStyle>(*proposed_styles)
                                 : std::span<const GridStyle>(styles_);
    const bool styles_changed = proposed_styles.has_value() && *proposed_styles != styles_;
    if (patch.cursor.has_value() &&
        (patch.cursor->column >= columns_ || patch.cursor->row >= rows_count_)) {
      return std::unexpected(GridError::invalid_cursor);
    }

    const auto previous_style_bytes = styles_.size() * sizeof(GridStyle);
    const auto proposed_style_bytes = style_table.size() * sizeof(GridStyle);
    if (previous_style_bytes > retained_bytes_ ||
        proposed_style_bytes > retained_bytes_max - std::min(retained_bytes_ - previous_style_bytes,
                                                             retained_bytes_max)) {
      return std::unexpected(GridError::resource_limit);
    }
    std::size_t retained = retained_bytes_ - previous_style_bytes + proposed_style_bytes;
    for (std::size_t index = 0; index < patch.rows.size(); ++index) {
      auto& row_patch = patch.rows.at(index);
      if (row_patch.row >= rows_count_ || std::ranges::any_of(std::span(patch.rows).first(index),
                                                              [&](const GridRowPatch& previous) {
                                                                return previous.row ==
                                                                       row_patch.row;
                                                              })) {
        return std::unexpected(GridError::invalid_row);
      }
      if (row_patch.runs.size() > limits::surface_runs_per_row_max) {
        return std::unexpected(GridError::resource_limit);
      }
      std::size_t text_bytes = 0;
      std::uint32_t previous_end = 0;
      for (auto& run : row_patch.runs) {
        // Kitty placeholders are terminal protocol, not ordinary Surface text.
        if (run.text.empty() || run.text.contains("\xF4\x8E\xBB\xAE") ||
            run.style >= style_table.size() || run.column >= columns_ ||
            run.text.size() > limits::surface_text_bytes_per_row_max -
                                  std::min(text_bytes, limits::surface_text_bytes_per_row_max)) {
          return std::unexpected(run.style >= style_table.size() ? GridError::invalid_style
                                                                 : GridError::invalid_run);
        }
        const auto metrics = vt::measure_grid_text(run.text);
        if (!metrics.has_value() || metrics->columns == 0 ||
            metrics->columns > std::numeric_limits<std::uint16_t>::max()) {
          return std::unexpected(GridError::invalid_run);
        }
        run.columns = static_cast<std::uint16_t>(metrics->columns);
        const auto end = static_cast<std::uint32_t>(run.column) + run.columns;
        if (run.column < previous_end || end > columns_) {
          return std::unexpected(GridError::invalid_run);
        }
        previous_end = end;
        text_bytes += run.text.size();
      }
      const auto old_bytes = row_bytes(rows_.at(row_patch.row).runs);
      const auto new_bytes = row_bytes(row_patch.runs);
      if (old_bytes > retained || new_bytes > retained_bytes_max - (retained - old_bytes)) {
        return std::unexpected(GridError::resource_limit);
      }
      retained = retained - old_bytes + new_bytes;
    }

    if (styles_changed && std::ranges::any_of(rows_, [&](const Row& row) {
          const auto row_index = static_cast<std::uint16_t>(&row - rows_.data());
          const bool replaced = std::ranges::any_of(
              patch.rows, [row_index](const GridRowPatch& item) { return item.row == row_index; });
          return !replaced && std::ranges::any_of(row.runs, [&](const GridRun& run) {
            return run.style >= style_table.size();
          });
        })) {
      return std::unexpected(GridError::invalid_style);
    }
    const auto changed_rows =
        static_cast<std::size_t>(std::ranges::count_if(patch.rows, [&](const GridRowPatch& row) {
          return !same_runs(rows_.at(row.row).runs, row.runs);
        }));
    const bool cursor_changed = patch.cursor.has_value() && *patch.cursor != cursor_;
    if (changed_rows == 0 && !styles_changed && !cursor_changed) {
      return GridApplyResult{.retained_bytes = retained_bytes_, .generation = generation_};
    }

    generation_ = next_generation(generation_);
    if (styles_changed) {
      styles_ = std::move(*proposed_styles);
      for (auto& row : rows_) {
        row.generation = generation_;
      }
    }
    for (auto& row_patch : patch.rows) {
      auto& current = rows_.at(row_patch.row);
      if (same_runs(current.runs, row_patch.runs)) {
        continue;
      }
      const auto presented = current.presented_generation;
      current = {.runs = std::move(row_patch.runs),
                 .generation = generation_,
                 .presented_generation = presented};
    }
    if (cursor_changed) {
      cursor_ = *patch.cursor;
      cursor_generation_ = generation_;
    }
    retained_bytes_ = retained;
    return GridApplyResult{.retained_bytes = retained_bytes_,
                           .changed_rows = changed_rows,
                           .generation = generation_,
                           .cursor_changed = cursor_changed,
                           .styles_changed = styles_changed};
  } catch (const std::bad_alloc&) {
    return std::unexpected(GridError::out_of_memory);
  }
}

auto Grid::resized(const std::uint16_t columns, const std::uint16_t rows) const noexcept
    -> std::expected<Grid, GridError> {
  if (columns == 0 || rows == 0 || columns > limits::terminal_columns_hard_max ||
      rows > limits::terminal_rows_hard_max) {
    return std::unexpected(GridError::invalid_dimensions);
  }
  try {
    std::vector<Row> storage(rows);
    const auto retained_rows = std::min<std::size_t>(rows, rows_.size());
    for (std::size_t index = 0; index < retained_rows; ++index) {
      auto& destination = storage.at(index);
      for (const auto& run : rows_.at(index).runs) {
        const auto end = static_cast<std::uint32_t>(run.column) + run.columns;
        if (end <= columns) {
          destination.runs.push_back(run);
        }
      }
    }
    auto styles = styles_;
    Grid result(columns, rows, std::move(storage), std::move(styles));
    for (const auto& row : result.rows_) {
      const auto bytes = row_bytes(row.runs);
      if (bytes > limits::surface_retained_bytes_max -
                      std::min(result.retained_bytes_, limits::surface_retained_bytes_max)) {
        return std::unexpected(GridError::resource_limit);
      }
      result.retained_bytes_ += bytes;
    }
    result.generation_ = next_generation(generation_);
    for (auto& row : result.rows_) {
      row.generation = result.generation_;
      row.presented_generation = 0;
    }
    result.cursor_ = {.column = std::min<std::uint16_t>(cursor_.column, columns - 1U),
                      .row = std::min<std::uint16_t>(cursor_.row, rows - 1U),
                      .visible = cursor_.visible && cursor_.column < columns && cursor_.row < rows};
    result.cursor_generation_ = result.generation_;
    result.presented_cursor_generation_ = 0;
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(GridError::out_of_memory);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Grid::render_ansi(const std::span<std::byte> output, const GridRenderOptions options) noexcept
    -> std::expected<GridRenderResult, GridError> {
  if (options.rectangle.columns != columns_ || options.rectangle.rows != rows_count_) {
    return std::unexpected(GridError::invalid_dimensions);
  }
  std::size_t used = 0;
  std::size_t rendered_rows = 0;
  for (std::size_t index = 0; index < rows_.size(); ++index) {
    const auto& row = rows_.at(index);
    if (!options.force_full && row.generation == row.presented_generation) {
      continue;
    }
    const auto physical_row = static_cast<std::uint16_t>(options.rectangle.row + index + 1U);
    if (options.opaque &&
        (!append_position(output, used, physical_row,
                          static_cast<std::uint16_t>(options.rectangle.column + 1U)) ||
         !append(output, used, "\x1b[0m\x1b[") || !append_number(output, used, columns_) ||
         !append(output, used, "X"))) {
      return std::unexpected(GridError::output_exhausted);
    }
    for (const auto& run : row.runs) {
      if (!append_position(
              output, used, physical_row,
              static_cast<std::uint16_t>(options.rectangle.column + run.column + 1U)) ||
          !append_style(output, used, styles_.at(run.style)) || !append(output, used, run.text)) {
        return std::unexpected(GridError::output_exhausted);
      }
    }
    ++rendered_rows;
  }

  const bool cursor_changed = cursor_generation_ != presented_cursor_generation_;
  if (options.focused &&
      (options.force_full || options.project_cursor || cursor_changed || rendered_rows > 0)) {
    const auto projected = render_cursor_ansi(output.subspan(used), options.rectangle);
    if (!projected.has_value()) {
      return std::unexpected(projected.error());
    }
    used += projected->bytes;
  }

  for (auto& row : rows_) {
    if (options.force_full || row.generation != row.presented_generation) {
      row.presented_generation = row.generation;
    }
  }
  presented_cursor_generation_ = cursor_generation_;
  return GridRenderResult{.bytes = used,
                          .rows = rendered_rows,
                          .cursor_visible = options.focused && cursor_.visible,
                          .full = options.force_full};
}

auto Grid::render_cursor_ansi(const std::span<std::byte> output, const PaneRectangle rectangle,
                              const bool unobscured) noexcept
    -> std::expected<GridRenderResult, GridError> {
  if (rectangle.columns != columns_ || rectangle.rows != rows_count_) {
    return std::unexpected(GridError::invalid_dimensions);
  }
  std::size_t used = 0;
  const bool visible = cursor_.visible && unobscured;
  if (visible) {
    if (!append_position(output, used, static_cast<std::uint16_t>(rectangle.row + cursor_.row + 1U),
                         static_cast<std::uint16_t>(rectangle.column + cursor_.column + 1U)) ||
        !append(output, used, "\x1b[0m\x1b[2 q\x1b[?25h")) {
      return std::unexpected(GridError::output_exhausted);
    }
  } else if (!append(output, used, "\x1b[?25l")) {
    return std::unexpected(GridError::output_exhausted);
  }
  presented_cursor_generation_ = cursor_generation_;
  return GridRenderResult{.bytes = used, .rows = 0, .cursor_visible = visible, .full = false};
}

auto Grid::paints_cell(const std::uint16_t column, const std::uint16_t row) const noexcept -> bool {
  if (column >= columns_ || row >= rows_count_) {
    return false;
  }
  return std::ranges::any_of(rows_.at(row).runs, [column](const GridRun& run) {
    return column >= run.column && column < static_cast<std::uint32_t>(run.column) + run.columns;
  });
}

void Grid::invalidate_render_state() noexcept {
  for (auto& row : rows_) {
    row.presented_generation = 0;
  }
  presented_cursor_generation_ = 0;
}

auto Grid::damaged() const noexcept -> bool {
  return cursor_generation_ != presented_cursor_generation_ ||
         std::ranges::any_of(
             rows_, [](const Row& row) { return row.generation != row.presented_generation; });
}

// NOLINTEND(bugprone-exception-escape)

} // namespace frame::render
