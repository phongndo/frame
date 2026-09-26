#include "core/float_layer.hpp"
#include "core/layout.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace lemma::core {
namespace {

[[nodiscard]] auto checked_placement(const std::optional<FloatPlacement> placement)
    -> FloatPlacement {
  if (!placement.has_value()) {
    throw std::logic_error("test requires a valid float placement");
  }
  return *placement;
}

[[nodiscard]] constexpr auto pane(const std::uint32_t slot,
                                  const std::uint32_t generation = 1) noexcept -> PaneId {
  return PaneId::from_parts(slot, generation);
}

[[nodiscard]] auto rectangle(const PaneLayout& layout, const PaneId id,
                             const PaneRectangle viewport) -> PaneRectangle {
  const auto projection = layout.project(viewport);
  EXPECT_TRUE(projection.has_value());
  const auto result = projection.has_value() ? projection->rectangle(id) : std::nullopt;
  EXPECT_TRUE(result.has_value());
  return result.value_or(PaneRectangle{});
}

TEST(PaneLayoutTest, EqualSplitPreservesExistingOddCellBehavior) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));

  const auto projection = layout.project({.columns = 80, .rows = 23});

  ASSERT_TRUE(projection.has_value());
  const auto projected = projection.value_or(LayoutProjection{});
  EXPECT_EQ(projected.pane_count, 2U);
  EXPECT_EQ(projected.rectangle(pane(0)),
            (PaneRectangle{.column = 0, .row = 0, .columns = 40, .rows = 23}));
  EXPECT_EQ(projected.rectangle(pane(1)),
            (PaneRectangle{.column = 41, .row = 0, .columns = 39, .rows = 23}));
  EXPECT_FALSE(projected.rectangle(pane(0, 2)).has_value());
  EXPECT_TRUE(layout.valid());
}

TEST(PaneLayoutTest, IntrospectionSnapshotIsDenseAndPreservesTopology) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  ASSERT_TRUE(layout.split(pane(1), pane(2), SplitAxis::top_bottom));

  const auto snapshot = layout.snapshot();

  ASSERT_TRUE(snapshot.has_value());
  const auto value = snapshot.value_or(LayoutSnapshot{});
  ASSERT_EQ(value.size, 5U);
  const auto& root = value.nodes.at(0);
  EXPECT_FALSE(root.leaf);
  EXPECT_EQ(root.axis, SplitAxis::left_right);
  EXPECT_EQ(root.first, 1);
  EXPECT_EQ(root.second, 2);
  EXPECT_EQ(value.nodes.at(1).pane, pane(0));
  EXPECT_FALSE(value.nodes.at(2).leaf);
  EXPECT_EQ(value.nodes.at(2).axis, SplitAxis::top_bottom);
  EXPECT_EQ(value.nodes.at(3).pane, pane(1));
  EXPECT_EQ(value.nodes.at(4).pane, pane(2));
}

TEST(PaneLayoutTest, OneCellResizePersistsAcrossViewportChanges) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  constexpr PaneRectangle original_viewport{.columns = 80, .rows = 23};

  ASSERT_EQ(layout.resize(pane(1), ResizeDirection::left, original_viewport),
            LayoutResizeStatus::applied);
  EXPECT_EQ(rectangle(layout, pane(0), original_viewport).columns, 39U);
  EXPECT_EQ(rectangle(layout, pane(1), original_viewport).columns, 40U);

  constexpr PaneRectangle larger_viewport{.columns = 100, .rows = 23};
  EXPECT_EQ(rectangle(layout, pane(0), larger_viewport).columns, 49U);
  EXPECT_EQ(rectangle(layout, pane(1), larger_viewport).columns, 50U);

  const auto retained = layout;
  ASSERT_TRUE(layout.project({.columns = 3, .rows = 1}).has_value());
  EXPECT_EQ(layout, retained) << "projection and minimum clamping must not rewrite the ratio";
  EXPECT_EQ(rectangle(layout, pane(0), original_viewport).columns, 39U);
  EXPECT_EQ(rectangle(layout, pane(1), original_viewport).columns, 40U);
}

TEST(PaneLayoutTest, DividerHitMovesToPointerAndClampsAtStructuralMinimum) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  constexpr PaneRectangle viewport{.columns = 80, .rows = 23};

  const auto divider = layout.divider_at(viewport, 40, 10);

  ASSERT_TRUE(divider.has_value());
  const auto captured = divider.value_or(LayoutDivider{});
  EXPECT_EQ(captured,
            (LayoutDivider{.first = pane(0), .second = pane(1), .axis = SplitAxis::left_right}));
  EXPECT_EQ(layout.divider_rectangle(captured, viewport),
            (PaneRectangle{.column = 40, .row = 0, .columns = 1, .rows = 23}));
  EXPECT_FALSE(layout.divider_at(viewport, 39, 10).has_value());
  EXPECT_FALSE(layout.divider_at(viewport, 41, 10).has_value());
  ASSERT_EQ(layout.resize_divider(captured, 45, viewport), LayoutResizeStatus::applied);
  EXPECT_EQ(rectangle(layout, pane(0), viewport).columns, 45U);
  EXPECT_EQ(rectangle(layout, pane(1), viewport).columns, 34U);

  ASSERT_EQ(layout.resize_divider(captured, 0, viewport), LayoutResizeStatus::applied);
  EXPECT_EQ(rectangle(layout, pane(0), viewport).columns, 1U);
  EXPECT_EQ(rectangle(layout, pane(1), viewport).columns, 78U);
  EXPECT_EQ(layout.resize_divider(captured, 0, viewport), LayoutResizeStatus::no_effect);
  EXPECT_TRUE(layout.valid());
}

TEST(PaneLayoutTest, DividerHitSelectsExactNestedBranchAndStaleHandleDoesNotRetarget) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  ASSERT_TRUE(layout.split(pane(0), pane(2), SplitAxis::top_bottom));
  constexpr PaneRectangle viewport{.columns = 100, .rows = 41};

  const auto nested = layout.divider_at(viewport, 10, 20);
  const auto outer = layout.divider_at(viewport, 50, 20);

  ASSERT_TRUE(nested.has_value());
  ASSERT_TRUE(outer.has_value());
  const auto captured_nested = nested.value_or(LayoutDivider{});
  const auto captured_outer = outer.value_or(LayoutDivider{});
  EXPECT_EQ(captured_nested,
            (LayoutDivider{.first = pane(0), .second = pane(2), .axis = SplitAxis::top_bottom}));
  EXPECT_EQ(captured_outer,
            (LayoutDivider{.first = pane(0), .second = pane(1), .axis = SplitAxis::left_right}));
  EXPECT_EQ(layout.divider_rectangle(captured_nested, viewport),
            (PaneRectangle{.column = 0, .row = 20, .columns = 50, .rows = 1}));
  EXPECT_EQ(layout.divider_rectangle(captured_outer, viewport),
            (PaneRectangle{.column = 50, .row = 0, .columns = 1, .rows = 41}));
  const auto outer_pane_before = rectangle(layout, pane(1), viewport);
  ASSERT_EQ(layout.resize_divider(captured_nested, 25, viewport), LayoutResizeStatus::applied);
  EXPECT_EQ(rectangle(layout, pane(0), viewport).rows, 25U);
  EXPECT_EQ(rectangle(layout, pane(1), viewport), outer_pane_before);

  ASSERT_TRUE(layout.remove(pane(0)).has_value());
  EXPECT_EQ(layout.resize_divider(captured_nested, 10, viewport), LayoutResizeStatus::invalid);
  EXPECT_EQ(layout.resize_divider(captured_outer, 40, viewport), LayoutResizeStatus::invalid);
  EXPECT_FALSE(layout.divider_rectangle(captured_nested, viewport).has_value());
  EXPECT_FALSE(layout.divider_rectangle(captured_outer, viewport).has_value());
  EXPECT_TRUE(layout.valid());
}

TEST(PaneLayoutTest, DividerCoordinatesRemainAbsoluteInsideOffsetSubtree) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  ASSERT_TRUE(layout.split(pane(1), pane(2), SplitAxis::left_right));
  constexpr PaneRectangle viewport{.columns = 100, .rows = 23};
  const auto outer_left_before = rectangle(layout, pane(0), viewport);

  const auto divider = layout.divider_at(viewport, 75, 10);

  ASSERT_TRUE(divider.has_value());
  const auto captured = divider.value_or(LayoutDivider{});
  EXPECT_EQ(captured,
            (LayoutDivider{.first = pane(1), .second = pane(2), .axis = SplitAxis::left_right}));
  ASSERT_EQ(layout.resize_divider(captured, 80, viewport), LayoutResizeStatus::applied);
  EXPECT_EQ(layout.divider_rectangle(captured, viewport),
            (PaneRectangle{.column = 80, .row = 0, .columns = 1, .rows = 23}));
  EXPECT_EQ(rectangle(layout, pane(0), viewport), outer_left_before);
  EXPECT_EQ(rectangle(layout, pane(1), viewport).columns, 29U);
  EXPECT_EQ(rectangle(layout, pane(2), viewport).columns, 19U);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(PaneLayoutTest, EveryProjectedCellIsEitherPaneOrDivider) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  ASSERT_TRUE(layout.split(pane(0), pane(2), SplitAxis::top_bottom));
  ASSERT_TRUE(layout.split(pane(1), pane(3), SplitAxis::top_bottom));
  constexpr PaneRectangle viewport{.columns = 11, .rows = 7};
  const auto projection = layout.project(viewport);
  ASSERT_TRUE(projection.has_value());
  const auto projected = projection.value_or(LayoutProjection{});

  for (std::uint16_t row = 0; row < viewport.rows; ++row) {
    for (std::uint16_t column = 0; column < viewport.columns; ++column) {
      bool pane_cell = false;
      for (const auto id : {pane(0), pane(1), pane(2), pane(3)}) {
        const auto projected_pane = projected.rectangle(id).value_or(PaneRectangle{});
        pane_cell =
            pane_cell ||
            (column >= projected_pane.column && row >= projected_pane.row &&
             column < static_cast<std::uint32_t>(projected_pane.column) + projected_pane.columns &&
             row < static_cast<std::uint32_t>(projected_pane.row) + projected_pane.rows);
      }
      EXPECT_NE(pane_cell, layout.divider_at(viewport, column, row).has_value())
          << "column=" << column << " row=" << row;
    }
  }
}

TEST(PaneLayoutTest, StructuralMinimumPreventsZeroSizedNestedPanes) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  ASSERT_TRUE(layout.split(pane(0), pane(2), SplitAxis::left_right));
  constexpr PaneRectangle minimum_viewport{.columns = 5, .rows = 1};

  const auto projection = layout.project(minimum_viewport);

  ASSERT_TRUE(projection.has_value());
  EXPECT_EQ(projection.value_or(LayoutProjection{}).pane_count, 3U);
  EXPECT_EQ(rectangle(layout, pane(0), minimum_viewport).columns, 1U);
  EXPECT_EQ(rectangle(layout, pane(1), minimum_viewport).columns, 1U);
  EXPECT_EQ(rectangle(layout, pane(2), minimum_viewport).columns, 1U);
  EXPECT_FALSE(layout.project({.columns = 4, .rows = 1}).has_value());

  const auto retained = layout;
  EXPECT_EQ(layout.resize(pane(0), ResizeDirection::left, minimum_viewport),
            LayoutResizeStatus::no_effect);
  EXPECT_EQ(layout.resize(pane(0), ResizeDirection::right, minimum_viewport),
            LayoutResizeStatus::no_effect);
  EXPECT_EQ(layout, retained);
}

TEST(PaneLayoutTest, ResizeSelectsNearestMatchingStructuralAncestor) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  ASSERT_TRUE(layout.split(pane(0), pane(2), SplitAxis::top_bottom));
  ASSERT_TRUE(layout.split(pane(0), pane(3), SplitAxis::left_right));
  constexpr PaneRectangle viewport{.columns = 100, .rows = 41};
  const auto outer_right_before = rectangle(layout, pane(1), viewport);
  const auto target_before = rectangle(layout, pane(0), viewport);

  ASSERT_EQ(layout.resize(pane(0), ResizeDirection::right, viewport), LayoutResizeStatus::applied);

  const auto outer_right_after = rectangle(layout, pane(1), viewport);
  const auto target_after = rectangle(layout, pane(0), viewport);
  EXPECT_EQ(outer_right_after, outer_right_before) << "the outer aligned divider must not move";
  EXPECT_EQ(target_after.columns, target_before.columns + 1U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(PaneLayoutTest, BatchedResizeMatchesRepeatedCellResizes) {
  PaneLayout batched(pane(0));
  ASSERT_TRUE(batched.split(pane(0), pane(1), SplitAxis::left_right));
  auto repeated = batched;
  constexpr PaneRectangle viewport{.columns = 100, .rows = 24};

  ASSERT_EQ(batched.resize(pane(0), ResizeDirection::right, viewport, 10),
            LayoutResizeStatus::applied);
  for (std::size_t step = 0; step < 10; ++step) {
    ASSERT_EQ(repeated.resize(pane(0), ResizeDirection::right, viewport),
              LayoutResizeStatus::applied);
  }

  EXPECT_EQ(batched, repeated);
  EXPECT_EQ(rectangle(batched, pane(0), viewport).columns,
            rectangle(repeated, pane(0), viewport).columns);
}

TEST(PaneLayoutTest, RemovePromotesSiblingWithoutInvalidatingRetainedTopology) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  ASSERT_TRUE(layout.split(pane(1), pane(2), SplitAxis::top_bottom));
  constexpr PaneRectangle viewport{.columns = 80, .rows = 23};
  ASSERT_EQ(layout.resize(pane(2), ResizeDirection::up, viewport), LayoutResizeStatus::applied);

  const auto focus_candidate = layout.remove(pane(0));

  ASSERT_TRUE(focus_candidate.has_value());
  EXPECT_TRUE(layout.valid());
  EXPECT_EQ(layout.pane_count(), 2U);
  EXPECT_FALSE(layout.contains(pane(0)));
  EXPECT_TRUE(layout.contains(pane(1)));
  EXPECT_TRUE(layout.contains(pane(2)));
  const auto projection = layout.project(viewport);
  ASSERT_TRUE(projection.has_value());
  EXPECT_EQ(projection.value_or(LayoutProjection{}).pane_count, 2U);
}

TEST(PaneLayoutTest, SwapExchangesOnlyLeafIdentity) {
  PaneLayout layout(pane(0));
  ASSERT_TRUE(layout.split(pane(0), pane(1), SplitAxis::left_right));
  ASSERT_TRUE(layout.split(pane(1), pane(2), SplitAxis::top_bottom));
  constexpr PaneRectangle viewport{.columns = 80, .rows = 23};
  ASSERT_EQ(layout.resize(pane(2), ResizeDirection::up, viewport), LayoutResizeStatus::applied);
  const auto first_before = rectangle(layout, pane(0), viewport);
  const auto second_before = rectangle(layout, pane(2), viewport);

  ASSERT_TRUE(layout.swap(pane(0), pane(2)));

  EXPECT_TRUE(layout.valid());
  EXPECT_EQ(rectangle(layout, pane(0), viewport), second_before);
  EXPECT_EQ(rectangle(layout, pane(2), viewport), first_before);
  EXPECT_FALSE(layout.swap(pane(0), pane(0)));
  EXPECT_FALSE(layout.swap(pane(0), pane(9)));
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(PaneLayoutTest, SupportsMaximumBoundedDepthAndPaneCount) {
  PaneLayout layout(pane(0));
  for (std::uint32_t slot = 1; slot < pane_layout_panes_max; ++slot) {
    ASSERT_TRUE(layout.split(pane(0), pane(slot), SplitAxis::left_right)) << slot;
  }

  EXPECT_EQ(layout.pane_count(), pane_layout_panes_max);
  EXPECT_TRUE(layout.valid());
  const auto projection = layout.project(
      {.columns = static_cast<std::uint16_t>((pane_layout_panes_max * 2U) - 1U), .rows = 1});
  ASSERT_TRUE(projection.has_value());
  EXPECT_EQ(projection.value_or(LayoutProjection{}).pane_count, pane_layout_panes_max);
}

TEST(FloatPlacementTest, FactoriesAcceptOnlyPresentablePlacements) {
  EXPECT_TRUE(FloatPlacement::absolute(0, 0, 3, 3).has_value());
  EXPECT_TRUE(FloatPlacement::absolute(997, 997, 3, 3).has_value());
  // Below the one-cell frame around one terminal cell, or unable to fit the largest viewport.
  EXPECT_FALSE(FloatPlacement::absolute(0, 0, 2, 3).has_value());
  EXPECT_FALSE(FloatPlacement::absolute(0, 0, 3, 2).has_value());
  EXPECT_FALSE(FloatPlacement::absolute(998, 0, 3, 3).has_value());
  EXPECT_FALSE(FloatPlacement::absolute(0, 998, 3, 3).has_value());
  EXPECT_FALSE(FloatPlacement::absolute(65'535, 0, 65'535, 3).has_value());

  EXPECT_TRUE(FloatPlacement::centered(3, 3).has_value());
  EXPECT_TRUE(FloatPlacement::centered(1'000, 1'000).has_value());
  EXPECT_FALSE(FloatPlacement::centered(2, 10).has_value());
  EXPECT_FALSE(FloatPlacement::centered(1'001, 10).has_value());

  EXPECT_TRUE(FloatPlacement::relative(1, 100).has_value());
  EXPECT_FALSE(FloatPlacement::relative(0, 50).has_value());
  EXPECT_FALSE(FloatPlacement::relative(50, 101).has_value());

  const auto placement = FloatPlacement::absolute(4, 5, 6, 7);
  ASSERT_TRUE(placement.has_value());
  EXPECT_EQ(checked_placement(placement).kind(), FloatPlacementKind::absolute);
  EXPECT_EQ(checked_placement(placement).column(), 4U);
  EXPECT_EQ(checked_placement(placement).row(), 5U);
  EXPECT_EQ(checked_placement(placement).columns(), 6U);
  EXPECT_EQ(checked_placement(placement).rows(), 7U);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(FloatPlacementTest, ResolvesInsideTheViewportOrSuspendsWithoutClipping) {
  // A docked Tab viewport does not start at the origin.
  constexpr PaneRectangle viewport{.column = 2, .row = 1, .columns = 80, .rows = 23};
  const auto absolute = FloatPlacement::absolute(10, 3, 30, 10);
  ASSERT_TRUE(absolute.has_value());
  EXPECT_EQ(checked_placement(absolute).resolve(viewport),
            (PaneRectangle{.column = 12, .row = 4, .columns = 30, .rows = 10}));
  // Exactly filling the viewport fits; one more cell suspends rather than clips or moves.
  EXPECT_TRUE(
      checked_placement(FloatPlacement::absolute(50, 13, 30, 10)).resolve(viewport).has_value());
  EXPECT_FALSE(
      checked_placement(FloatPlacement::absolute(51, 13, 30, 10)).resolve(viewport).has_value());
  EXPECT_FALSE(
      checked_placement(FloatPlacement::absolute(50, 14, 30, 10)).resolve(viewport).has_value());

  // Centering rounds the odd cell toward the origin.
  EXPECT_EQ(checked_placement(FloatPlacement::centered(31, 10)).resolve(viewport),
            (PaneRectangle{.column = 26, .row = 7, .columns = 31, .rows = 10}));
  EXPECT_EQ(checked_placement(FloatPlacement::centered(80, 23)).resolve(viewport), viewport);
  EXPECT_FALSE(checked_placement(FloatPlacement::centered(81, 23)).resolve(viewport).has_value());

  // Relative extents round to the nearest cell, never below the minimum or above the viewport.
  EXPECT_EQ(checked_placement(FloatPlacement::relative(50, 50)).resolve(viewport),
            (PaneRectangle{.column = 22, .row = 6, .columns = 40, .rows = 12}));
  EXPECT_EQ(checked_placement(FloatPlacement::relative(1, 1)).resolve(viewport),
            (PaneRectangle{.column = 40, .row = 11, .columns = 3, .rows = 3}));
  EXPECT_EQ(checked_placement(FloatPlacement::relative(100, 100)).resolve(viewport), viewport);
  EXPECT_FALSE(checked_placement(FloatPlacement::relative(100, 100))
                   .resolve({.columns = 2, .rows = 23})
                   .has_value());
  EXPECT_EQ(
      checked_placement(FloatPlacement::relative(100, 100)).resolve({.columns = 3, .rows = 3}),
      (PaneRectangle{.columns = 3, .rows = 3}));

  // The Pane inside the native frame keeps at least one cell.
  EXPECT_EQ(float_inner_rectangle({.column = 12, .row = 4, .columns = 30, .rows = 10}),
            (PaneRectangle{.column = 13, .row = 5, .columns = 28, .rows = 8}));
  EXPECT_EQ(float_inner_rectangle({.column = 0, .row = 0, .columns = 3, .rows = 3}),
            (PaneRectangle{.column = 1, .row = 1, .columns = 1, .rows = 1}));
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(FloatLayerTest, OrdersBackToFrontWithinFixedCapacity) {
  static_assert(std::is_trivially_copyable_v<FloatLayer>);
  const auto small = checked_placement(FloatPlacement::centered(10, 5));
  const auto large = checked_placement(FloatPlacement::relative(80, 80));
  FloatLayer layer;
  EXPECT_TRUE(layer.empty());
  EXPECT_FALSE(layer.top().has_value());
  EXPECT_FALSE(layer.push(PaneId{}, small));
  for (std::uint32_t slot = 0; slot < floats_per_tab_max; ++slot) {
    ASSERT_TRUE(layer.push(pane(slot), small)) << slot;
  }
  EXPECT_FALSE(layer.push(pane(40), small));
  EXPECT_FALSE(layer.push(pane(0), small));
  EXPECT_EQ(layer.size(), floats_per_tab_max);
  EXPECT_EQ(layer.top(), pane(floats_per_tab_max - 1U));

  // Raising preserves the relative order of the others.
  ASSERT_TRUE(layer.raise(pane(2)));
  EXPECT_EQ(layer.top(), pane(2));
  EXPECT_EQ(layer.z(pane(3)), 2U);
  EXPECT_EQ(layer.z(pane(2)), floats_per_tab_max - 1U);
  EXPECT_FALSE(layer.raise(pane(40)));

  ASSERT_TRUE(layer.place(pane(3), large));
  EXPECT_EQ(layer.placement(pane(3)), large);
  EXPECT_FALSE(layer.place(pane(40), large));

  // Erasing compacts the order and clears vacated storage, so equality compares live state only.
  const auto staged = layer;
  ASSERT_TRUE(layer.erase(pane(2)));
  EXPECT_FALSE(layer.contains(pane(2)));
  EXPECT_FALSE(layer.erase(pane(2)));
  EXPECT_EQ(layer.top(), pane(floats_per_tab_max - 1U));
  EXPECT_EQ(layer.size(), floats_per_tab_max - 1U);
  EXPECT_NE(layer, staged);
  ASSERT_TRUE(layer.push(pane(2), small));
  ASSERT_TRUE(layer.place(pane(3), large));
  EXPECT_EQ(layer, staged);
  for (std::uint32_t slot = 0; slot < floats_per_tab_max; ++slot) {
    ASSERT_TRUE(layer.erase(pane(slot)));
  }
  EXPECT_EQ(layer, FloatLayer{});
}

} // namespace
} // namespace lemma::core
