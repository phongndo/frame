#include "core/float_layer.hpp"

#include "lemma/geometry.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace lemma::core {
namespace {

[[nodiscard]] constexpr auto valid_extent(const std::uint32_t extent,
                                          const std::uint32_t maximum) noexcept -> bool {
  return extent >= float_outer_extent_min && extent <= maximum;
}

// Rounds percent of available to the nearest cell, within the minimum extent and available space.
[[nodiscard]] constexpr auto percent_extent(const std::uint16_t available,
                                            const std::uint16_t percent) noexcept -> std::uint16_t {
  const auto scaled = ((static_cast<std::uint32_t>(available) * percent) + 50U) / 100U;
  return static_cast<std::uint16_t>(
      std::clamp<std::uint32_t>(scaled, float_outer_extent_min, available));
}

[[nodiscard]] constexpr auto centered_rectangle(const PaneRectangle viewport,
                                                const std::uint16_t columns,
                                                const std::uint16_t rows) noexcept
    -> std::optional<PaneRectangle> {
  if (columns > viewport.columns || rows > viewport.rows) {
    return std::nullopt;
  }
  return PaneRectangle{
      .column = static_cast<std::uint16_t>(viewport.column + ((viewport.columns - columns) / 2U)),
      .row = static_cast<std::uint16_t>(viewport.row + ((viewport.rows - rows) / 2U)),
      .columns = columns,
      .rows = rows};
}

} // namespace

auto FloatPlacement::absolute(const std::uint16_t column, const std::uint16_t row,
                              const std::uint16_t columns, const std::uint16_t rows) noexcept
    -> std::optional<FloatPlacement> {
  // A placement that cannot fit the largest viewport could never be presented.
  if (!valid_extent(static_cast<std::uint32_t>(column) + columns,
                    limits::terminal_columns_hard_max) ||
      !valid_extent(static_cast<std::uint32_t>(row) + rows, limits::terminal_rows_hard_max) ||
      columns < float_outer_extent_min || rows < float_outer_extent_min) {
    return std::nullopt;
  }
  return FloatPlacement(FloatPlacementKind::absolute, column, row, columns, rows);
}

auto FloatPlacement::centered(const std::uint16_t columns, const std::uint16_t rows) noexcept
    -> std::optional<FloatPlacement> {
  if (!valid_extent(columns, limits::terminal_columns_hard_max) ||
      !valid_extent(rows, limits::terminal_rows_hard_max)) {
    return std::nullopt;
  }
  return FloatPlacement(FloatPlacementKind::centered, 0, 0, columns, rows);
}

auto FloatPlacement::relative(const std::uint16_t width_percent,
                              const std::uint16_t height_percent) noexcept
    -> std::optional<FloatPlacement> {
  if (width_percent == 0 || width_percent > float_percent_max || height_percent == 0 ||
      height_percent > float_percent_max) {
    return std::nullopt;
  }
  return FloatPlacement(FloatPlacementKind::relative, 0, 0, width_percent, height_percent);
}

auto FloatPlacement::resolve(const PaneRectangle viewport) const noexcept
    -> std::optional<PaneRectangle> {
  switch (kind_) {
  case FloatPlacementKind::absolute:
    if (static_cast<std::uint32_t>(column_) + columns_ > viewport.columns ||
        static_cast<std::uint32_t>(row_) + rows_ > viewport.rows) {
      return std::nullopt;
    }
    return PaneRectangle{.column = static_cast<std::uint16_t>(viewport.column + column_),
                         .row = static_cast<std::uint16_t>(viewport.row + row_),
                         .columns = columns_,
                         .rows = rows_};
  case FloatPlacementKind::centered:
    return centered_rectangle(viewport, columns_, rows_);
  case FloatPlacementKind::relative:
    if (viewport.columns < float_outer_extent_min || viewport.rows < float_outer_extent_min) {
      return std::nullopt;
    }
    return centered_rectangle(viewport, percent_extent(viewport.columns, columns_),
                              percent_extent(viewport.rows, rows_));
  }
  return std::nullopt;
}

auto FloatLayer::index_of(const PaneId pane) const noexcept -> std::optional<std::size_t> {
  if (!pane.is_valid()) {
    return std::nullopt;
  }
  const auto live = entries();
  for (std::size_t index = 0; index < live.size(); ++index) {
    if (live.subspan(index, 1).front().pane == pane) {
      return index;
    }
  }
  return std::nullopt;
}

auto FloatLayer::contains(const PaneId pane) const noexcept -> bool {
  return index_of(pane).has_value();
}

auto FloatLayer::placement(const PaneId pane) const noexcept -> std::optional<FloatPlacement> {
  const auto index = index_of(pane);
  return index.has_value() ? std::optional{std::span(entries_).subspan(*index, 1).front().placement}
                           : std::nullopt;
}

auto FloatLayer::z(const PaneId pane) const noexcept -> std::optional<std::size_t> {
  return index_of(pane);
}

auto FloatLayer::top() const noexcept -> std::optional<PaneId> {
  return size_ == 0 ? std::nullopt
                    : std::optional{std::span(entries_).subspan(size_ - 1U, 1).front().pane};
}

auto FloatLayer::push(const PaneId pane, const FloatPlacement placement) noexcept -> bool {
  if (!pane.is_valid() || size_ == entries_.size() || contains(pane)) {
    return false;
  }
  std::span(entries_).subspan(size_, 1).front() = {.pane = pane, .placement = placement};
  ++size_;
  return true;
}

auto FloatLayer::erase(const PaneId pane) noexcept -> bool {
  const auto index = index_of(pane);
  if (!index.has_value()) {
    return false;
  }
  auto live = std::span(entries_).first(size_);
  std::ranges::copy(live.subspan(*index + 1U), live.subspan(*index).begin());
  --size_;
  std::span(entries_).subspan(size_, 1).front() = {};
  return true;
}

auto FloatLayer::raise(const PaneId pane) noexcept -> bool {
  const auto index = index_of(pane);
  if (!index.has_value()) {
    return false;
  }
  auto live = std::span(entries_).first(size_);
  std::ranges::rotate(live.subspan(*index), live.subspan(*index + 1U).begin());
  return true;
}

auto FloatLayer::place(const PaneId pane, const FloatPlacement placement) noexcept -> bool {
  const auto index = index_of(pane);
  if (!index.has_value()) {
    return false;
  }
  std::span(entries_).subspan(*index, 1).front().placement = placement;
  return true;
}

} // namespace lemma::core
