#ifndef FRAME_RENDER_UI_HPP
#define FRAME_RENDER_UI_HPP

#include "frame/geometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace frame::render::ui {

// Built-in status and rename text is validated before composition. One cell stores one UTF-8
// scalar from that bounded text projection.
inline constexpr std::size_t cell_text_bytes_max = 4;

inline constexpr std::uint16_t attribute_bold = 1U << 0U;
inline constexpr std::uint16_t attribute_underline = 1U << 1U;
inline constexpr std::uint16_t attribute_reverse = 1U << 2U;
inline constexpr std::uint16_t attributes_all =
    attribute_bold | attribute_underline | attribute_reverse;

struct Style final {
  std::uint16_t attributes{0};

  [[nodiscard]] constexpr auto operator==(const Style&) const noexcept -> bool = default;
};

struct Cell final {
  std::array<char, cell_text_bytes_max> text{};
  Style style;
  std::uint8_t text_size{0};
  bool painted{false};
};

// Paints one opaque built-in cell surface without allocation. The status and rename projections use
// this path so styling, clearing, clipping, and positioning have one bounded implementation.
[[nodiscard]] auto paint_cells(PaneRectangle rectangle, std::uint16_t columns, std::uint16_t rows,
                               std::span<const Cell> cells, std::span<std::byte> output,
                               std::size_t& used) noexcept -> bool;

} // namespace frame::render::ui

#endif // FRAME_RENDER_UI_HPP
