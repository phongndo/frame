#include "model.hpp"
#include "random.hpp"
#include "trace.hpp"

#include "core/float_layer.hpp"
#include "core/layout.hpp"
#include "core/session.hpp"
#include "lemma/command.hpp"
#include "lemma/generational_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lemma::test::sim {
namespace {

struct PaneToken final {
  std::uint64_t serial{0};
};

struct TabToken final {
  std::uint64_t serial{0};
};

[[nodiscard]] constexpr auto pane_id(const std::uint32_t slot,
                                     const std::uint32_t generation = 1) noexcept -> PaneId {
  return PaneId::from_parts(slot, generation);
}

[[nodiscard]] auto projected_rectangle(const core::PaneLayout& layout, const PaneId pane,
                                       const PaneRectangle viewport) -> PaneRectangle {
  const auto projection = layout.project(viewport);
  EXPECT_TRUE(projection.has_value());
  const auto rectangle = projection.has_value() ? projection->rectangle(pane) : std::nullopt;
  EXPECT_TRUE(rectangle.has_value());
  return rectangle.value_or(PaneRectangle{});
}

// Independent float placement rules: validity at construction and resolution in a viewport.
struct PlacementModel final {
  core::FloatPlacementKind kind{core::FloatPlacementKind::centered};
  std::uint16_t column{0};
  std::uint16_t row{0};
  std::uint16_t columns{0};
  std::uint16_t rows{0};

  [[nodiscard]] auto valid() const noexcept -> bool {
    constexpr std::uint32_t maximum = 1'000;
    switch (kind) {
    case core::FloatPlacementKind::absolute:
      return columns >= 3U && rows >= 3U && std::uint32_t{column} + columns <= maximum &&
             std::uint32_t{row} + rows <= maximum;
    case core::FloatPlacementKind::centered:
      return columns >= 3U && rows >= 3U && columns <= maximum && rows <= maximum;
    case core::FloatPlacementKind::relative:
      return columns >= 1U && columns <= 100U && rows >= 1U && rows <= 100U;
    }
    return false;
  }

  [[nodiscard]] auto build() const noexcept -> std::optional<core::FloatPlacement> {
    switch (kind) {
    case core::FloatPlacementKind::absolute:
      return core::FloatPlacement::absolute(column, row, columns, rows);
    case core::FloatPlacementKind::centered:
      return core::FloatPlacement::centered(columns, rows);
    case core::FloatPlacementKind::relative:
      return core::FloatPlacement::relative(columns, rows);
    }
    return std::nullopt;
  }

  [[nodiscard]] static auto percent(const std::uint16_t available, const std::uint16_t share)
      -> std::uint16_t {
    // Round half up to whole cells, then keep the minimum and the viewport bound.
    const auto cells =
        static_cast<std::uint16_t>(((std::uint32_t{available} * share) + 50U) / 100U);
    return std::min(available, std::max<std::uint16_t>(cells, 3U));
  }

  [[nodiscard]] auto resolve(const PaneRectangle viewport) const -> std::optional<PaneRectangle> {
    auto width = columns;
    auto height = rows;
    if (kind == core::FloatPlacementKind::relative) {
      if (viewport.columns < 3U || viewport.rows < 3U) {
        return std::nullopt;
      }
      width = percent(viewport.columns, columns);
      height = percent(viewport.rows, rows);
    }
    if (kind == core::FloatPlacementKind::absolute) {
      if (std::uint32_t{column} + width > viewport.columns ||
          std::uint32_t{row} + height > viewport.rows) {
        return std::nullopt;
      }
      return PaneRectangle{.column = static_cast<std::uint16_t>(viewport.column + column),
                           .row = static_cast<std::uint16_t>(viewport.row + row),
                           .columns = width,
                           .rows = height};
    }
    if (width > viewport.columns || height > viewport.rows) {
      return std::nullopt;
    }
    return PaneRectangle{
        .column = static_cast<std::uint16_t>(viewport.column + ((viewport.columns - width) / 2U)),
        .row = static_cast<std::uint16_t>(viewport.row + ((viewport.rows - height) / 2U)),
        .columns = width,
        .rows = height};
  }
};

template <typename Id, std::size_t Capacity> class StaleIds final {
public:
  void retain(const Id id) noexcept {
    std::span(ids_).subspan(next_, 1).front() = id;
    next_ = (next_ + 1U) % ids_.size();
    count_ = std::min(count_ + 1U, ids_.size());
  }

  [[nodiscard]] constexpr auto empty() const noexcept -> bool { return count_ == 0; }
  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return count_; }
  [[nodiscard]] auto at(const std::size_t index) const noexcept -> Id {
    return std::span(ids_).subspan(index, 1).front();
  }

private:
  std::array<Id, Capacity> ids_{};
  std::size_t next_{0};
  std::size_t count_{0};
};

class CoreWorld final {
public:
  CoreWorld() {
    const auto first_pane =
        panes_.insert(std::make_unique<PaneToken>(PaneToken{.serial = next_serial_++}));
    const auto first_tab =
        tabs_.insert(std::make_unique<TabToken>(TabToken{.serial = next_serial_++}));
    if (!first_pane.has_value() || !first_tab.has_value()) {
      std::abort();
    }
    layout_ = std::make_unique<core::PaneLayout>(*first_pane);
    layout_model_ = std::make_unique<LayoutModel>(*first_pane);
    if (!tab_order_.append(*first_tab) || !tab_model_.append(*first_tab)) {
      std::abort();
    }
  }

  [[nodiscard]] auto apply(Random& random, Operation& operation) -> std::optional<std::string> {
    switch (random.index(16)) {
    case 0:
      return split_pane(random, operation);
    case 1:
      return remove_pane(random, operation);
    case 2:
      return swap_panes(random, operation);
    case 3:
      return resize_pane(random, operation);
    case 4:
      return resize_divider(random, operation);
    case 5:
      return invalidate_divider(random, operation);
    case 6:
      return change_viewport(random, operation);
    case 7:
      return probe_stale_pane(random, operation);
    case 8:
      return append_tab(random, operation);
    case 9:
      return erase_tab(random, operation);
    case 10:
      return place_tab(random, operation);
    case 11:
      return probe_stale_tab(random, operation);
    case 12:
      return push_float(random, operation);
    case 13:
      return erase_float(random, operation);
    case 14:
      return raise_float(random, operation);
    case 15:
      return place_float(random, operation);
    default:
      break;
    }
    return std::string{"generator selected an unknown operation"};
  }

  [[nodiscard]] auto validate() const -> std::optional<std::string> {
    if (const auto error = validate_layout_and_ids(); error.has_value()) {
      return error;
    }
    if (const auto error = validate_projection(); error.has_value()) {
      return error;
    }
    if (const auto error = validate_floats(); error.has_value()) {
      return error;
    }
    return validate_tab_order();
  }

  [[nodiscard]] auto state_hash() const noexcept -> std::uint64_t {
    auto hash = layout_model_->hash();
    hash = hash_mix(hash, tab_model_.hash());
    hash = hash_mix(hash, viewport_.columns);
    for (const auto& entry : float_model_) {
      hash = hash_mix(hash, (static_cast<std::uint64_t>(entry.first.generation()) << 32U) |
                                entry.first.slot());
      hash = hash_mix(hash, (std::uint64_t{entry.second.columns} << 16U) | entry.second.rows);
    }
    return hash_mix(hash, viewport_.rows);
  }

private:
  [[nodiscard]] auto validate_layout_and_ids() const -> std::optional<std::string> {
    if (!layout_->valid() || layout_->pane_count() != layout_model_->size() ||
        panes_.size() != layout_model_->size()) {
      return std::string{"layout/store/model pane counts diverged"};
    }
    const auto snapshot = layout_->snapshot();
    if (!snapshot.has_value() || !layout_model_->matches(*snapshot)) {
      return std::string{"production layout topology differs from the reference model"};
    }
    for (const auto pane : layout_model_->panes()) {
      if (!layout_->contains(pane) || panes_.get(pane) == nullptr) {
        return std::string{"a modeled live Pane is absent from production state"};
      }
    }
    for (std::size_t index = 0; index < stale_panes_.size(); ++index) {
      const auto stale = stale_panes_.at(index);
      if (layout_->contains(stale) || panes_.get(stale) != nullptr) {
        return std::string{"a stale Pane ID resolved after slot reuse"};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static auto valid_rectangle(const PaneRectangle rectangle,
                                            const PaneRectangle viewport) noexcept -> bool {
    return rectangle.columns > 0 && rectangle.rows > 0 &&
           static_cast<std::uint32_t>(rectangle.column) + rectangle.columns <= viewport.columns &&
           static_cast<std::uint32_t>(rectangle.row) + rectangle.rows <= viewport.rows;
  }

  [[nodiscard]] static auto separated(const PaneRectangle first,
                                      const PaneRectangle second) noexcept -> bool {
    return static_cast<std::uint32_t>(first.column) + first.columns <= second.column ||
           static_cast<std::uint32_t>(second.column) + second.columns <= first.column ||
           static_cast<std::uint32_t>(first.row) + first.rows <= second.row ||
           static_cast<std::uint32_t>(second.row) + second.rows <= first.row;
  }

  [[nodiscard]] auto validate_live_rectangles(const core::LayoutProjection& projection,
                                              const std::vector<PaneId>& live_panes) const
      -> std::optional<std::string> {
    for (std::size_t index = 0; index < live_panes.size(); ++index) {
      const auto first = projection.rectangle(live_panes.at(index));
      if (!first.has_value() || !valid_rectangle(*first, viewport_)) {
        return std::string{"projected Pane rectangle is missing, empty, or out of bounds"};
      }
      for (std::size_t other = index + 1U; other < live_panes.size(); ++other) {
        const auto second = projection.rectangle(live_panes.at(other));
        if (!second.has_value() || !separated(*first, *second)) {
          return std::string{"projected Pane rectangles are missing or overlap"};
        }
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto validate_projection() const -> std::optional<std::string> {
    const auto minimum = layout_model_->minimum();
    const bool expected = viewport_.columns >= minimum.columns && viewport_.rows >= minimum.rows;
    const auto projection = layout_->project(viewport_);
    if (projection.has_value() != expected) {
      return std::string{"projection availability differs from independent minimum extents"};
    }
    if (!projection.has_value()) {
      return std::nullopt;
    }
    const auto live_panes = layout_model_->panes();
    if (projection->pane_count != live_panes.size()) {
      return std::string{"projection Pane count differs from the model"};
    }
    if (const auto error = validate_live_rectangles(*projection, live_panes); error.has_value()) {
      return error;
    }
    for (std::size_t index = 0; index < stale_panes_.size(); ++index) {
      if (projection->rectangle(stale_panes_.at(index)).has_value()) {
        return std::string{"a stale Pane ID appeared in a projection"};
      }
    }
    return std::nullopt;
  }

  // The production layer matches the back-to-front model, and every resolved placement equals the
  // independent resolution: inside the viewport with a nonempty inner Pane, or suspended.
  [[nodiscard]] auto validate_floats() const -> std::optional<std::string> {
    const auto entries = floats_.entries();
    if (entries.size() != float_model_.size() ||
        floats_.top() !=
            (float_model_.empty() ? std::nullopt : std::optional{float_model_.back().first})) {
      return std::string{"float layer size or top differs from the model"};
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const auto& entry = entries.subspan(index, 1).front();
      const auto& [pane, model] = float_model_.at(index);
      if (entry.pane != pane || floats_.z(pane) != index || !floats_.contains(pane) ||
          model.build() != entry.placement) {
        return std::string{"float layer order or placement differs from the model"};
      }
      if (const auto error = validate_float_geometry(entry.placement, model); error.has_value()) {
        return error;
      }
    }
    for (std::size_t index = 0; index < stale_floats_.size(); ++index) {
      if (floats_.contains(stale_floats_.at(index))) {
        return std::string{"an erased float remained in the layer"};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto validate_float_geometry(const core::FloatPlacement placement,
                                             const PlacementModel& model) const
      -> std::optional<std::string> {
    const auto resolved = placement.resolve(viewport_);
    if (resolved != model.resolve(viewport_)) {
      return std::string{"float placement resolution differs from the model"};
    }
    if (resolved.has_value()) {
      const auto inner = core::float_inner_rectangle(*resolved);
      if (!valid_rectangle(*resolved, viewport_) || inner.columns == 0 || inner.rows == 0) {
        return std::string{"resolved float is outside the viewport or has no Pane cell"};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static auto random_placement(Random& random) -> PlacementModel {
    constexpr std::array<std::uint16_t, 7> extents{0, 2, 3, 4, 60, 999, 1'000};
    constexpr std::array<std::uint16_t, 6> offsets{0, 1, 17, 996, 997, 998};
    constexpr std::array<std::uint16_t, 6> percents{0, 1, 33, 50, 100, 101};
    PlacementModel model;
    switch (random.index(3)) {
    case 0:
      model.kind = core::FloatPlacementKind::absolute;
      model.column = offsets.at(random.index(offsets.size()));
      model.row = offsets.at(random.index(offsets.size()));
      model.columns = extents.at(random.index(extents.size()));
      model.rows = extents.at(random.index(extents.size()));
      break;
    case 1:
      model.kind = core::FloatPlacementKind::centered;
      model.columns =
          random.boolean() ? extents.at(random.index(extents.size())) : random.between(3, 120);
      model.rows =
          random.boolean() ? extents.at(random.index(extents.size())) : random.between(3, 80);
      break;
    default:
      model.kind = core::FloatPlacementKind::relative;
      model.columns = percents.at(random.index(percents.size()));
      model.rows = percents.at(random.index(percents.size()));
      break;
    }
    return model;
  }

  [[nodiscard]] auto random_float(Random& random) const -> PaneId {
    if (float_model_.empty() || random.index(8) == 0) {
      return stale_floats_.empty() ? PaneId{}
                                   : stale_floats_.at(random.index(stale_floats_.size()));
    }
    return float_model_.at(random.index(float_model_.size())).first;
  }

  [[nodiscard]] auto model_index(const PaneId pane) const -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < float_model_.size(); ++index) {
      if (float_model_.at(index).first == pane) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto push_float(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::float_push;
    const auto model = random_placement(random);
    const auto placement = model.build();
    operation.argument_0 = model.columns;
    operation.argument_1 = model.rows;
    if (placement.has_value() != model.valid()) {
      return std::string{"float placement validity differs from the model"};
    }
    if (!placement.has_value()) {
      return std::nullopt;
    }
    const auto added =
        float_ids_.insert(std::make_unique<PaneToken>(PaneToken{.serial = next_serial_++}));
    if (!added.has_value()) {
      return std::string{"float ID store exhausted before its bound"};
    }
    operation.pane = *added;
    const auto before = floats_;
    const bool actual = floats_.push(*added, *placement);
    const bool expected = float_model_.size() < core::floats_per_tab_max;
    operation.result = static_cast<std::int32_t>(actual);
    if (actual != expected || (!actual && floats_ != before)) {
      return std::string{"float push outcome differs from the capacity model"};
    }
    if (actual) {
      float_model_.emplace_back(*added, model);
    } else if (!float_ids_.erase(*added)) {
      return std::string{"rejected float could not release its ID"};
    }
    return std::nullopt;
  }

  [[nodiscard]] auto erase_float(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::float_erase;
    operation.pane = random_float(random);
    const auto index = model_index(operation.pane);
    const auto before = floats_;
    const bool actual = floats_.erase(operation.pane);
    operation.result = static_cast<std::int32_t>(actual);
    if (actual != index.has_value() || (!actual && floats_ != before)) {
      return std::string{"float erase outcome differs from the model"};
    }
    if (actual) {
      float_model_.erase(float_model_.begin() + static_cast<std::ptrdiff_t>(*index));
      if (!float_ids_.erase(operation.pane)) {
        return std::string{"erased float ID remained live"};
      }
      stale_floats_.retain(operation.pane);
    }
    return std::nullopt;
  }

  [[nodiscard]] auto raise_float(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::float_raise;
    operation.pane = random_float(random);
    const auto index = model_index(operation.pane);
    const bool actual = floats_.raise(operation.pane);
    operation.result = static_cast<std::int32_t>(actual);
    if (actual != index.has_value()) {
      return std::string{"float raise outcome differs from the model"};
    }
    if (actual) {
      const auto raised = float_model_.at(*index);
      float_model_.erase(float_model_.begin() + static_cast<std::ptrdiff_t>(*index));
      float_model_.push_back(raised);
    }
    return std::nullopt;
  }

  [[nodiscard]] auto place_float(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::float_place;
    operation.pane = random_float(random);
    const auto model = random_placement(random);
    const auto placement = model.build();
    operation.argument_0 = model.columns;
    operation.argument_1 = model.rows;
    if (placement.has_value() != model.valid()) {
      return std::string{"float placement validity differs from the model"};
    }
    if (!placement.has_value()) {
      return std::nullopt;
    }
    const auto index = model_index(operation.pane);
    const bool actual = floats_.place(operation.pane, *placement);
    operation.result = static_cast<std::int32_t>(actual);
    if (actual != index.has_value()) {
      return std::string{"float place outcome differs from the model"};
    }
    if (actual) {
      float_model_.at(*index).second = model;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto validate_tab_order() const -> std::optional<std::string> {
    if (tab_order_.size() != tab_model_.size() || tabs_.size() != tab_model_.size()) {
      return std::string{"Tab order/store/model counts diverged"};
    }
    for (std::size_t index = 0; index < tab_model_.size(); ++index) {
      const auto tab = tab_model_.at(index);
      if (tab_order_.at(index) != tab || tab_order_.position_of(tab) != index ||
          tabs_.get(tab) == nullptr) {
        return std::string{"Tab order differs from the reference permutation"};
      }
    }
    for (std::size_t index = 0; index < stale_tabs_.size(); ++index) {
      const auto stale = stale_tabs_.at(index);
      if (tab_order_.position_of(stale).has_value() || tabs_.get(stale) != nullptr) {
        return std::string{"a stale Tab ID resolved after slot reuse"};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto random_pane(Random& random) const -> PaneId {
    const auto panes = layout_model_->panes();
    return panes.at(random.index(panes.size()));
  }

  [[nodiscard]] auto random_tab(Random& random) const -> TabId {
    return tab_model_.at(random.index(tab_model_.size()));
  }

  [[nodiscard]] auto split_pane(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::pane_split;
    operation.pane = random_pane(random);
    operation.argument_0 = static_cast<std::uint16_t>(
        random.boolean() ? core::SplitAxis::left_right : core::SplitAxis::top_bottom);

    if (layout_model_->size() == core::panes_per_session_max) {
      operation.peer_pane = operation.pane;
      const auto before = *layout_;
      const bool actual = layout_->split(operation.pane, operation.peer_pane,
                                         static_cast<core::SplitAxis>(operation.argument_0));
      operation.result = static_cast<std::int32_t>(actual);
      if (actual || *layout_ != before) {
        return std::string{"capacity/duplicate split mutated the production layout"};
      }
      return std::nullopt;
    }

    const auto added =
        panes_.insert(std::make_unique<PaneToken>(PaneToken{.serial = next_serial_++}));
    if (!added.has_value()) {
      return std::string{"Pane store exhausted before the modeled capacity"};
    }
    operation.peer_pane = *added;
    const auto axis = static_cast<core::SplitAxis>(operation.argument_0);
    const bool actual = layout_->split(operation.pane, *added, axis);
    const bool expected = layout_model_->split(operation.pane, *added, axis);
    operation.result = static_cast<std::int32_t>(actual);
    if (actual != expected) {
      return std::string{"split outcome differs from the topology model"};
    }
    if (!actual) {
      if (!panes_.erase(*added)) {
        return std::string{"failed split could not release its staged Pane ID"};
      }
      stale_panes_.retain(*added);
    }
    return std::nullopt;
  }

  [[nodiscard]] auto remove_pane(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::pane_remove;
    operation.pane = random_pane(random);
    return remove_pane_id(operation.pane, operation);
  }

  [[nodiscard]] auto remove_pane_id(const PaneId pane, Operation& operation)
      -> std::optional<std::string> {
    const auto before = *layout_;
    const auto actual = layout_->remove(pane);
    const auto expected = layout_model_->remove(pane);
    operation.peer_pane = actual.value_or(PaneId{});
    operation.result = static_cast<std::int32_t>(actual.has_value());
    if (actual != expected) {
      return std::string{"remove outcome/focus candidate differs from the topology model"};
    }
    if (!actual.has_value()) {
      if (*layout_ != before) {
        return std::string{"rejected remove mutated the production layout"};
      }
      return std::nullopt;
    }
    if (!panes_.erase(pane)) {
      return std::string{"removed Pane remained live in the generational store"};
    }
    stale_panes_.retain(pane);
    return std::nullopt;
  }

  [[nodiscard]] auto swap_panes(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::pane_swap;
    operation.pane = random_pane(random);
    operation.peer_pane = random_pane(random);
    const auto before = *layout_;
    const bool actual = layout_->swap(operation.pane, operation.peer_pane);
    const bool expected = layout_model_->swap(operation.pane, operation.peer_pane);
    operation.result = static_cast<std::int32_t>(actual);
    if (actual != expected) {
      return std::string{"swap outcome differs from the topology model"};
    }
    if (!actual && *layout_ != before) {
      return std::string{"rejected swap mutated the production layout"};
    }
    return std::nullopt;
  }

  [[nodiscard]] auto resize_pane(Random& random, Operation& operation)
      -> std::optional<std::string> {
    constexpr std::array directions{
        core::ResizeDirection::left,
        core::ResizeDirection::right,
        core::ResizeDirection::up,
        core::ResizeDirection::down,
    };
    operation.kind = OperationKind::pane_resize;
    operation.pane = random_pane(random);
    operation.argument_0 =
        static_cast<std::uint16_t>(directions.at(random.index(directions.size())));
    operation.argument_1 = random.between(1, command_resize_amount_max);
    const auto before = *layout_;
    const auto previous_projection = before.project(viewport_);
    const auto status =
        layout_->resize(operation.pane, static_cast<core::ResizeDirection>(operation.argument_0),
                        viewport_, operation.argument_1);
    operation.result = static_cast<std::int32_t>(status);
    return validate_resize(before, previous_projection, status);
  }

  [[nodiscard]] auto resize_divider(Random& random, Operation& operation)
      -> std::optional<std::string> {
    const auto dividers = layout_model_->dividers();
    if (dividers.empty()) {
      return resize_pane(random, operation);
    }
    const auto divider = dividers.at(random.index(dividers.size()));
    operation.kind = OperationKind::pane_resize_divider;
    operation.pane = divider.first;
    operation.peer_pane = divider.second;
    operation.argument_0 = static_cast<std::uint16_t>(divider.axis);
    const auto maximum = divider.axis == core::SplitAxis::left_right
                             ? static_cast<std::uint16_t>(viewport_.columns + 20U)
                             : static_cast<std::uint16_t>(viewport_.rows + 20U);
    operation.argument_1 = random.between(0, maximum);
    const auto before = *layout_;
    const auto previous_projection = before.project(viewport_);
    const auto status = layout_->resize_divider(
        {.first = divider.first, .second = divider.second, .axis = divider.axis},
        operation.argument_1, viewport_);
    operation.result = static_cast<std::int32_t>(status);
    return validate_resize(before, previous_projection, status);
  }

  [[nodiscard]] auto validate_resize(const core::PaneLayout& before,
                                     const std::optional<core::LayoutProjection>& before_projection,
                                     const core::LayoutResizeStatus status) const
      -> std::optional<std::string> {
    if (status == core::LayoutResizeStatus::invalid) {
      return std::string{"valid generated resize returned invalid"};
    }
    if (status != core::LayoutResizeStatus::applied) {
      return *layout_ == before
                 ? std::nullopt
                 : std::optional<std::string>{"non-applied resize mutated the production layout"};
    }
    const auto after_projection = layout_->project(viewport_);
    if (!before_projection.has_value() || !after_projection.has_value() ||
        *before_projection == *after_projection) {
      return std::string{"applied resize did not change the projected geometry"};
    }
    return std::nullopt;
  }

  [[nodiscard]] auto invalidate_divider(Random& random, Operation& operation)
      -> std::optional<std::string> {
    const auto dividers = layout_model_->dividers();
    if (dividers.empty()) {
      return probe_stale_pane(random, operation);
    }
    const auto divider = dividers.at(random.index(dividers.size()));
    operation.kind = OperationKind::pane_invalidate_divider;
    operation.pane = divider.first;
    operation.peer_pane = divider.second;
    operation.argument_0 = static_cast<std::uint16_t>(divider.axis);
    operation.argument_1 = random.between(0, std::max(viewport_.columns, viewport_.rows));
    Operation removal;
    const auto removed = random.boolean() ? divider.first : divider.second;
    operation.other_pane = removed;
    if (const auto error = remove_pane_id(removed, removal); error.has_value()) {
      return error;
    }
    if (removal.result == 0) {
      return std::string{"generated divider representative was not removable"};
    }
    const auto before_probe = *layout_;
    const auto status = layout_->resize_divider(
        {.first = divider.first, .second = divider.second, .axis = divider.axis},
        operation.argument_1, viewport_);
    operation.result = static_cast<std::int32_t>(status);
    if (status != core::LayoutResizeStatus::invalid || *layout_ != before_probe) {
      return std::string{"stale divider retargeted or mutated the layout"};
    }
    return std::nullopt;
  }

  [[nodiscard]] auto change_viewport(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::pane_change_viewport;
    operation.argument_0 = random.between(1, 200);
    operation.argument_1 = random.between(1, 120);
    viewport_ = {.columns = operation.argument_0, .rows = operation.argument_1};
    operation.result = 1;
    return std::nullopt;
  }

  [[nodiscard]] auto probe_stale_pane(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::pane_probe_stale;
    operation.pane =
        stale_panes_.empty() ? PaneId{} : stale_panes_.at(random.index(stale_panes_.size()));
    operation.peer_pane = random_pane(random);
    const auto before = *layout_;
    const auto projection = layout_->project(viewport_);
    const bool rejected =
        !layout_->contains(operation.pane) && panes_.get(operation.pane) == nullptr &&
        !layout_->remove(operation.pane).has_value() &&
        !layout_->swap(operation.pane, operation.peer_pane) &&
        (!projection.has_value() || !projection->rectangle(operation.pane).has_value());
    operation.result = static_cast<std::int32_t>(rejected);
    if (!rejected || *layout_ != before) {
      return std::string{"stale Pane probe resolved or mutated production state"};
    }
    return std::nullopt;
  }

  [[nodiscard]] auto append_tab(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::tab_append;
    if (tab_model_.size() == core::tabs_per_session_max) {
      operation.tab = random_tab(random);
      const bool actual = tab_order_.append(operation.tab);
      const bool expected = tab_model_.append(operation.tab);
      operation.result = static_cast<std::int32_t>(actual);
      return actual == expected ? std::nullopt
                                : std::optional<std::string>{"duplicate Tab append diverged"};
    }
    const auto tab = tabs_.insert(std::make_unique<TabToken>(TabToken{.serial = next_serial_++}));
    if (!tab.has_value()) {
      return std::string{"Tab store exhausted before the modeled capacity"};
    }
    operation.tab = *tab;
    const bool actual = tab_order_.append(*tab);
    const bool expected = tab_model_.append(*tab);
    operation.result = static_cast<std::int32_t>(actual);
    if (actual != expected) {
      return std::string{"Tab append outcome differs from the permutation model"};
    }
    return std::nullopt;
  }

  [[nodiscard]] auto erase_tab(Random& random, Operation& operation) -> std::optional<std::string> {
    operation.kind = OperationKind::tab_erase;
    operation.tab = random_tab(random);
    if (tab_model_.size() == 1U) {
      operation.tab =
          stale_tabs_.empty() ? TabId{} : stale_tabs_.at(random.index(stale_tabs_.size()));
      const bool actual = tab_order_.erase(operation.tab);
      const bool expected = tab_model_.erase(operation.tab);
      operation.result = static_cast<std::int32_t>(actual);
      return actual == expected ? std::nullopt
                                : std::optional<std::string>{"rejected Tab erase diverged"};
    }
    const bool actual = tab_order_.erase(operation.tab);
    const bool expected = tab_model_.erase(operation.tab);
    operation.result = static_cast<std::int32_t>(actual);
    if (actual != expected) {
      return std::string{"Tab erase outcome differs from the permutation model"};
    }
    if (!tabs_.erase(operation.tab)) {
      return std::string{"erased Tab remained live in the generational store"};
    }
    stale_tabs_.retain(operation.tab);
    return std::nullopt;
  }

  [[nodiscard]] auto place_tab(Random& random, Operation& operation) -> std::optional<std::string> {
    operation.kind = OperationKind::tab_place;
    operation.tab = random_tab(random);
    std::optional<TabId> anchor;
    switch (random.index(4)) {
    case 0:
      anchor = std::nullopt;
      break;
    case 1:
      anchor = operation.tab;
      break;
    default:
      anchor = random_tab(random);
      break;
    }
    operation.anchor_tab = anchor.value_or(TabId{});
    const bool actual = tab_order_.place_before(operation.tab, anchor);
    const bool expected = tab_model_.place_before(operation.tab, anchor);
    operation.result = static_cast<std::int32_t>(actual);
    return actual == expected ? std::nullopt
                              : std::optional<std::string>{
                                    "Tab placement outcome differs from the permutation model"};
  }

  [[nodiscard]] auto probe_stale_tab(Random& random, Operation& operation)
      -> std::optional<std::string> {
    operation.kind = OperationKind::tab_probe_stale;
    operation.tab =
        stale_tabs_.empty() ? TabId{} : stale_tabs_.at(random.index(stale_tabs_.size()));
    operation.anchor_tab = random_tab(random);
    const auto before = tab_model_.tabs();
    const bool rejected = !tab_order_.erase(operation.tab) &&
                          !tab_order_.place_before(operation.tab, std::nullopt) &&
                          !tab_order_.place_before(operation.anchor_tab, operation.tab) &&
                          tabs_.get(operation.tab) == nullptr;
    operation.result = static_cast<std::int32_t>(rejected);
    if (!rejected || before != tab_model_.tabs()) {
      return std::string{"stale Tab probe resolved or mutated production state"};
    }
    return std::nullopt;
  }

  BoundedGenerationalStore<PaneToken, PaneId, core::panes_per_session_max> panes_;
  BoundedGenerationalStore<TabToken, TabId, core::tabs_per_session_max> tabs_;
  std::unique_ptr<core::PaneLayout> layout_;
  std::unique_ptr<LayoutModel> layout_model_;
  core::TabOrder tab_order_;
  TabOrderModel tab_model_;
  StaleIds<PaneId, 1'024> stale_panes_;
  StaleIds<TabId, 256> stale_tabs_;
  // Twice the layer capacity, so pushes also probe a full layer with a fresh ID.
  BoundedGenerationalStore<PaneToken, PaneId, core::floats_per_tab_max * 2U> float_ids_;
  core::FloatLayer floats_;
  std::vector<std::pair<PaneId, PlacementModel>> float_model_;
  StaleIds<PaneId, 64> stale_floats_;
  PaneRectangle viewport_{.columns = 120, .rows = 80};
  std::uint64_t next_serial_{1};
};

[[nodiscard]] auto parse_environment_integer(const char* const name, std::uint64_t& result) noexcept
    -> bool {
  const auto* const encoded = std::getenv(name);
  if (encoded == nullptr) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtoull(encoded, &end, 0);
  if (errno != 0 || end == encoded || *end != '\0') {
    return false;
  }
  result = parsed;
  return true;
}

[[nodiscard]] auto run_world(const std::uint64_t seed, const std::size_t operation_count,
                             std::uint64_t* const final_state_hash = nullptr)
    -> testing::AssertionResult {
  auto trace = std::make_unique<Trace>(seed, operation_count);
  CoreWorld world;
  Random random(seed);
  for (std::size_t index = 0; index < operation_count; ++index) {
    Operation operation;
    const auto applied = world.apply(random, operation);
    if (!trace->append(operation)) {
      return testing::AssertionFailure() << "simulation trace capacity exhausted\n" << *trace;
    }
    trace->complete_last(world.state_hash());
    if (applied.has_value()) {
      return testing::AssertionFailure() << *applied << " at operation " << index << '\n' << *trace;
    }
    if (const auto invariant = world.validate(); invariant.has_value()) {
      return testing::AssertionFailure() << *invariant << " at operation " << index << '\n'
                                         << *trace;
    }
  }
  if (final_state_hash != nullptr) {
    *final_state_hash = world.state_hash();
  }
  return testing::AssertionSuccess();
}

TEST(SimulationInfrastructureTest, SplitMix64SequenceIsStable) {
  Random random(0);
  EXPECT_EQ(random.next(), 0xE220A8397B1DCDAFULL);
  EXPECT_EQ(random.next(), 0x6E789E6AA1B965F4ULL);
  EXPECT_EQ(random.next(), 0x06C45D188009454FULL);
  EXPECT_EQ(random.next(), 0xF88BB8A8724C81ECULL);
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(CoreExhaustiveTest, FixedPointRatioRoundTripsEverySupportedOneCellBoundary) {
  constexpr std::uint16_t columns_max = 500;
  for (std::uint16_t columns = 3; columns <= columns_max; ++columns) {
    core::PaneLayout layout(pane_id(0));
    ASSERT_TRUE(layout.split(pane_id(0), pane_id(1), core::SplitAxis::left_right));
    const PaneRectangle viewport{.columns = columns, .rows = 1};
    auto previous = projected_rectangle(layout, pane_id(0), viewport).columns;
    while (layout.resize(pane_id(0), core::ResizeDirection::right, viewport) ==
           core::LayoutResizeStatus::applied) {
      const auto current = projected_rectangle(layout, pane_id(0), viewport).columns;
      ASSERT_EQ(current, previous + 1U) << "viewport columns=" << columns;
      previous = current;
    }
    EXPECT_EQ(previous, columns - 2U) << "viewport columns=" << columns;
    EXPECT_TRUE(layout.valid());
  }
}

TEST(CoreSimulationTest, SameSeedAndConfigurationReachTheSameState) {
  std::uint64_t first_hash = 0;
  std::uint64_t second_hash = 0;
  ASSERT_TRUE(run_world(0xA11CE5EEDULL, 512, &first_hash));
  ASSERT_TRUE(run_world(0xA11CE5EEDULL, 512, &second_hash));
  EXPECT_EQ(first_hash, second_hash);
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(CoreSimulationTest, GeneratedOperationsPreserveModelAndProductionInvariants) {
  constexpr std::array<std::uint64_t, 6> default_seeds{
      0ULL,
      1ULL,
      0xC0FFEEULL,
      0x51A7E123ULL,
      0xDEADBEEFCAFEBABEULL,
      std::numeric_limits<std::uint64_t>::max(),
  };
  constexpr std::size_t default_operations = 1'024;

  std::uint64_t configured_seed = 0;
  std::uint64_t configured_operations = default_operations;
  const bool has_seed = std::getenv("LEMMA_SIM_SEED") != nullptr;
  if (has_seed) {
    ASSERT_TRUE(parse_environment_integer("LEMMA_SIM_SEED", configured_seed))
        << "LEMMA_SIM_SEED must be an integer accepted by strtoull";
  }
  if (std::getenv("LEMMA_SIM_OPERATIONS") != nullptr) {
    ASSERT_TRUE(parse_environment_integer("LEMMA_SIM_OPERATIONS", configured_operations))
        << "LEMMA_SIM_OPERATIONS must be an integer accepted by strtoull";
  }
  ASSERT_GT(configured_operations, 0U);
  ASSERT_LE(configured_operations, trace_operations_max);

  if (has_seed) {
    EXPECT_TRUE(run_world(configured_seed, static_cast<std::size_t>(configured_operations)));
    return;
  }
  for (const auto seed : default_seeds) {
    SCOPED_TRACE(testing::Message() << "seed=0x" << std::hex << seed);
    EXPECT_TRUE(run_world(seed, static_cast<std::size_t>(configured_operations)));
  }
}

} // namespace
} // namespace lemma::test::sim
