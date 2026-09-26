#ifndef LEMMA_CORE_FLOAT_LAYER_HPP
#define LEMMA_CORE_FLOAT_LAYER_HPP

#include "lemma/geometry.hpp"
#include "lemma/id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace lemma::core {

inline constexpr std::size_t floats_per_tab_max = 8;
// A float's native frame is one cell on every side, so its outer rectangle holds at least one
// terminal cell.
inline constexpr std::uint16_t float_outer_extent_min = 3;
inline constexpr std::uint16_t float_percent_max = 100;

enum class FloatPlacementKind : std::uint8_t {
  absolute,
  centered,
  relative,
};

// A floating Pane's requested outer rectangle, including its native frame. Values exist only
// through validated factories. Resolution against a Tab viewport is recomputed on every geometry
// change rather than stored, so suspension cannot disagree with the viewport it depends on.
class FloatPlacement final {
public:
  // Outer rectangle at a Tab-viewport-relative origin.
  [[nodiscard]] static auto absolute(std::uint16_t column, std::uint16_t row, std::uint16_t columns,
                                     std::uint16_t rows) noexcept -> std::optional<FloatPlacement>;
  // Outer extent centered in the Tab viewport.
  [[nodiscard]] static auto centered(std::uint16_t columns, std::uint16_t rows) noexcept
      -> std::optional<FloatPlacement>;
  // Centered outer extent as 1..100 percent of the Tab viewport, never below the minimum extent.
  [[nodiscard]] static auto relative(std::uint16_t width_percent,
                                     std::uint16_t height_percent) noexcept
      -> std::optional<FloatPlacement>;

  // The outer rectangle in viewport coordinates, or nothing when the float does not fit and is
  // suspended. A float is never clipped or moved to make it fit.
  [[nodiscard]] auto resolve(PaneRectangle viewport) const noexcept -> std::optional<PaneRectangle>;

  [[nodiscard]] constexpr auto kind() const noexcept -> FloatPlacementKind { return kind_; }
  // Absolute origin; zero for other kinds.
  [[nodiscard]] constexpr auto column() const noexcept -> std::uint16_t { return column_; }
  [[nodiscard]] constexpr auto row() const noexcept -> std::uint16_t { return row_; }
  // Outer extent in cells, or percentages for a relative placement.
  [[nodiscard]] constexpr auto columns() const noexcept -> std::uint16_t { return columns_; }
  [[nodiscard]] constexpr auto rows() const noexcept -> std::uint16_t { return rows_; }

  friend constexpr auto operator==(const FloatPlacement&, const FloatPlacement&) noexcept
      -> bool = default;

private:
  // Only unused layer storage holds the default value.
  friend struct FloatEntry;
  friend class FloatLayer;

  constexpr FloatPlacement() noexcept = default;
  constexpr FloatPlacement(const FloatPlacementKind kind, const std::uint16_t column,
                           const std::uint16_t row, const std::uint16_t columns,
                           const std::uint16_t rows) noexcept
      : kind_(kind), column_(column), row_(row), columns_(columns), rows_(rows) {}

  // The default is the smallest valid centered float.
  FloatPlacementKind kind_{FloatPlacementKind::centered};
  std::uint16_t column_{0};
  std::uint16_t row_{0};
  std::uint16_t columns_{float_outer_extent_min};
  std::uint16_t rows_{float_outer_extent_min};
};

// The Pane rectangle (PTY geometry) inside a resolved outer rectangle's native frame.
[[nodiscard]] constexpr auto float_inner_rectangle(const PaneRectangle outer) noexcept
    -> PaneRectangle {
  return {.column = static_cast<std::uint16_t>(outer.column + 1U),
          .row = static_cast<std::uint16_t>(outer.row + 1U),
          .columns = static_cast<std::uint16_t>(outer.columns - 2U),
          .rows = static_cast<std::uint16_t>(outer.rows - 2U)};
}

struct FloatEntry final {
  PaneId pane;
  FloatPlacement placement;

  friend constexpr auto operator==(const FloatEntry&, const FloatEntry&) noexcept -> bool = default;
};

// A Tab's fixed-capacity floating Panes in back-to-front order: the last entry is the top float.
// Trivially copyable, so transitions can stage a candidate layer and publish it only on success.
class FloatLayer final {
public:
  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] constexpr auto empty() const noexcept -> bool { return size_ == 0; }
  [[nodiscard]] auto entries() const noexcept -> std::span<const FloatEntry> {
    return std::span(entries_).first(size_);
  }
  [[nodiscard]] auto contains(PaneId pane) const noexcept -> bool;
  [[nodiscard]] auto placement(PaneId pane) const noexcept -> std::optional<FloatPlacement>;
  // Zero is the bottom float.
  [[nodiscard]] auto z(PaneId pane) const noexcept -> std::optional<std::size_t>;
  [[nodiscard]] auto top() const noexcept -> std::optional<PaneId>;

  // Adds pane as the new top float; rejects an invalid or present Pane and a full layer.
  [[nodiscard]] auto push(PaneId pane, FloatPlacement placement) noexcept -> bool;
  [[nodiscard]] auto erase(PaneId pane) noexcept -> bool;
  // Moves pane to the top, preserving the relative order of the others.
  [[nodiscard]] auto raise(PaneId pane) noexcept -> bool;
  [[nodiscard]] auto place(PaneId pane, FloatPlacement placement) noexcept -> bool;

  friend constexpr auto operator==(const FloatLayer&, const FloatLayer&) noexcept -> bool = default;

private:
  [[nodiscard]] auto index_of(PaneId pane) const noexcept -> std::optional<std::size_t>;

  // Entries past size_ stay default so equality compares only live state.
  std::array<FloatEntry, floats_per_tab_max> entries_{};
  std::uint8_t size_{0};
};

static_assert(std::is_trivially_copyable_v<FloatLayer>);

} // namespace lemma::core

#endif // LEMMA_CORE_FLOAT_LAYER_HPP
