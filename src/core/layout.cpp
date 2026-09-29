#include "core/layout.hpp"

#include "frame/assert.hpp"
#include "frame/geometry.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace frame::core {

namespace {

[[nodiscard]] constexpr auto valid_node_index(const std::int16_t index) noexcept -> bool {
  return index >= 0 && std::cmp_less(index, pane_layout_nodes_max);
}

[[nodiscard]] constexpr auto valid_split_axis(const SplitAxis axis) noexcept -> bool {
  switch (axis) {
  case SplitAxis::left_right:
  case SplitAxis::top_bottom:
    return true;
  }
  return false;
}

[[nodiscard]] constexpr auto valid_resize_direction(const ResizeDirection direction) noexcept
    -> bool {
  switch (direction) {
  case ResizeDirection::left:
  case ResizeDirection::right:
  case ResizeDirection::up:
  case ResizeDirection::down:
    return true;
  }
  return false;
}

} // namespace

auto LayoutProjection::rectangle(const PaneId pane) const noexcept -> std::optional<PaneRectangle> {
  if (!pane.is_valid() || pane.slot() >= included.size() ||
      !std::span(included).subspan(pane.slot(), 1).front() ||
      std::span(panes).subspan(pane.slot(), 1).front() != pane) {
    return std::nullopt;
  }
  return std::span(rectangles).subspan(pane.slot(), 1).front();
}

PaneLayout::PaneLayout(const PaneId first_pane) noexcept {
  FRAME_ASSERT(first_pane.is_valid() && first_pane.slot() < pane_layout_panes_max);
  nodes_.front() = {.pane = first_pane, .ratio = {}, .active = true};
  FRAME_ASSERT(valid());
}

auto PaneLayout::SplitRatio::from_extents(const std::uint16_t first,
                                          const std::uint16_t second) noexcept -> SplitRatio {
  FRAME_ASSERT(first > 0 && second > 0);
  const auto total = static_cast<std::uint32_t>(first) + second;
  FRAME_ASSERT(total <= std::numeric_limits<std::uint16_t>::max());
  const auto scaled = (static_cast<std::uint64_t>(first) * scale) + (total / 2U);
  const auto share = static_cast<std::uint32_t>(scaled / total);
  FRAME_ASSERT(share > 0 && share < scale);
  return SplitRatio(static_cast<std::uint16_t>(share));
}

auto PaneLayout::SplitRatio::first_extent(const std::uint16_t available) const noexcept
    -> std::uint16_t {
  FRAME_ASSERT(valid() && available > 0);
  const auto scaled = (static_cast<std::uint64_t>(available) * first_share_) + (scale / 2U);
  return static_cast<std::uint16_t>(scaled / scale);
}

auto PaneLayout::node_for_pane(const PaneId pane) const noexcept -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto& node = std::span(nodes_).subspan(index, 1).front();
    if (node.active && node.leaf && node.pane == pane) {
      return index;
    }
  }
  return std::nullopt;
}

auto PaneLayout::node_for_divider(const LayoutDivider divider) const noexcept
    -> std::optional<std::size_t> {
  if (!divider.first.is_valid() || !divider.second.is_valid() || divider.first == divider.second ||
      !valid_split_axis(divider.axis)) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto& node = std::span(nodes_).subspan(index, 1).front();
    if (!node.active || node.leaf || node.axis != divider.axis || !valid_node_index(node.first) ||
        !valid_node_index(node.second)) {
      continue;
    }
    const auto first = first_leaf(static_cast<std::size_t>(node.first));
    const auto second = first_leaf(static_cast<std::size_t>(node.second));
    if (first == divider.first && second == divider.second) {
      return index;
    }
  }
  return std::nullopt;
}

auto PaneLayout::contains(const PaneId pane) const noexcept -> bool {
  return node_for_pane(pane).has_value();
}

auto PaneLayout::pane_count() const noexcept -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(nodes_, [](const Node& node) { return node.active && node.leaf; }));
}

// The recursive branch mirrors the bounded binary topology into one dense value.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto PaneLayout::snapshot() const noexcept -> std::optional<LayoutSnapshot> {
  LayoutSnapshot output;
  const auto visit = [&](auto&& self, const std::size_t source_index,
                         const std::size_t depth) noexcept -> std::optional<std::int16_t> {
    if (source_index >= nodes_.size() || depth >= limits::layout_depth_hard_max ||
        output.size >= output.nodes.size()) {
      return std::nullopt;
    }
    const auto& source = std::span(nodes_).subspan(source_index, 1).front();
    if (!source.active) {
      return std::nullopt;
    }
    const auto target_index = output.size++;
    auto& target = std::span(output.nodes).subspan(target_index, 1).front();
    target = {.pane = source.pane,
              .first = -1,
              .second = -1,
              .ratio = source.leaf ? std::uint16_t{0} : source.ratio.value(),
              .leaf = source.leaf,
              .axis = source.axis};
    if (source.leaf) {
      return static_cast<std::int16_t>(target_index);
    }
    if (!valid_node_index(source.first) || !valid_node_index(source.second)) {
      return std::nullopt;
    }
    const auto first = self(self, static_cast<std::size_t>(source.first), depth + 1U);
    const auto second = self(self, static_cast<std::size_t>(source.second), depth + 1U);
    if (!first.has_value() || !second.has_value()) {
      return std::nullopt;
    }
    target.first = *first;
    target.second = *second;
    return static_cast<std::int16_t>(target_index);
  };
  const auto root = visit(visit, 0, 0);
  return root == std::optional<std::int16_t>{0} ? std::optional{output} : std::nullopt;
}

auto PaneLayout::first_leaf(std::size_t node_index) const noexcept -> PaneId {
  for (std::size_t depth = 0; depth < limits::layout_depth_hard_max; ++depth) {
    if (node_index >= nodes_.size()) {
      return {};
    }
    const auto& node = std::span(nodes_).subspan(node_index, 1).front();
    if (!node.active) {
      return {};
    }
    if (node.leaf) {
      return node.pane;
    }
    if (!valid_node_index(node.first)) {
      return {};
    }
    node_index = static_cast<std::size_t>(node.first);
  }
  return {};
}

// Recursive validation and axis-specific minimum composition are deliberately kept together.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto PaneLayout::compute_minimum(const std::size_t node_index, const std::size_t depth,
                                 MinimumExtents& extents) const noexcept -> bool {
  if (node_index >= nodes_.size() || depth >= limits::layout_depth_hard_max) {
    return false;
  }
  const auto& node = std::span(nodes_).subspan(node_index, 1).front();
  if (!node.active) {
    return false;
  }
  if (node.leaf) {
    if (!node.pane.is_valid() || node.pane.slot() >= pane_layout_panes_max) {
      return false;
    }
    std::span(extents).subspan(node_index, 1).front() = {.columns = 1, .rows = 1};
    return true;
  }
  if (!valid_node_index(node.first) || !valid_node_index(node.second) ||
      node.first == node.second || !node.ratio.valid() || !valid_split_axis(node.axis)) {
    return false;
  }
  const auto first = static_cast<std::size_t>(node.first);
  const auto second = static_cast<std::size_t>(node.second);
  if (!compute_minimum(first, depth + 1U, extents) ||
      !compute_minimum(second, depth + 1U, extents)) {
    return false;
  }
  const auto first_minimum = std::span(extents).subspan(first, 1).front();
  const auto second_minimum = std::span(extents).subspan(second, 1).front();
  MinimumExtent result;
  switch (node.axis) {
  case SplitAxis::left_right: {
    const auto columns =
        static_cast<std::uint32_t>(first_minimum.columns) + 1U + second_minimum.columns;
    if (columns > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }
    result.columns = static_cast<std::uint16_t>(columns);
    result.rows = std::max(first_minimum.rows, second_minimum.rows);
    break;
  }
  case SplitAxis::top_bottom: {
    const auto rows = static_cast<std::uint32_t>(first_minimum.rows) + 1U + second_minimum.rows;
    if (rows > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }
    result.columns = std::max(first_minimum.columns, second_minimum.columns);
    result.rows = static_cast<std::uint16_t>(rows);
    break;
  }
  }
  std::span(extents).subspan(node_index, 1).front() = result;
  return true;
}

// Projection handles both axes explicitly so partition and separator arithmetic stay local.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto PaneLayout::project_node(const std::size_t node_index, const PaneRectangle rectangle,
                              const std::size_t depth, const MinimumExtents& extents,
                              LayoutProjection& projection,
                              NodeRectangles* const node_rectangles) const noexcept -> bool {
  if (node_index >= nodes_.size() || depth >= limits::layout_depth_hard_max ||
      rectangle.columns == 0 || rectangle.rows == 0) {
    return false;
  }
  const auto& node = std::span(nodes_).subspan(node_index, 1).front();
  if (!node.active) {
    return false;
  }
  if (node_rectangles != nullptr) {
    std::span(*node_rectangles).subspan(node_index, 1).front() = rectangle;
  }
  if (node.leaf) {
    if (!node.pane.is_valid()) {
      return false;
    }
    const auto slot = static_cast<std::size_t>(node.pane.slot());
    if (slot >= projection.included.size() ||
        std::span(projection.included).subspan(slot, 1).front()) {
      return false;
    }
    std::span(projection.rectangles).subspan(slot, 1).front() = rectangle;
    std::span(projection.panes).subspan(slot, 1).front() = node.pane;
    std::span(projection.included).subspan(slot, 1).front() = true;
    ++projection.pane_count;
    return true;
  }
  if (!valid_node_index(node.first) || !valid_node_index(node.second)) {
    return false;
  }
  const auto first = static_cast<std::size_t>(node.first);
  const auto second = static_cast<std::size_t>(node.second);
  const auto first_minimum = std::span(extents).subspan(first, 1).front();
  const auto second_minimum = std::span(extents).subspan(second, 1).front();
  auto first_rectangle = rectangle;
  auto second_rectangle = rectangle;

  switch (node.axis) {
  case SplitAxis::left_right: {
    if (rectangle.columns < 3) {
      return false;
    }
    const auto available = static_cast<std::uint16_t>(rectangle.columns - 1U);
    if (available < static_cast<std::uint32_t>(first_minimum.columns) + second_minimum.columns) {
      return false;
    }
    const auto maximum_first = static_cast<std::uint16_t>(available - second_minimum.columns);
    const auto first_columns =
        std::clamp(node.ratio.first_extent(available), first_minimum.columns, maximum_first);
    first_rectangle.columns = first_columns;
    second_rectangle.column = static_cast<std::uint16_t>(rectangle.column + first_columns + 1U);
    second_rectangle.columns = static_cast<std::uint16_t>(available - first_columns);
    break;
  }
  case SplitAxis::top_bottom: {
    if (rectangle.rows < 3) {
      return false;
    }
    const auto available = static_cast<std::uint16_t>(rectangle.rows - 1U);
    if (available < static_cast<std::uint32_t>(first_minimum.rows) + second_minimum.rows) {
      return false;
    }
    const auto maximum_first = static_cast<std::uint16_t>(available - second_minimum.rows);
    const auto first_rows =
        std::clamp(node.ratio.first_extent(available), first_minimum.rows, maximum_first);
    first_rectangle.rows = first_rows;
    second_rectangle.row = static_cast<std::uint16_t>(rectangle.row + first_rows + 1U);
    second_rectangle.rows = static_cast<std::uint16_t>(available - first_rows);
    break;
  }
  }

  return project_node(first, first_rectangle, depth + 1U, extents, projection, node_rectangles) &&
         project_node(second, second_rectangle, depth + 1U, extents, projection, node_rectangles);
}

auto PaneLayout::project_internal(const PaneRectangle viewport, LayoutProjection& projection,
                                  NodeRectangles* const node_rectangles,
                                  MinimumExtents* const minimum_extents) const noexcept -> bool {
  constexpr auto coordinate_end_max =
      static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()) + 1U;
  if (viewport.columns == 0 || viewport.rows == 0 ||
      static_cast<std::uint32_t>(viewport.column) + viewport.columns > coordinate_end_max ||
      static_cast<std::uint32_t>(viewport.row) + viewport.rows > coordinate_end_max) {
    return false;
  }
  projection = {};
  if (node_rectangles != nullptr) {
    *node_rectangles = {};
  }
  MinimumExtents extents{};
  if (!compute_minimum(0, 0, extents)) {
    return false;
  }
  const auto root_minimum = extents.front();
  if (viewport.columns < root_minimum.columns || viewport.rows < root_minimum.rows) {
    return false;
  }
  if (!project_node(0, viewport, 0, extents, projection, node_rectangles) ||
      projection.pane_count != pane_count()) {
    return false;
  }
  if (minimum_extents != nullptr) {
    *minimum_extents = extents;
  }
  return true;
}

auto PaneLayout::project(const PaneRectangle viewport) const noexcept
    -> std::optional<LayoutProjection> {
  LayoutProjection projection;
  if (!project_internal(viewport, projection, nullptr, nullptr)) {
    return std::nullopt;
  }
  return projection;
}

auto PaneLayout::divider_at(const PaneRectangle viewport, const std::uint16_t column,
                            const std::uint16_t row) const noexcept
    -> std::optional<LayoutDivider> {
  LayoutProjection projection;
  NodeRectangles node_rectangles{};
  if (!project_internal(viewport, projection, &node_rectangles, nullptr)) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto& node = std::span(nodes_).subspan(index, 1).front();
    if (!node.active || node.leaf || !valid_node_index(node.first) ||
        !valid_node_index(node.second)) {
      continue;
    }
    const auto first_index = static_cast<std::size_t>(node.first);
    const auto rectangle = std::span(node_rectangles).subspan(index, 1).front();
    const auto first_rectangle = std::span(node_rectangles).subspan(first_index, 1).front();
    bool hit = false;
    switch (node.axis) {
    case SplitAxis::left_right: {
      const auto separator =
          static_cast<std::uint32_t>(first_rectangle.column) + first_rectangle.columns;
      hit = column == separator && row >= rectangle.row &&
            static_cast<std::uint32_t>(row) <
                static_cast<std::uint32_t>(rectangle.row) + rectangle.rows;
      break;
    }
    case SplitAxis::top_bottom: {
      const auto separator = static_cast<std::uint32_t>(first_rectangle.row) + first_rectangle.rows;
      hit = row == separator && column >= rectangle.column &&
            static_cast<std::uint32_t>(column) <
                static_cast<std::uint32_t>(rectangle.column) + rectangle.columns;
      break;
    }
    }
    if (!hit) {
      continue;
    }
    const auto divider = LayoutDivider{
        .first = first_leaf(first_index),
        .second = first_leaf(static_cast<std::size_t>(node.second)),
        .axis = node.axis,
    };
    if (!divider.first.is_valid() || !divider.second.is_valid()) {
      return std::nullopt;
    }
    return divider;
  }
  return std::nullopt;
}

auto PaneLayout::divider_rectangle(const LayoutDivider divider,
                                   const PaneRectangle viewport) const noexcept
    -> std::optional<PaneRectangle> {
  const auto divider_node = node_for_divider(divider);
  if (!divider_node.has_value()) {
    return std::nullopt;
  }
  LayoutProjection projection;
  NodeRectangles node_rectangles{};
  if (!project_internal(viewport, projection, &node_rectangles, nullptr)) {
    return std::nullopt;
  }
  const auto& node = std::span(nodes_).subspan(*divider_node, 1).front();
  if (!valid_node_index(node.first)) {
    return std::nullopt;
  }
  const auto rectangle = std::span(node_rectangles).subspan(*divider_node, 1).front();
  const auto first_rectangle =
      std::span(node_rectangles).subspan(static_cast<std::size_t>(node.first), 1).front();
  switch (node.axis) {
  case SplitAxis::left_right:
    return PaneRectangle{
        .column = static_cast<std::uint16_t>(first_rectangle.column + first_rectangle.columns),
        .row = rectangle.row,
        .columns = 1,
        .rows = rectangle.rows,
    };
  case SplitAxis::top_bottom:
    return PaneRectangle{
        .column = rectangle.column,
        .row = static_cast<std::uint16_t>(first_rectangle.row + first_rectangle.rows),
        .columns = rectangle.columns,
        .rows = 1,
    };
  }
  return std::nullopt;
}

// Splitting validates every bounded resource before replacing the source leaf.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto PaneLayout::split(const PaneId source, const PaneId added, const SplitAxis axis) noexcept
    -> bool {
  if (!source.is_valid() || !added.is_valid() || source == added ||
      added.slot() >= pane_layout_panes_max || contains(added) || !valid_split_axis(axis)) {
    return false;
  }
  for (const auto& node : nodes_) {
    if (node.active && node.leaf && node.pane.slot() == added.slot()) {
      return false;
    }
  }
  const auto parent_node = node_for_pane(source);
  if (!parent_node.has_value()) {
    return false;
  }

  std::size_t depth = 0;
  auto ancestor = std::span(nodes_).subspan(*parent_node, 1).front().parent;
  while (ancestor >= 0) {
    if (!valid_node_index(ancestor)) {
      return false;
    }
    ++depth;
    ancestor = std::span(nodes_).subspan(static_cast<std::size_t>(ancestor), 1).front().parent;
  }
  if (depth + 1U >= limits::layout_depth_hard_max) {
    return false;
  }

  std::optional<std::size_t> first_node;
  std::optional<std::size_t> second_node;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    if (std::span(nodes_).subspan(index, 1).front().active) {
      continue;
    }
    if (!first_node.has_value()) {
      first_node = index;
    } else {
      second_node = index;
      break;
    }
  }
  if (!first_node.has_value() || !second_node.has_value()) {
    return false;
  }

  const auto parent_parent = std::span(nodes_).subspan(*parent_node, 1).front().parent;
  std::span(nodes_).subspan(*parent_node, 1).front() = {
      .pane = {},
      .parent = parent_parent,
      .first = static_cast<std::int16_t>(*first_node),
      .second = static_cast<std::int16_t>(*second_node),
      .ratio = {},
      .active = true,
      .leaf = false,
      .axis = axis,
  };
  std::span(nodes_).subspan(*first_node, 1).front() = {
      .pane = source,
      .parent = static_cast<std::int16_t>(*parent_node),
      .ratio = {},
      .active = true,
  };
  std::span(nodes_).subspan(*second_node, 1).front() = {
      .pane = added,
      .parent = static_cast<std::int16_t>(*parent_node),
      .ratio = {},
      .active = true,
  };
  FRAME_ASSERT(valid());
  return true;
}

auto PaneLayout::swap(const PaneId first, const PaneId second) noexcept -> bool {
  if (!first.is_valid() || !second.is_valid() || first == second) {
    return false;
  }
  const auto first_node = node_for_pane(first);
  const auto second_node = node_for_pane(second);
  if (!first_node.has_value() || !second_node.has_value()) {
    return false;
  }
  std::swap(std::span(nodes_).subspan(*first_node, 1).front().pane,
            std::span(nodes_).subspan(*second_node, 1).front().pane);
  FRAME_ASSERT(valid());
  return true;
}

auto PaneLayout::remove(const PaneId pane) noexcept -> std::optional<PaneId> {
  if (pane_count() <= 1) {
    return std::nullopt;
  }
  const auto leaf_index = node_for_pane(pane);
  if (!leaf_index.has_value()) {
    return std::nullopt;
  }
  const auto leaf = std::span(nodes_).subspan(*leaf_index, 1).front();
  if (!valid_node_index(leaf.parent)) {
    return std::nullopt;
  }
  const auto parent_index = static_cast<std::size_t>(leaf.parent);
  const auto parent = std::span(nodes_).subspan(parent_index, 1).front();
  const auto sibling_encoded =
      parent.first == static_cast<std::int16_t>(*leaf_index) ? parent.second : parent.first;
  if (!valid_node_index(sibling_encoded)) {
    return std::nullopt;
  }
  const auto sibling_index = static_cast<std::size_t>(sibling_encoded);
  auto replacement = std::span(nodes_).subspan(sibling_index, 1).front();
  replacement.parent = parent.parent;
  std::span(nodes_).subspan(parent_index, 1).front() = replacement;
  if (!replacement.leaf) {
    FRAME_ASSERT(valid_node_index(replacement.first) && valid_node_index(replacement.second));
    std::span(nodes_).subspan(static_cast<std::size_t>(replacement.first), 1).front().parent =
        static_cast<std::int16_t>(parent_index);
    std::span(nodes_).subspan(static_cast<std::size_t>(replacement.second), 1).front().parent =
        static_cast<std::int16_t>(parent_index);
  }
  std::span(nodes_).subspan(*leaf_index, 1).front() = {};
  std::span(nodes_).subspan(sibling_index, 1).front() = {};
  const auto focus_candidate = first_leaf(parent_index);
  FRAME_ASSERT(focus_candidate.is_valid() && valid());
  return focus_candidate;
}

// Resizing one identified branch derives an exact cell boundary and verifies reprojection before
// publishing the ratio into the candidate layout.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto PaneLayout::resize_node(const std::size_t node_index, const PaneRectangle viewport,
                             const std::int32_t value, const ResizeValueKind value_kind) noexcept
    -> LayoutResizeStatus {
  if (node_index >= nodes_.size()) {
    return LayoutResizeStatus::invalid;
  }
  LayoutProjection previous_projection;
  NodeRectangles node_rectangles{};
  MinimumExtents minimum_extents{};
  if (!project_internal(viewport, previous_projection, &node_rectangles, &minimum_extents)) {
    return LayoutResizeStatus::unavailable;
  }

  auto& split_node = std::span(nodes_).subspan(node_index, 1).front();
  if (!split_node.active || split_node.leaf || !valid_node_index(split_node.first) ||
      !valid_node_index(split_node.second) || !valid_split_axis(split_node.axis)) {
    return LayoutResizeStatus::invalid;
  }
  const auto first_index = static_cast<std::size_t>(split_node.first);
  const auto second_index = static_cast<std::size_t>(split_node.second);
  const auto split_rectangle = std::span(node_rectangles).subspan(node_index, 1).front();
  const auto first_rectangle = std::span(node_rectangles).subspan(first_index, 1).front();
  const auto first_minimum = std::span(minimum_extents).subspan(first_index, 1).front();
  const auto second_minimum = std::span(minimum_extents).subspan(second_index, 1).front();

  std::uint16_t available = 0;
  std::uint16_t first_extent = 0;
  std::uint16_t minimum_first = 0;
  std::uint16_t minimum_second = 0;
  std::uint16_t split_origin = 0;
  switch (split_node.axis) {
  case SplitAxis::left_right:
    available = static_cast<std::uint16_t>(split_rectangle.columns - 1U);
    first_extent = first_rectangle.columns;
    minimum_first = first_minimum.columns;
    minimum_second = second_minimum.columns;
    split_origin = split_rectangle.column;
    break;
  case SplitAxis::top_bottom:
    available = static_cast<std::uint16_t>(split_rectangle.rows - 1U);
    first_extent = first_rectangle.rows;
    minimum_first = first_minimum.rows;
    minimum_second = second_minimum.rows;
    split_origin = split_rectangle.row;
    break;
  }

  std::int32_t requested_first = 0;
  switch (value_kind) {
  case ResizeValueKind::extent_delta:
    requested_first = static_cast<std::int32_t>(first_extent) + value;
    break;
  case ResizeValueKind::divider_coordinate:
    requested_first = value - split_origin;
    break;
  }
  const auto maximum_first = static_cast<std::int32_t>(available - minimum_second);
  const auto target_first = static_cast<std::uint16_t>(
      std::clamp(requested_first, static_cast<std::int32_t>(minimum_first), maximum_first));
  if (target_first == first_extent) {
    return LayoutResizeStatus::no_effect;
  }
  const auto target_second = static_cast<std::uint16_t>(available - target_first);
  const auto previous_ratio = split_node.ratio;
  split_node.ratio = SplitRatio::from_extents(target_first, target_second);
  if (split_node.ratio == previous_ratio) {
    return LayoutResizeStatus::no_effect;
  }

  LayoutProjection updated_projection;
  NodeRectangles updated_nodes{};
  if (!project_internal(viewport, updated_projection, &updated_nodes, nullptr)) {
    split_node.ratio = previous_ratio;
    return LayoutResizeStatus::invalid;
  }
  const auto projected_first = std::span(updated_nodes).subspan(first_index, 1).front();
  const auto projected_extent =
      split_node.axis == SplitAxis::left_right ? projected_first.columns : projected_first.rows;
  if (projected_extent != target_first) {
    split_node.ratio = previous_ratio;
    return LayoutResizeStatus::invalid;
  }

  FRAME_ASSERT(updated_projection != previous_projection && valid());
  return LayoutResizeStatus::applied;
}

auto PaneLayout::resize(const PaneId pane, const ResizeDirection direction,
                        const PaneRectangle viewport, const std::uint16_t amount) noexcept
    -> LayoutResizeStatus {
  if (!valid_resize_direction(direction) || amount == 0) {
    return LayoutResizeStatus::invalid;
  }
  const auto leaf_index = node_for_pane(pane);
  if (!leaf_index.has_value()) {
    return LayoutResizeStatus::invalid;
  }
  const auto requested_axis =
      direction == ResizeDirection::left || direction == ResizeDirection::right
          ? SplitAxis::left_right
          : SplitAxis::top_bottom;
  auto split_index = std::span(nodes_).subspan(*leaf_index, 1).front().parent;
  while (split_index >= 0) {
    if (!valid_node_index(split_index)) {
      return LayoutResizeStatus::invalid;
    }
    const auto& candidate =
        std::span(nodes_).subspan(static_cast<std::size_t>(split_index), 1).front();
    if (!candidate.leaf && candidate.axis == requested_axis) {
      break;
    }
    split_index = candidate.parent;
  }
  if (split_index < 0) {
    return LayoutResizeStatus::no_effect;
  }
  const auto delta =
      static_cast<std::int32_t>(amount) *
      (direction == ResizeDirection::left || direction == ResizeDirection::up ? -1 : 1);
  return resize_node(static_cast<std::size_t>(split_index), viewport, delta,
                     ResizeValueKind::extent_delta);
}

auto PaneLayout::resize_divider(const LayoutDivider divider, const std::uint16_t coordinate,
                                const PaneRectangle viewport) noexcept -> LayoutResizeStatus {
  const auto divider_node = node_for_divider(divider);
  if (!divider_node.has_value()) {
    return LayoutResizeStatus::invalid;
  }
  return resize_node(*divider_node, viewport, coordinate, ResizeValueKind::divider_coordinate);
}

auto PaneLayout::validate_node(const std::size_t node_index, const std::int16_t parent,
                               const std::size_t depth,
                               std::array<bool, pane_layout_nodes_max>& visited,
                               std::array<bool, pane_layout_panes_max>& pane_slots) const noexcept
    -> bool {
  if (node_index >= nodes_.size() || depth >= limits::layout_depth_hard_max ||
      std::span(visited).subspan(node_index, 1).front()) {
    return false;
  }
  const auto& node = std::span(nodes_).subspan(node_index, 1).front();
  if (!node.active || node.parent != parent) {
    return false;
  }
  std::span(visited).subspan(node_index, 1).front() = true;
  if (node.leaf) {
    if (!node.pane.is_valid() || node.pane.slot() >= pane_slots.size() || node.first >= 0 ||
        node.second >= 0) {
      return false;
    }
    auto& occupied = std::span(pane_slots).subspan(node.pane.slot(), 1).front();
    if (occupied) {
      return false;
    }
    occupied = true;
    return true;
  }
  if (node.pane.is_valid() || !node.ratio.valid() || !valid_split_axis(node.axis) ||
      !valid_node_index(node.first) || !valid_node_index(node.second) ||
      node.first == node.second) {
    return false;
  }
  return validate_node(static_cast<std::size_t>(node.first), static_cast<std::int16_t>(node_index),
                       depth + 1U, visited, pane_slots) &&
         validate_node(static_cast<std::size_t>(node.second), static_cast<std::int16_t>(node_index),
                       depth + 1U, visited, pane_slots);
}

auto PaneLayout::valid() const noexcept -> bool {
  std::array<bool, pane_layout_nodes_max> visited{};
  std::array<bool, pane_layout_panes_max> pane_slots{};
  if (!validate_node(0, -1, 0, visited, pane_slots)) {
    return false;
  }
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto active = std::span(nodes_).subspan(index, 1).front().active;
    if (active != std::span(visited).subspan(index, 1).front()) {
      return false;
    }
  }
  return true;
}

} // namespace frame::core
