#include "render/ui.hpp"

#include "frame/geometry.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace frame::render::ui {
namespace {

[[nodiscard]] auto append_bytes(const std::span<std::byte> output, std::size_t& used,
                                const std::string_view value) noexcept -> bool {
  if (used > output.size() || value.size() > output.size() - used) {
    return false;
  }
  std::memcpy(output.subspan(used).data(), value.data(), value.size());
  used += value.size();
  return true;
}

[[nodiscard]] auto append_integer(const std::span<std::byte> output, std::size_t& used,
                                  const std::uint16_t value) noexcept -> bool {
  std::array<char, 8> storage{};
  const auto encoded = std::to_chars(storage.begin(), storage.end(), value);
  if (encoded.ec != std::errc{}) {
    return false;
  }
  return append_bytes(
      output, used,
      std::string_view(storage.data(), static_cast<std::size_t>(encoded.ptr - storage.data())));
}

[[nodiscard]] auto append_position(const std::span<std::byte> output, std::size_t& used,
                                   const std::uint16_t row, const std::uint16_t column) noexcept
    -> bool {
  return append_bytes(output, used, "\x1B[") && append_integer(output, used, row) &&
         append_bytes(output, used, ";") && append_integer(output, used, column) &&
         append_bytes(output, used, "H");
}

[[nodiscard]] auto append_style(const std::span<std::byte> output, std::size_t& used,
                                const Style style) noexcept -> bool {
  if ((style.attributes & ~attributes_all) != 0U || !append_bytes(output, used, "\x1B[0")) {
    return false;
  }
  return ((style.attributes & attribute_bold) == 0U || append_bytes(output, used, ";1")) &&
         ((style.attributes & attribute_underline) == 0U || append_bytes(output, used, ";4")) &&
         ((style.attributes & attribute_reverse) == 0U || append_bytes(output, used, ";7")) &&
         append_bytes(output, used, "m");
}

[[nodiscard]] auto append_spaces(const std::span<std::byte> output, std::size_t& used,
                                 std::size_t count) noexcept -> bool {
  constexpr std::string_view spaces =
      "                                                                ";
  while (count > 0) {
    const auto chunk = std::min(count, spaces.size());
    // The explicit length bounds this non-null-terminated view.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    if (!append_bytes(output, used, std::string_view(spaces.data(), chunk))) {
      return false;
    }
    count -= chunk;
  }
  return true;
}

[[nodiscard]] auto clear_rectangle(const PaneRectangle rectangle, const std::span<std::byte> output,
                                   std::size_t& used) noexcept -> bool {
  for (std::uint16_t row = 0; row < rectangle.rows; ++row) {
    if (!append_position(output, used, static_cast<std::uint16_t>(rectangle.row + row + 1U),
                         static_cast<std::uint16_t>(rectangle.column + 1U)) ||
        !append_bytes(output, used, "\x1B[0m") || !append_spaces(output, used, rectangle.columns)) {
      return false;
    }
  }
  return true;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto paint_cells(const PaneRectangle rectangle, const std::uint16_t columns,
                 const std::uint16_t rows, const std::span<const Cell> cells,
                 const std::span<std::byte> output, std::size_t& used) noexcept -> bool {
  if (columns == 0 || rows == 0 || rectangle.columns == 0 || rectangle.rows == 0 ||
      static_cast<std::size_t>(columns) * rows != cells.size() ||
      !clear_rectangle(rectangle, output, used)) {
    return false;
  }
  const auto visible_columns = std::min(columns, rectangle.columns);
  const auto visible_rows = std::min(rows, rectangle.rows);
  for (std::uint16_t row = 0; row < visible_rows; ++row) {
    const auto source = cells.subspan(static_cast<std::size_t>(row) * columns, columns);
    std::size_t column = 0;
    while (column < visible_columns) {
      while (column < visible_columns && !source.subspan(column, 1).front().painted) {
        ++column;
      }
      if (column == visible_columns) {
        break;
      }
      if (!append_position(output, used, static_cast<std::uint16_t>(rectangle.row + row + 1U),
                           static_cast<std::uint16_t>(rectangle.column + column + 1U))) {
        return false;
      }
      std::optional<Style> previous;
      while (column < visible_columns) {
        const auto& cell = source.subspan(column, 1).front();
        if (!cell.painted) {
          break;
        }
        if (cell.text_size == 0 || cell.text_size > cell.text.size() ||
            ((!previous.has_value() || *previous != cell.style) &&
             !append_style(output, used, cell.style)) ||
            !append_bytes(output, used, std::string_view(cell.text.data(), cell.text_size))) {
          return false;
        }
        previous = cell.style;
        ++column;
      }
      if (!append_bytes(output, used, "\x1B[0m")) {
        return false;
      }
    }
  }
  return true;
}

} // namespace frame::render::ui
