#include "core/session_machine.hpp"

#include "core/layout.hpp"
#include "core/session.hpp"
#include "lemma/assert.hpp"
#include "lemma/command.hpp"
#include "lemma/geometry.hpp"
#include "lemma/id.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace lemma::core {

// Transition values intentionally rely on default member values for effects and created IDs that
// do not apply to a particular operation.
#ifdef __clang__
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif
namespace {

[[nodiscard]] constexpr auto tab_viewport(const Tab& tab) noexcept -> PaneRectangle {
  return {.column = tab.layout_column,
          .row = tab.layout_row,
          .columns = tab.layout_columns,
          .rows = tab.layout_rows};
}

[[nodiscard]] constexpr auto next_generation(const std::uint32_t generation) noexcept
    -> std::uint32_t {
  return generation == std::numeric_limits<std::uint32_t>::max() ? 0U : generation + 1U;
}

void record_mutation(Session& session) noexcept {
  session.mutation_generation =
      session.mutation_generation == std::numeric_limits<std::uint64_t>::max()
          ? std::uint64_t{1}
          : session.mutation_generation + 1U;
}

[[nodiscard]] auto find_tab(Session& session, const TabId id) noexcept -> Tab* {
  if (!id.is_valid() || id.slot() >= session.tabs.size()) {
    return nullptr;
  }
  auto& slot = std::span(session.tabs).subspan(id.slot(), 1).front();
  return slot.generation == id.generation() ? slot.tab.get() : nullptr;
}

[[nodiscard]] auto find_tab(const Session& session, const TabId id) noexcept -> const Tab* {
  if (!id.is_valid() || id.slot() >= session.tabs.size()) {
    return nullptr;
  }
  const auto& slot = std::span(session.tabs).subspan(id.slot(), 1).front();
  return slot.generation == id.generation() ? slot.tab.get() : nullptr;
}

[[nodiscard]] auto find_pane(Session& session, const PaneId id) noexcept -> Pane* {
  if (!id.is_valid() || id.slot() >= session.panes.size()) {
    return nullptr;
  }
  auto& slot = std::span(session.panes).subspan(id.slot(), 1).front();
  return slot.generation == id.generation() ? slot.pane.get() : nullptr;
}

[[nodiscard]] auto find_pane(const Session& session, const PaneId id) noexcept -> const Pane* {
  if (!id.is_valid() || id.slot() >= session.panes.size()) {
    return nullptr;
  }
  const auto& slot = std::span(session.panes).subspan(id.slot(), 1).front();
  return slot.generation == id.generation() ? slot.pane.get() : nullptr;
}

[[nodiscard]] auto find_pane(Session& session, const Tab& tab, const PaneId id) noexcept -> Pane* {
  auto* const pane = find_pane(session, id);
  return pane != nullptr && pane->tab == tab.id ? pane : nullptr;
}

[[nodiscard]] auto find_pane(const Session& session, const Tab& tab, const PaneId id) noexcept
    -> const Pane* {
  const auto* const pane = find_pane(session, id);
  return pane != nullptr && pane->tab == tab.id ? pane : nullptr;
}

[[nodiscard]] auto active_tab(Session& session) noexcept -> Tab* {
  return find_tab(session, session.active_tab);
}

[[nodiscard]] auto pane_count(const Session& session) noexcept -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(
      session.panes, [](const PaneSlot& slot) { return slot.pane != nullptr; }));
}

[[nodiscard]] auto tab_count(const Session& session) noexcept -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(session.tabs, [](const TabSlot& slot) { return slot.tab != nullptr; }));
}

[[nodiscard]] auto empty_pane_slot(const Session& session) noexcept -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < session.panes.size(); ++index) {
    const auto& slot = std::span(session.panes).subspan(index, 1).front();
    if (slot.pane == nullptr && next_generation(slot.generation) != 0) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto empty_tab_slot(const Session& session) noexcept -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < session.tabs.size(); ++index) {
    const auto& slot = std::span(session.tabs).subspan(index, 1).front();
    if (slot.tab == nullptr && next_generation(slot.generation) != 0) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto pane_for_projection(Session& session, Pane* const staged,
                                       const PaneId id) noexcept -> Pane* {
  if (staged != nullptr && staged->id == id) {
    return staged;
  }
  return find_pane(session, id);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto make_projection(const Session& session, const Tab& tab, const PaneLayout& layout,
                                   const bool zoomed, const PaneId focused,
                                   const PaneRectangle viewport, const Pane* const staged) noexcept
    -> std::optional<LayoutProjection> {
  LayoutProjection projection;
  if (zoomed) {
    const auto* const pane =
        staged != nullptr && staged->id == focused ? staged : find_pane(session, focused);
    if (pane == nullptr || pane->tab != tab.id) {
      return std::nullopt;
    }
    std::span(projection.rectangles).subspan(focused.slot(), 1).front() = viewport;
    std::span(projection.panes).subspan(focused.slot(), 1).front() = focused;
    std::span(projection.included).subspan(focused.slot(), 1).front() = true;
    projection.pane_count = 1;
    return projection;
  }
  const auto projected = layout.project(viewport);
  if (!projected.has_value()) {
    return std::nullopt;
  }
  projection = *projected;
  std::size_t members = 0;
  for (std::size_t index = 0; index < session.panes.size(); ++index) {
    const auto& pane_slot = std::span(session.panes).subspan(index, 1).front();
    const bool belongs = pane_slot.pane != nullptr && pane_slot.pane->tab == tab.id;
    const bool staged_here =
        staged != nullptr && staged->tab == tab.id && staged->id.slot() == index;
    const bool included = std::span(projection.included).subspan(index, 1).front();
    if ((belongs || staged_here) != included) {
      return std::nullopt;
    }
    if (included) {
      const auto expected = staged_here ? staged->id : pane_slot.pane->id;
      if (std::span(projection.panes).subspan(index, 1).front() != expected) {
        return std::nullopt;
      }
      ++members;
    }
  }
  return members == projection.pane_count ? std::optional{projection} : std::nullopt;
}

[[nodiscard]] auto request_projection(Session& session, const SessionRuntimeEffects& runtime,
                                      const Tab& tab, const LayoutProjection& projection,
                                      Pane* const staged = nullptr) noexcept
    -> RuntimeEffectStatus {
  std::array<ResizePaneEffect, panes_per_session_max> effects{};
  std::size_t count = 0;
  for (std::size_t index = 0; index < projection.included.size(); ++index) {
    if (!std::span(projection.included).subspan(index, 1).front()) {
      continue;
    }
    const auto pane_id = std::span(projection.panes).subspan(index, 1).front();
    auto* const pane = pane_for_projection(session, staged, pane_id);
    if (pane == nullptr || pane->tab != tab.id || count == effects.size()) {
      return RuntimeEffectStatus::consistency_lost;
    }
    const auto target = std::span(projection.rectangles).subspan(index, 1).front();
    std::span(effects).subspan(count, 1).front() = {
        .session = session.id,
        .pane = pane_id,
        .previous = pane == staged ? target : pane->rectangle,
        .target = target,
    };
    ++count;
  }
  return runtime.resize == nullptr
             ? RuntimeEffectStatus::rejected
             : runtime.resize(runtime.context, std::span(effects).first(count));
}

void commit_projection(Session& session, const LayoutProjection& projection,
                       Pane* const staged = nullptr) noexcept {
  for (std::size_t index = 0; index < projection.included.size(); ++index) {
    if (!std::span(projection.included).subspan(index, 1).front()) {
      continue;
    }
    const auto pane_id = std::span(projection.panes).subspan(index, 1).front();
    auto* const pane = pane_for_projection(session, staged, pane_id);
    LEMMA_ASSERT(pane != nullptr);
    pane->rectangle = std::span(projection.rectangles).subspan(index, 1).front();
  }
}

[[nodiscard]] auto effect_failure_transition(const RuntimeEffectStatus status) noexcept
    -> SessionTransition {
  return {.result = {.status = status == RuntimeEffectStatus::consistency_lost
                                   ? CommandStatus::failed
                                   : CommandStatus::unavailable},
          .handled = true,
          .mutated = status == RuntimeEffectStatus::consistency_lost};
}

[[nodiscard]] auto resize_candidate(Session& session, const SessionRuntimeEffects& runtime,
                                    Tab& tab, const PaneLayout& layout, const bool zoomed,
                                    const PaneId focused, const PaneRectangle viewport,
                                    Pane* const staged, LayoutProjection& projection) noexcept
    -> RuntimeEffectStatus {
  const auto projected = make_projection(session, tab, layout, zoomed, focused, viewport, staged);
  if (!projected.has_value()) {
    return RuntimeEffectStatus::rejected;
  }
  projection = *projected;
  return request_projection(session, runtime, tab, projection, staged);
}

void commit_tab_viewport(Tab& tab, const PaneRectangle viewport) noexcept {
  tab.layout_column = viewport.column;
  tab.layout_row = viewport.row;
  tab.layout_columns = viewport.columns;
  tab.layout_rows = viewport.rows;
  tab.layout_suspended = false;
}

// Resizes a Tab's Panes into viewport and presents it there, or suspends the Tab when the layout
// does not fit or Runtime rejects the resize.
[[nodiscard]] auto present_tab(Session& session, const SessionRuntimeEffects& runtime, Tab& tab,
                               const std::optional<LayoutProjection>& projection,
                               const PaneRectangle viewport) noexcept -> RuntimeEffectStatus {
  if (!projection.has_value()) {
    tab.layout_suspended = true;
    return RuntimeEffectStatus::rejected;
  }
  const auto status = request_projection(session, runtime, tab, *projection);
  if (status == RuntimeEffectStatus::applied) {
    commit_projection(session, *projection);
    commit_tab_viewport(tab, viewport);
  } else if (status == RuntimeEffectStatus::rejected) {
    tab.layout_suspended = true;
  }
  return status;
}

[[nodiscard]] auto fit_tab(Session& session, const SessionRuntimeEffects& runtime, Tab& tab,
                           const bool suspend_on_rejection) noexcept -> RuntimeEffectStatus {
  const auto viewport = session.attachment.content_viewport;
  const auto projection = tab.layout.project(viewport);
  if (!projection.has_value()) {
    tab.layout_suspended = true;
    return RuntimeEffectStatus::applied;
  }
  const auto status = request_projection(session, runtime, tab, *projection);
  if (status == RuntimeEffectStatus::applied) {
    commit_tab_viewport(tab, viewport);
    commit_projection(session, *projection);
  } else if (status == RuntimeEffectStatus::rejected && suspend_on_rejection) {
    tab.layout_suspended = true;
  }
  return status;
}

void reset_removed_tab_attachment(Session& session, const TabId tab) noexcept {
  if (session.attachment.selection_target.has_value() &&
      session.attachment.selection_target->tab == tab) {
    session.attachment.selection_target.reset();
    session.attachment.copy_mode = {};
  }
  if (session.attachment.rename_prompt.kind == RenamePromptKind::tab &&
      session.attachment.rename_prompt.tab == tab) {
    session.attachment.rename_prompt = {};
  }
  if (session.attachment.mouse_capture.has_value() &&
      (session.attachment.mouse_capture->target.tab == tab ||
       session.attachment.mouse_capture->status_tab_before == tab)) {
    session.attachment.mouse_capture.reset();
  }
}

[[nodiscard]] auto remove_tab_transition(Session& session, const SessionRuntimeEffects& runtime,
                                         const TabId id) noexcept -> SessionTransition {
  auto* const tab = find_tab(session, id);
  const auto removed_position = session.tab_order.position_of(id);
  if (tab == nullptr || !removed_position.has_value()) {
    return {.result = {.status = CommandStatus::stale_target}, .handled = true};
  }
  const bool removed_active = session.active_tab == id;
  reset_removed_tab_attachment(session, id);
  for (auto& pane_slot : session.panes) {
    if (pane_slot.pane == nullptr || pane_slot.pane->tab != id) {
      continue;
    }
    runtime.retire(runtime.context, session.id, pane_slot.pane->id);
    pane_slot.pane.reset();
  }
  std::span(session.tabs).subspan(id.slot(), 1).front().tab.reset();
  const bool erased = session.tab_order.erase(id);
  LEMMA_ASSERT(erased);

  SessionTransition transition{
      .result = {.status = CommandStatus::applied},
      .change = {.frame_requested = true,
                 .force_full_frame = removed_active,
                 .status_changed = true,
                 .layout_changed = removed_active},
      .handled = true,
      .mutated = true,
  };
  if (session.tab_order.empty()) {
    session.active_tab = {};
    session.previous_tab = {};
    session.active = false;
    return transition;
  }
  if (!removed_active) {
    if (session.previous_tab == id) {
      session.previous_tab = session.active_tab;
    }
    return transition;
  }
  const auto next_position = std::min(*removed_position, session.tab_order.size() - 1U);
  const auto selected_id = session.tab_order.at(next_position);
  LEMMA_ASSERT(selected_id.has_value());
  auto* const selected = find_tab(session, *selected_id);
  LEMMA_ASSERT(selected != nullptr);
  session.active_tab = *selected_id;
  session.previous_tab = *selected_id;
  const auto fitted = fit_tab(session, runtime, *selected, true);
  if (fitted == RuntimeEffectStatus::consistency_lost) {
    session.active = false;
    transition.result.status = CommandStatus::failed;
  }
  return transition;
}

[[nodiscard]] auto close_pane_transition(Session& session, const SessionRuntimeEffects& runtime,
                                         Tab& tab, const PaneId pane_id) noexcept
    -> SessionTransition {
  auto* const pane = find_pane(session, tab, pane_id);
  if (pane == nullptr) {
    return {.result = {.status = CommandStatus::stale_target}, .handled = true};
  }
  if (tab.layout.pane_count() == 1U) {
    return remove_tab_transition(session, runtime, tab.id);
  }
  auto proposed = tab.layout;
  const auto focus_candidate = proposed.remove(pane_id);
  if (!focus_candidate.has_value()) {
    return {.result = {.status = CommandStatus::failed}, .handled = true};
  }
  // A presented Tab keeps its viewport, which is the content viewport while it is active. A
  // suspended active Tab instead refits the content viewport now that it has fewer Panes; if it
  // still does not fit, the Pane leaves without resizing its siblings and the Tab stays suspended.
  const bool refit = tab.layout_suspended && session.active_tab == tab.id;
  const auto viewport = refit ? session.attachment.content_viewport : tab_viewport(tab);
  const auto projection = proposed.project(viewport);
  if (!projection.has_value() && !refit) {
    return {.result = {.status = CommandStatus::failed}, .handled = true};
  }
  const bool was_focused = tab.focused_pane == pane_id;
  const auto slot = static_cast<std::size_t>(pane_id.slot());
  runtime.retire(runtime.context, session.id, pane_id);
  std::span(session.panes).subspan(slot, 1).front().pane.reset();
  tab.layout = proposed;
  tab.zoomed = false;
  if (was_focused) {
    tab.focused_pane = *focus_candidate;
  }
  if (tab.previous_pane == pane_id || find_pane(session, tab, tab.previous_pane) == nullptr) {
    tab.previous_pane = tab.focused_pane;
  }
  if (session.attachment.selection_target ==
      std::optional{AttachmentPaneTarget{.tab = tab.id, .pane = pane_id}}) {
    session.attachment.selection_target.reset();
    session.attachment.copy_mode = {};
  }
  const auto resized = present_tab(session, runtime, tab, projection, viewport);
  if (resized == RuntimeEffectStatus::consistency_lost) {
    session.active = false;
  }
  return {.result = {.status = resized == RuntimeEffectStatus::consistency_lost
                                   ? CommandStatus::failed
                                   : CommandStatus::applied},
          .change = {.frame_requested = true,
                     .force_full_frame = true,
                     .status_changed = true,
                     .layout_changed = true},
          .handled = true,
          .mutated = true};
}

[[nodiscard]] auto focus_transition(Session& session, const SessionRuntimeEffects& runtime,
                                    Tab& tab, const PaneId target) noexcept -> SessionTransition {
  if (find_pane(session, tab, target) == nullptr) {
    return {.result = {.status = CommandStatus::stale_target}, .handled = true};
  }
  if (tab.focused_pane == target) {
    return {.result = {.status = CommandStatus::no_effect}, .handled = true};
  }
  LayoutProjection projection;
  if (tab.zoomed) {
    const auto viewport = tab_viewport(tab);
    const auto resized = resize_candidate(session, runtime, tab, tab.layout, true, target, viewport,
                                          nullptr, projection);
    if (resized != RuntimeEffectStatus::applied) {
      if (resized == RuntimeEffectStatus::consistency_lost) {
        session.active = false;
      }
      return effect_failure_transition(resized);
    }
  }
  tab.previous_pane = tab.focused_pane;
  tab.focused_pane = target;
  if (tab.zoomed) {
    commit_projection(session, projection);
  }
  return {.result = {.status = CommandStatus::applied},
          .change = {.invalidate_terminal = target,
                     .frame_requested = true,
                     .force_full_frame = tab.zoomed},
          .handled = true,
          .mutated = true};
}

[[nodiscard]] auto layout_transition(Session& session, const SessionRuntimeEffects& runtime,
                                     Tab& tab, const PaneLayout& proposed) noexcept
    -> SessionTransition {
  const auto viewport = tab_viewport(tab);
  LayoutProjection projection;
  const auto resized = resize_candidate(session, runtime, tab, proposed, false, tab.focused_pane,
                                        viewport, nullptr, projection);
  if (resized != RuntimeEffectStatus::applied) {
    if (resized == RuntimeEffectStatus::consistency_lost) {
      session.active = false;
    }
    return effect_failure_transition(resized);
  }
  tab.layout = proposed;
  commit_projection(session, projection);
  return {.result = {.status = CommandStatus::applied},
          .change = {.frame_requested = true, .force_full_frame = true, .layout_changed = true},
          .handled = true,
          .mutated = true};
}

[[nodiscard]] auto select_tab_transition(Session& session, const SessionRuntimeEffects& runtime,
                                         const TabId id) noexcept -> SessionTransition {
  auto* const selected = find_tab(session, id);
  if (selected == nullptr) {
    return {.result = {.status = CommandStatus::stale_target}, .handled = true};
  }
  if (session.active_tab == id) {
    return {.result = {.status = CommandStatus::no_effect}, .handled = true};
  }
  const auto fitted = fit_tab(session, runtime, *selected, false);
  if (fitted != RuntimeEffectStatus::applied) {
    if (fitted == RuntimeEffectStatus::consistency_lost) {
      session.active = false;
    }
    return effect_failure_transition(fitted);
  }
  session.previous_tab = session.active_tab;
  session.active_tab = id;
  return {.result = {.status = CommandStatus::applied},
          .change = {.frame_requested = true,
                     .force_full_frame = true,
                     .status_changed = true,
                     .layout_changed = true},
          .handled = true,
          .mutated = true};
}

[[nodiscard]] auto targeted_tab(Session& session, const Command& command) noexcept -> Tab* {
  return command.target.tab.is_valid() ? find_tab(session, command.target.tab)
                                       : active_tab(session);
}

[[nodiscard]] auto targeted_pane(Session& session, Tab& tab, const Command& command) noexcept
    -> Pane* {
  return command.target.pane.is_valid() ? find_pane(session, tab, command.target.pane)
                                        : find_pane(session, tab, tab.focused_pane);
}

[[nodiscard]] constexpr auto hash_mix(std::uint64_t hash, const std::uint64_t value) noexcept
    -> std::uint64_t {
  hash ^= value;
  hash *= 1'099'511'628'211ULL;
  return hash;
}

[[nodiscard]] constexpr auto id_code(const auto id) noexcept -> std::uint64_t {
  return id.is_valid() ? (static_cast<std::uint64_t>(id.generation()) << 32U) | id.slot() : 0;
}

} // namespace

auto session_lifecycle_command(const CommandKind kind) noexcept -> bool {
  switch (kind) {
  case CommandKind::split_left_right:
  case CommandKind::split_top_bottom:
  case CommandKind::resize_left_right_divider:
  case CommandKind::resize_top_bottom_divider:
  case CommandKind::resize_left:
  case CommandKind::resize_right:
  case CommandKind::resize_up:
  case CommandKind::resize_down:
  case CommandKind::focus_left:
  case CommandKind::focus_right:
  case CommandKind::focus_up:
  case CommandKind::focus_down:
  case CommandKind::focus_next:
  case CommandKind::focus_previous:
  case CommandKind::focus_pane:
  case CommandKind::close_pane:
  case CommandKind::toggle_zoom:
  case CommandKind::set_zoom:
  case CommandKind::create_tab:
  case CommandKind::next_tab:
  case CommandKind::previous_tab:
  case CommandKind::close_tab:
  case CommandKind::select_tab:
  case CommandKind::rename_session:
  case CommandKind::rename_tab:
  case CommandKind::place_tab:
  case CommandKind::swap_panes:
  case CommandKind::stop_session:
    return true;
  case CommandKind::none:
  case CommandKind::detach_client:
  case CommandKind::cancel_attachment_interaction:
  case CommandKind::enter_copy_mode:
  case CommandKind::enter_copy_search_forward:
  case CommandKind::enter_copy_search_backward:
  case CommandKind::copy_selection:
  case CommandKind::begin_rename_session:
  case CommandKind::begin_rename_tab:
    return false;
  }
  return false;
}

// Directional scoring is semantic and deterministic; Runtime geometry never participates.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto pane_in_direction(const Session& session, const Tab& tab, const PaneId source,
                       const PaneDirection direction) noexcept -> std::optional<PaneId> {
  const auto viewport = tab_viewport(tab);
  const auto projection = tab.layout.project(viewport);
  const auto current = projection.has_value() ? projection->rectangle(source) : std::nullopt;
  if (!projection.has_value() || !current.has_value()) {
    return std::nullopt;
  }
  const auto current_right = static_cast<std::uint32_t>(current->column) + current->columns;
  const auto current_bottom = static_cast<std::uint32_t>(current->row) + current->rows;
  const auto current_x = (static_cast<std::uint32_t>(current->column) * 2U) + current->columns;
  const auto current_y = (static_cast<std::uint32_t>(current->row) * 2U) + current->rows;
  std::uint64_t best_score = std::numeric_limits<std::uint64_t>::max();
  std::optional<PaneId> best;
  for (const auto& pane_slot : session.panes) {
    if (pane_slot.pane == nullptr || pane_slot.pane->tab != tab.id ||
        pane_slot.pane->id == source) {
      continue;
    }
    const auto rectangle = projection->rectangle(pane_slot.pane->id);
    if (!rectangle.has_value()) {
      continue;
    }
    const auto right = static_cast<std::uint32_t>(rectangle->column) + rectangle->columns;
    const auto bottom = static_cast<std::uint32_t>(rectangle->row) + rectangle->rows;
    const auto x = (static_cast<std::uint32_t>(rectangle->column) * 2U) + rectangle->columns;
    const auto y = (static_cast<std::uint32_t>(rectangle->row) * 2U) + rectangle->rows;
    bool eligible = false;
    std::uint32_t primary = 0;
    std::uint32_t secondary = 0;
    switch (direction) {
    case PaneDirection::left:
      eligible = right <= current->column;
      primary = eligible ? current->column - right : 0;
      secondary = y > current_y ? y - current_y : current_y - y;
      break;
    case PaneDirection::right:
      eligible = rectangle->column >= current_right;
      primary = eligible ? rectangle->column - current_right : 0;
      secondary = y > current_y ? y - current_y : current_y - y;
      break;
    case PaneDirection::up:
      eligible = bottom <= current->row;
      primary = eligible ? current->row - bottom : 0;
      secondary = x > current_x ? x - current_x : current_x - x;
      break;
    case PaneDirection::down:
      eligible = rectangle->row >= current_bottom;
      primary = eligible ? rectangle->row - current_bottom : 0;
      secondary = x > current_x ? x - current_x : current_x - x;
      break;
    }
    const auto score = (static_cast<std::uint64_t>(primary) * 4'096U) + secondary;
    if (eligible && score < best_score) {
      best_score = score;
      best = pane_slot.pane->id;
    }
  }
  return best;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto SessionMachine::create_tab(const CreateTabOptions options) noexcept -> SessionTransition {
  if (!session_.id.is_valid() || !options_.runtime.valid() ||
      tab_count(session_) >= session_.tabs.size() ||
      pane_count(session_) >= panes_per_session_max) {
    return {.result = {.status = CommandStatus::capacity}, .handled = true};
  }
  const bool initial =
      tab_count(session_) == 0 && pane_count(session_) == 0 && !session_.active_tab.is_valid();
  const auto pane_index = empty_pane_slot(session_);
  const auto tab_index = empty_tab_slot(session_);
  if (!pane_index.has_value() || !tab_index.has_value()) {
    return {.result = {.status = CommandStatus::capacity}, .handled = true};
  }
  auto& pane_slot = std::span(session_.panes).subspan(*pane_index, 1).front();
  auto& tab_slot = std::span(session_.tabs).subspan(*tab_index, 1).front();
  const auto pane_generation = next_generation(pane_slot.generation);
  const auto tab_generation = next_generation(tab_slot.generation);
  const auto pane_id = PaneId::from_parts(static_cast<std::uint32_t>(*pane_index), pane_generation);
  const auto tab_id = TabId::from_parts(static_cast<std::uint32_t>(*tab_index), tab_generation);
  const auto working_directory =
      options.working_directory.empty() ? session_.cwd() : options.working_directory;

  std::unique_ptr<PaneLaunchIntent> launch;
  std::unique_ptr<Pane> pane;
  std::unique_ptr<Tab> tab;
  try {
    launch = std::make_unique<PaneLaunchIntent>();
    launch->bytes.assign(options.command.begin(), options.command.end());
    launch->working_directory = working_directory;
    pane = std::make_unique<Pane>(Pane{
        .id = pane_id,
        .tab = tab_id,
        .rectangle = session_.attachment.content_viewport,
        .launch_intent = std::move(launch),
        .process_exit = std::nullopt,
        .exit_policy = options.exit_policy,
    });
    tab = std::make_unique<Tab>(tab_id, pane_id);
  } catch (...) {
    return {.result = {.status = CommandStatus::unavailable}, .handled = true};
  }
  tab->layout_column = pane->rectangle.column;
  tab->layout_row = pane->rectangle.row;
  tab->layout_columns = pane->rectangle.columns;
  tab->layout_rows = pane->rectangle.rows;
  const SpawnPaneEffect spawn{
      .session = session_.id,
      .tab = tab_id,
      .pane = pane_id,
      .rectangle = pane->rectangle,
      .working_directory = pane->launch_working_directory(),
      .command = pane->launch_command(),
      .fallback_working_directory = options.fallback_working_directory,
  };
  const auto spawned = options_.runtime.spawn(options_.runtime.context, spawn);
  if (spawned != RuntimeEffectStatus::applied) {
    if (spawned == RuntimeEffectStatus::consistency_lost) {
      session_.active = false;
    }
    return finish(effect_failure_transition(spawned));
  }
  if (!session_.tab_order.append(tab_id)) {
    options_.runtime.retire(options_.runtime.context, session_.id, pane_id);
    return {.result = {.status = CommandStatus::failed}, .handled = true};
  }
  pane_slot.generation = pane_generation;
  pane_slot.pane = std::move(pane);
  tab_slot.generation = tab_generation;
  tab_slot.tab = std::move(tab);
  const bool activate = options.activate || !session_.active_tab.is_valid();
  if (activate) {
    session_.previous_tab = session_.active_tab.is_valid() ? session_.active_tab : tab_id;
    session_.active_tab = tab_id;
  }
  session_.active = true;
  return finish({.result = {.status = CommandStatus::applied},
                 .change = {.frame_requested = true,
                            .force_full_frame = activate,
                            .status_changed = true,
                            .layout_changed = activate},
                 .created_tab = tab_id,
                 .created_pane = pane_id,
                 .handled = true,
                 .mutated = !initial});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto SessionMachine::split_pane(const TabId tab_id, const PaneId source, const SplitAxis axis,
                                const SplitPaneOptions options) noexcept -> SessionTransition {
  auto* const tab = find_tab(session_, tab_id);
  if (tab == nullptr || find_pane(session_, *tab, source) == nullptr) {
    return {.result = {.status = CommandStatus::stale_target}, .handled = true};
  }
  if (!options_.runtime.valid() || pane_count(session_) >= panes_per_session_max) {
    return {.result = {.status = CommandStatus::capacity}, .handled = true};
  }
  const auto pane_index = empty_pane_slot(session_);
  if (!pane_index.has_value()) {
    return {.result = {.status = CommandStatus::capacity}, .handled = true};
  }
  auto& pane_slot = std::span(session_.panes).subspan(*pane_index, 1).front();
  const auto pane_generation = next_generation(pane_slot.generation);
  const auto pane_id = PaneId::from_parts(static_cast<std::uint32_t>(*pane_index), pane_generation);
  auto proposed = tab->layout;
  if (!proposed.split(source, pane_id, axis)) {
    return {.result = {.status = CommandStatus::unavailable}, .handled = true};
  }
  const auto viewport = tab_viewport(*tab);
  const auto projection = proposed.project(viewport);
  const auto rectangle = projection.has_value() ? projection->rectangle(pane_id) : std::nullopt;
  if (!projection.has_value() || !rectangle.has_value()) {
    return {.result = {.status = CommandStatus::unavailable}, .handled = true};
  }
  const auto working_directory =
      options.working_directory.empty() ? session_.cwd() : options.working_directory;
  std::unique_ptr<Pane> pane;
  try {
    auto launch = std::make_unique<PaneLaunchIntent>();
    launch->bytes.assign(options.command.begin(), options.command.end());
    launch->working_directory = working_directory;
    pane = std::make_unique<Pane>(Pane{.id = pane_id,
                                       .tab = tab_id,
                                       .rectangle = *rectangle,
                                       .launch_intent = std::move(launch),
                                       .process_exit = std::nullopt,
                                       .exit_policy = options.exit_policy});
  } catch (...) {
    return {.result = {.status = CommandStatus::unavailable}, .handled = true};
  }
  const SpawnPaneEffect spawn{
      .session = session_.id,
      .tab = tab_id,
      .pane = pane_id,
      .rectangle = *rectangle,
      .working_directory = pane->launch_working_directory(),
      .command = pane->launch_command(),
      .fallback_working_directory = options.fallback_working_directory,
  };
  const auto spawned = options_.runtime.spawn(options_.runtime.context, spawn);
  if (spawned != RuntimeEffectStatus::applied) {
    if (spawned == RuntimeEffectStatus::consistency_lost) {
      session_.active = false;
    }
    return finish(effect_failure_transition(spawned));
  }
  const auto resized =
      request_projection(session_, options_.runtime, *tab, *projection, pane.get());
  if (resized != RuntimeEffectStatus::applied) {
    options_.runtime.retire(options_.runtime.context, session_.id, pane_id);
    if (resized == RuntimeEffectStatus::consistency_lost) {
      session_.active = false;
    }
    return finish(effect_failure_transition(resized));
  }
  const auto previous_focus = tab->focused_pane;
  pane_slot.generation = pane_generation;
  pane_slot.pane = std::move(pane);
  tab->layout = proposed;
  tab->zoomed = false;
  if (options.focus_created) {
    tab->previous_pane = previous_focus;
    tab->focused_pane = pane_id;
  }
  commit_projection(session_, *projection);
  return finish({.result = {.status = CommandStatus::applied},
                 .change = {.frame_requested = true,
                            .force_full_frame = true,
                            .status_changed = true,
                            .layout_changed = true},
                 .created_tab = tab_id,
                 .created_pane = pane_id,
                 .handled = true,
                 .mutated = true});
}

auto SessionMachine::resize_attachment(const std::uint16_t columns,
                                       const std::uint16_t rows) noexcept -> SessionTransition {
  if (session_.attachment.columns == columns && session_.attachment.rows == rows &&
      session_.attachment.content_viewport == PaneRectangle{.columns = columns, .rows = rows}) {
    return {.result = {.status = CommandStatus::no_effect},
            .change = {.frame_requested = true, .force_full_frame = true},
            .handled = true};
  }
  return resize_attachment(columns, rows, PaneRectangle{.columns = columns, .rows = rows});
}

auto SessionMachine::resize_attachment(const std::uint16_t columns, const std::uint16_t rows,
                                       const PaneRectangle viewport) noexcept -> SessionTransition {
  const auto viewport_right = static_cast<std::uint32_t>(viewport.column) + viewport.columns;
  const auto viewport_bottom = static_cast<std::uint32_t>(viewport.row) + viewport.rows;
  // Explicit content geometry is derived by native presentation policy (chrome and docks).
  // Validate physical containment here, not an assumed one-row status line.
  if (columns == 0 || rows == 0 || viewport.columns == 0 || viewport.rows == 0 ||
      viewport_right > columns || viewport_bottom > rows || !options_.runtime.valid()) {
    return {.result = {.status = CommandStatus::invalid_command}, .handled = true};
  }
  auto* const tab = active_tab(session_);
  if (tab == nullptr) {
    return {.result = {.status = CommandStatus::failed}, .handled = true};
  }
  // The unzoomed tree decides whether the physical viewport is representable. If it is, only the
  // currently visible zoomed Pane needs an immediate Runtime resize.
  if (!tab->layout.project(viewport).has_value()) {
    session_.attachment.columns = columns;
    session_.attachment.rows = rows;
    session_.attachment.content_viewport = viewport;
    tab->layout_suspended = true;
    return finish(
        {.result = {.status = CommandStatus::applied},
         .change = {.frame_requested = true, .force_full_frame = true, .layout_changed = true},
         .handled = true,
         .mutated = true});
  }
  const auto projection = make_projection(session_, *tab, tab->layout, tab->zoomed,
                                          tab->focused_pane, viewport, nullptr);
  if (!projection.has_value()) {
    session_.active = false;
    return finish(effect_failure_transition(RuntimeEffectStatus::consistency_lost));
  }
  const auto resized = request_projection(session_, options_.runtime, *tab, *projection);
  if (resized != RuntimeEffectStatus::applied) {
    if (resized == RuntimeEffectStatus::consistency_lost) {
      session_.active = false;
    }
    return finish(effect_failure_transition(resized));
  }
  session_.attachment.columns = columns;
  session_.attachment.rows = rows;
  session_.attachment.content_viewport = viewport;
  commit_tab_viewport(*tab, viewport);
  commit_projection(session_, *projection);
  return finish(
      {.result = {.status = CommandStatus::applied},
       .change = {.frame_requested = true, .force_full_frame = true, .layout_changed = true},
       .handled = true,
       .mutated = true});
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto SessionMachine::dispatch(const Command& command) noexcept -> SessionTransition {
  if (!session_lifecycle_command(command.kind)) {
    return {};
  }
  if (!options_.runtime.valid()) {
    return {.result = {.status = CommandStatus::failed}, .handled = true};
  }
  if (command.target.session.is_valid() && command.target.session != session_.id) {
    return {.result = {.status = command.target.session.slot() == session_.id.slot()
                                     ? CommandStatus::stale_target
                                     : CommandStatus::wrong_owner},
            .handled = true};
  }
  if (command.target.attachment.is_valid() && command.target.attachment != session_.attachment.id) {
    return {.result = {.status = command.target.attachment.slot() == session_.attachment.id.slot()
                                     ? CommandStatus::stale_target
                                     : CommandStatus::wrong_owner},
            .handled = true};
  }
  if (command.kind == CommandKind::stop_session) {
    const bool changed = session_.active;
    session_.active = false;
    return finish(
        {.result = {.status = changed ? CommandStatus::applied : CommandStatus::no_effect},
         .handled = true,
         .mutated = changed});
  }
  if (command.kind == CommandKind::rename_session) {
    const auto* const name = std::get_if<SessionNameValue>(&command.payload);
    if (name == nullptr || !name->valid()) {
      return {.result = {.status = CommandStatus::invalid_command}, .handled = true};
    }
    if (session_.session_name() == name->view()) {
      return {.result = {.status = CommandStatus::no_effect}, .handled = true};
    }
    if (options_.name_conflict == nullptr) {
      return {.result = {.status = CommandStatus::unavailable}, .handled = true};
    }
    if (options_.name_conflict(options_.name_conflict_context, session_.id, name->view())) {
      return {.result = {.status = CommandStatus::conflict}, .handled = true};
    }
    const bool renamed = session_.rename(name->view());
    LEMMA_ASSERT(renamed);
    session_.attachment.rename_prompt = {};
    return finish({.result = {.status = CommandStatus::applied},
                   .change = {.frame_requested = true, .status_changed = true},
                   .handled = true,
                   .mutated = true});
  }

  auto* const tab = targeted_tab(session_, command);
  if (tab == nullptr) {
    return {.result = {.status = command.target.tab.is_valid() ? CommandStatus::stale_target
                                                               : CommandStatus::failed},
            .handled = true};
  }
  if (command.kind == CommandKind::create_tab) {
    return create_tab();
  }
  if (command.kind == CommandKind::next_tab || command.kind == CommandKind::previous_tab) {
    const auto current = session_.tab_order.position_of(session_.active_tab);
    if (!current.has_value() || session_.tab_order.size() <= 1U) {
      return {.result = {.status = CommandStatus::no_effect}, .handled = true};
    }
    const auto count = session_.tab_order.size();
    const auto position = command.kind == CommandKind::next_tab ? (*current + 1U) % count
                                                                : (*current + count - 1U) % count;
    const auto selected = session_.tab_order.at(position);
    LEMMA_ASSERT(selected.has_value());
    return finish(select_tab_transition(session_, options_.runtime, *selected));
  }
  if (command.kind == CommandKind::close_tab) {
    return finish(remove_tab_transition(session_, options_.runtime, tab->id));
  }
  if (command.kind == CommandKind::select_tab) {
    if (!command.target.tab.is_valid()) {
      return {.result = {.status = CommandStatus::unavailable}, .handled = true};
    }
    return finish(select_tab_transition(session_, options_.runtime, command.target.tab));
  }
  if (command.kind == CommandKind::rename_tab) {
    const auto* const title = std::get_if<TabTitleValue>(&command.payload);
    if (title == nullptr) {
      return {.result = {.status = CommandStatus::invalid_command}, .handled = true};
    }
    if (tab->title_override() == title->view()) {
      return {.result = {.status = CommandStatus::no_effect}, .handled = true};
    }
    const bool renamed = tab->set_title_override(title->view());
    LEMMA_ASSERT(renamed);
    if (session_.attachment.rename_prompt.kind == RenamePromptKind::tab &&
        session_.attachment.rename_prompt.tab == tab->id) {
      session_.attachment.rename_prompt = {};
    }
    return finish({.result = {.status = CommandStatus::applied},
                   .change = {.frame_requested = true, .status_changed = true},
                   .handled = true,
                   .mutated = true});
  }
  if (command.kind == CommandKind::place_tab) {
    const auto* const placement = std::get_if<TabPlacementCommand>(&command.payload);
    if (placement == nullptr) {
      return {.result = {.status = CommandStatus::invalid_command}, .handled = true};
    }
    if (placement->before.is_valid() && find_tab(session_, placement->before) == nullptr) {
      return {.result = {.status = CommandStatus::stale_target}, .handled = true};
    }
    const bool changed = session_.tab_order.place_before(
        tab->id, placement->before.is_valid() ? std::optional{placement->before} : std::nullopt);
    return finish(
        {.result = {.status = changed ? CommandStatus::applied : CommandStatus::no_effect},
         .change = {.frame_requested = changed, .status_changed = changed},
         .handled = true,
         .mutated = changed});
  }

  auto* const pane = targeted_pane(session_, *tab, command);
  if (pane == nullptr) {
    return {.result = {.status = command.target.pane.is_valid() ? CommandStatus::stale_target
                                                                : CommandStatus::failed},
            .handled = true};
  }
  if (command.kind == CommandKind::split_left_right ||
      command.kind == CommandKind::split_top_bottom) {
    return split_pane(tab->id, pane->id,
                      command.kind == CommandKind::split_left_right ? SplitAxis::left_right
                                                                    : SplitAxis::top_bottom);
  }
  if (command.kind == CommandKind::close_pane) {
    return finish(close_pane_transition(session_, options_.runtime, *tab, pane->id));
  }
  if (command.kind == CommandKind::focus_pane) {
    return finish(focus_transition(session_, options_.runtime, *tab, pane->id));
  }
  if (command.kind == CommandKind::focus_previous) {
    return finish(focus_transition(session_, options_.runtime, *tab, tab->previous_pane));
  }
  if (command.kind == CommandKind::focus_next) {
    auto panes = std::span(session_.panes);
    for (std::size_t offset = 1; offset <= panes.size(); ++offset) {
      const auto slot = (static_cast<std::size_t>(pane->id.slot()) + offset) % panes.size();
      const auto candidate_slot = panes.subspan(slot, 1);
      auto* const candidate = candidate_slot.front().pane.get();
      if (candidate != nullptr && candidate->tab == tab->id) {
        return finish(focus_transition(session_, options_.runtime, *tab, candidate->id));
      }
    }
    return {.result = {.status = CommandStatus::no_effect}, .handled = true};
  }
  if (command.kind == CommandKind::focus_left || command.kind == CommandKind::focus_right ||
      command.kind == CommandKind::focus_up || command.kind == CommandKind::focus_down) {
    auto direction = PaneDirection::left;
    if (command.kind == CommandKind::focus_right) {
      direction = PaneDirection::right;
    } else if (command.kind == CommandKind::focus_up) {
      direction = PaneDirection::up;
    } else if (command.kind == CommandKind::focus_down) {
      direction = PaneDirection::down;
    }
    const auto target = pane_in_direction(session_, *tab, pane->id, direction);
    return target.has_value()
               ? finish(focus_transition(session_, options_.runtime, *tab, *target))
               : SessionTransition{.result = {.status = CommandStatus::no_effect}, .handled = true};
  }
  if (command.kind == CommandKind::toggle_zoom || command.kind == CommandKind::set_zoom) {
    const auto* const requested = std::get_if<PaneZoomCommand>(&command.payload);
    const bool desired = requested == nullptr ? !tab->zoomed : requested->enabled;
    if (requested != nullptr && tab->zoomed == desired &&
        (!desired || tab->focused_pane == pane->id)) {
      return {.result = {.status = CommandStatus::no_effect}, .handled = true};
    }
    const auto focused = desired ? pane->id : tab->focused_pane;
    const auto viewport = tab_viewport(*tab);
    LayoutProjection projection;
    const auto resized = resize_candidate(session_, options_.runtime, *tab, tab->layout, desired,
                                          focused, viewport, nullptr, projection);
    if (resized != RuntimeEffectStatus::applied) {
      if (resized == RuntimeEffectStatus::consistency_lost) {
        session_.active = false;
      }
      return finish(effect_failure_transition(resized));
    }
    if (desired && pane->id != tab->focused_pane) {
      tab->previous_pane = tab->focused_pane;
      tab->focused_pane = pane->id;
    }
    tab->zoomed = desired;
    commit_projection(session_, projection);
    return finish(
        {.result = {.status = CommandStatus::applied},
         .change = {.frame_requested = true, .force_full_frame = true, .layout_changed = true},
         .handled = true,
         .mutated = true});
  }
  if (command.kind == CommandKind::swap_panes) {
    const auto* const swap = std::get_if<PaneSwapCommand>(&command.payload);
    if (swap == nullptr || find_pane(session_, *tab, swap->other) == nullptr) {
      return {.result = {.status = CommandStatus::stale_target}, .handled = true};
    }
    if (tab->zoomed || tab->layout_suspended) {
      return {.result = {.status = CommandStatus::unavailable}, .handled = true};
    }
    auto proposed = tab->layout;
    if (!proposed.swap(pane->id, swap->other)) {
      return {.result = {.status = CommandStatus::stale_target}, .handled = true};
    }
    return finish(layout_transition(session_, options_.runtime, *tab, proposed));
  }
  if (command.kind == CommandKind::resize_left_right_divider ||
      command.kind == CommandKind::resize_top_bottom_divider) {
    if (tab->zoomed || tab->layout_suspended) {
      return {.result = {.status = CommandStatus::unavailable}, .handled = true};
    }
    const auto* const coordinate = std::get_if<CommandCoordinate>(&command.payload);
    auto* const peer = find_pane(session_, *tab, command.target.peer_pane);
    if (coordinate == nullptr || peer == nullptr) {
      return {.result = {.status = CommandStatus::stale_target}, .handled = true};
    }
    auto proposed = tab->layout;
    const auto viewport = tab_viewport(*tab);
    const auto status = proposed.resize_divider(
        {.first = pane->id,
         .second = peer->id,
         .axis = command.kind == CommandKind::resize_left_right_divider ? SplitAxis::left_right
                                                                        : SplitAxis::top_bottom},
        coordinate->value, viewport);
    if (status == LayoutResizeStatus::no_effect) {
      return {.result = {.status = CommandStatus::no_effect}, .handled = true};
    }
    if (status == LayoutResizeStatus::unavailable) {
      return {.result = {.status = CommandStatus::unavailable}, .handled = true};
    }
    if (status == LayoutResizeStatus::invalid) {
      return {.result = {.status = CommandStatus::stale_target}, .handled = true};
    }
    return finish(layout_transition(session_, options_.runtime, *tab, proposed));
  }
  if (command.kind == CommandKind::resize_left || command.kind == CommandKind::resize_right ||
      command.kind == CommandKind::resize_up || command.kind == CommandKind::resize_down) {
    if (tab->zoomed || tab->layout_suspended) {
      return {.result = {.status = CommandStatus::unavailable}, .handled = true};
    }
    const auto* const requested = std::get_if<CommandCoordinate>(&command.payload);
    const auto amount = requested == nullptr ? std::uint16_t{1} : requested->value;
    if (amount == 0) {
      return {.result = {.status = CommandStatus::invalid_command}, .handled = true};
    }
    auto direction = ResizeDirection::left;
    if (command.kind == CommandKind::resize_right) {
      direction = ResizeDirection::right;
    } else if (command.kind == CommandKind::resize_up) {
      direction = ResizeDirection::up;
    } else if (command.kind == CommandKind::resize_down) {
      direction = ResizeDirection::down;
    }
    auto proposed = tab->layout;
    const auto viewport = tab_viewport(*tab);
    const auto status = proposed.resize(pane->id, direction, viewport, amount);
    if (status == LayoutResizeStatus::no_effect) {
      return {.result = {.status = CommandStatus::no_effect}, .handled = true};
    }
    if (status == LayoutResizeStatus::unavailable) {
      return {.result = {.status = CommandStatus::unavailable}, .handled = true};
    }
    if (status == LayoutResizeStatus::invalid) {
      return {.result = {.status = CommandStatus::stale_target}, .handled = true};
    }
    return finish(layout_transition(session_, options_.runtime, *tab, proposed));
  }
  return {.result = {.status = CommandStatus::invalid_command}, .handled = true};
}

auto SessionMachine::runtime_failed(const PaneId pane_id, const ProcessExit process,
                                    const bool child_exit) noexcept -> SessionTransition {
  auto* const pane = find_pane(session_, pane_id);
  auto* const tab = pane == nullptr ? nullptr : find_tab(session_, pane->tab);
  if (pane == nullptr || tab == nullptr) {
    return {.result = {.status = CommandStatus::stale_target}, .handled = true};
  }
  if (child_exit && pane->commit_process_exit(process)) {
    options_.runtime.hold(options_.runtime.context, session_.id, pane_id, process);
    return finish(
        {.result = {.status = CommandStatus::applied},
         .change = {.frame_requested = true, .force_full_frame = true, .status_changed = true},
         .handled = true,
         .mutated = true});
  }
  return finish(close_pane_transition(session_, options_.runtime, *tab, pane_id));
}

auto SessionMachine::finish(SessionTransition transition) noexcept -> SessionTransition {
  if (transition.mutated) {
    record_mutation(session_);
  }
  return transition;
}

auto session_invariant_name(const SessionInvariantError error) noexcept -> std::string_view {
  switch (error) {
  case SessionInvariantError::invalid_session_id:
    return "invalid Session identity";
  case SessionInvariantError::tab_count:
    return "Tab store and order counts differ";
  case SessionInvariantError::tab_slot_identity:
    return "Tab slot identity or generation differs";
  case SessionInvariantError::tab_order_identity:
    return "Tab order is not a bijection with live Tab slots";
  case SessionInvariantError::active_tab:
    return "active Tab does not resolve";
  case SessionInvariantError::previous_tab:
    return "previous Tab does not resolve";
  case SessionInvariantError::pane_count:
    return "Pane store and layout counts differ";
  case SessionInvariantError::pane_slot_identity:
    return "Pane slot identity or generation differs";
  case SessionInvariantError::pane_tab:
    return "Pane owning Tab does not resolve";
  case SessionInvariantError::pane_layout_membership:
    return "Pane and layout membership differ";
  case SessionInvariantError::pane_rectangle_empty:
    return "Pane rectangle has zero width or height";
  case SessionInvariantError::pane_rectangle_out_of_bounds:
    return "presented Pane rectangle is outside its layout viewport";
  case SessionInvariantError::layout_invalid:
    return "Pane layout is invalid";
  case SessionInvariantError::focused_pane:
    return "focused Pane does not belong to its Tab";
  case SessionInvariantError::previous_pane:
    return "previous Pane does not belong to its Tab";
  case SessionInvariantError::process_exit_policy:
    return "process exit exists outside hold policy";
  case SessionInvariantError::attachment_identity:
    return "Attachment identity does not match its Session";
  case SessionInvariantError::attachment_viewport:
    return "Attachment content viewport is empty or outside its geometry";
  case SessionInvariantError::active_tab_viewport:
    return "active Tab is presented outside the Attachment content viewport";
  case SessionInvariantError::attachment_target:
    return "Attachment target does not resolve";
  case SessionInvariantError::mouse_capture_target:
    return "mouse capture target does not resolve";
  }
  return "unknown Session invariant";
}

// One bounded pass checks all semantic owner relationships after each atomic transition.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto check_session_invariants(const Session& session) noexcept
    -> std::optional<SessionInvariantError> {
  if (!session.id.is_valid()) {
    return SessionInvariantError::invalid_session_id;
  }
  std::size_t live_tabs = 0;
  std::array<bool, tabs_per_session_max> ordered{};
  for (std::size_t index = 0; index < session.tabs.size(); ++index) {
    const auto& slot = std::span(session.tabs).subspan(index, 1).front();
    if (slot.tab == nullptr) {
      continue;
    }
    ++live_tabs;
    if (slot.generation == 0 || slot.tab->id.slot() != index ||
        slot.tab->id.generation() != slot.generation) {
      return SessionInvariantError::tab_slot_identity;
    }
    if (!slot.tab->layout.valid()) {
      return SessionInvariantError::layout_invalid;
    }
  }
  if (live_tabs != session.tab_order.size()) {
    return SessionInvariantError::tab_count;
  }
  for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
    const auto id = session.tab_order.at(position);
    if (!id.has_value() || find_tab(session, *id) == nullptr) {
      return SessionInvariantError::tab_order_identity;
    }
    auto ordered_slot = std::span(ordered).subspan(id->slot(), 1);
    if (ordered_slot.front()) {
      return SessionInvariantError::tab_order_identity;
    }
    ordered_slot.front() = true;
  }
  if (live_tabs == 0) {
    if (session.active_tab.is_valid() || session.previous_tab.is_valid() || session.active) {
      return SessionInvariantError::active_tab;
    }
  } else if (find_tab(session, session.active_tab) == nullptr) {
    return SessionInvariantError::active_tab;
  } else if (find_tab(session, session.previous_tab) == nullptr) {
    return SessionInvariantError::previous_tab;
  }

  std::size_t live_panes = 0;
  std::array<std::size_t, tabs_per_session_max> tab_panes{};
  for (std::size_t index = 0; index < session.panes.size(); ++index) {
    const auto& slot = std::span(session.panes).subspan(index, 1).front();
    if (slot.pane == nullptr) {
      continue;
    }
    ++live_panes;
    const auto& pane = *slot.pane;
    if (slot.generation == 0 || pane.id.slot() != index ||
        pane.id.generation() != slot.generation) {
      return SessionInvariantError::pane_slot_identity;
    }
    const auto* const tab = find_tab(session, pane.tab);
    if (tab == nullptr) {
      return SessionInvariantError::pane_tab;
    }
    if (!tab->layout.contains(pane.id)) {
      return SessionInvariantError::pane_layout_membership;
    }
    ++std::span(tab_panes).subspan(tab->id.slot(), 1).front();
    const auto pane_right =
        static_cast<std::uint32_t>(pane.rectangle.column) + pane.rectangle.columns;
    const auto pane_bottom = static_cast<std::uint32_t>(pane.rectangle.row) + pane.rectangle.rows;
    const auto layout_right = static_cast<std::uint32_t>(tab->layout_column) + tab->layout_columns;
    const auto layout_bottom = static_cast<std::uint32_t>(tab->layout_row) + tab->layout_rows;
    if (pane.rectangle.columns == 0 || pane.rectangle.rows == 0) {
      return SessionInvariantError::pane_rectangle_empty;
    }
    const bool pane_presented =
        !tab->layout_suspended && (!tab->zoomed || pane.id == tab->focused_pane);
    if (pane_presented &&
        (pane.rectangle.column < tab->layout_column || pane.rectangle.row < tab->layout_row ||
         pane_right > layout_right || pane_bottom > layout_bottom)) {
      return SessionInvariantError::pane_rectangle_out_of_bounds;
    }
    if (pane.process_exit.has_value() && pane.exit_policy != PaneExitPolicy::hold) {
      return SessionInvariantError::process_exit_policy;
    }
  }
  if ((live_tabs == 0) != (live_panes == 0)) {
    return SessionInvariantError::pane_count;
  }
  for (const auto& slot : session.tabs) {
    if (slot.tab == nullptr) {
      continue;
    }
    const auto& tab = *slot.tab;
    if (tab.layout.pane_count() != std::span(tab_panes).subspan(tab.id.slot(), 1).front()) {
      return SessionInvariantError::pane_count;
    }
    if (find_pane(session, tab, tab.focused_pane) == nullptr) {
      return SessionInvariantError::focused_pane;
    }
    if (find_pane(session, tab, tab.previous_pane) == nullptr) {
      return SessionInvariantError::previous_pane;
    }
  }
  if (session.attachment.id.is_valid() && session.attachment.session != session.id) {
    return SessionInvariantError::attachment_identity;
  }
  const auto content = session.attachment.content_viewport;
  if (content.columns == 0 || content.rows == 0 ||
      static_cast<std::uint32_t>(content.column) + content.columns > session.attachment.columns ||
      static_cast<std::uint32_t>(content.row) + content.rows > session.attachment.rows) {
    return SessionInvariantError::attachment_viewport;
  }
  // The content viewport is the one geometry authority for the active Tab: a presented active
  // Tab is laid out in exactly that rectangle. Inactive Tabs retain their last usable geometry.
  if (const auto* const active = find_tab(session, session.active_tab);
      session.active && active != nullptr && !active->layout_suspended &&
      tab_viewport(*active) != content) {
    return SessionInvariantError::active_tab_viewport;
  }
  if (session.attachment.selection_target.has_value()) {
    const auto* const tab = find_tab(session, session.attachment.selection_target->tab);
    if (tab == nullptr ||
        find_pane(session, *tab, session.attachment.selection_target->pane) == nullptr) {
      return SessionInvariantError::attachment_target;
    }
  }
  if (session.attachment.mouse_capture.has_value()) {
    const auto& capture = *session.attachment.mouse_capture;
    const auto* const tab = find_tab(session, capture.target.tab);
    if (tab == nullptr || find_pane(session, *tab, capture.target.pane) == nullptr) {
      return SessionInvariantError::mouse_capture_target;
    }
  }
  return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto session_state_hash(const Session& session) noexcept -> std::uint64_t {
  auto hash = std::uint64_t{14'695'981'039'346'656'037ULL};
  hash = hash_mix(hash, id_code(session.id));
  hash = hash_mix(hash, static_cast<std::uint64_t>(session.active));
  hash = hash_mix(hash, id_code(session.active_tab));
  hash = hash_mix(hash, id_code(session.previous_tab));
  hash = hash_mix(hash, session.mutation_generation);
  for (const auto character : session.session_name()) {
    hash = hash_mix(hash, static_cast<std::uint8_t>(character));
  }
  for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
    const auto id = session.tab_order.at(position);
    hash = hash_mix(hash, id.has_value() ? id_code(*id) : 0);
  }
  for (const auto& tab_slot : session.tabs) {
    if (tab_slot.tab == nullptr) {
      continue;
    }
    const auto& tab = *tab_slot.tab;
    hash = hash_mix(hash, id_code(tab.id));
    hash = hash_mix(hash, id_code(tab.focused_pane));
    hash = hash_mix(hash, id_code(tab.previous_pane));
    hash = hash_mix(hash, tab.layout_column);
    hash = hash_mix(hash, tab.layout_row);
    hash = hash_mix(hash, tab.layout_columns);
    hash = hash_mix(hash, tab.layout_rows);
    hash = hash_mix(hash, static_cast<std::uint64_t>(tab.zoomed));
    hash = hash_mix(hash, static_cast<std::uint64_t>(tab.layout_suspended));
    const auto snapshot = tab.layout.snapshot();
    if (snapshot.has_value()) {
      for (const auto& node : std::span(snapshot->nodes).first(snapshot->size)) {
        hash = hash_mix(hash, node.leaf ? 1U : 2U);
        hash = hash_mix(hash, id_code(node.pane));
        hash = hash_mix(hash, node.ratio);
        hash = hash_mix(hash, static_cast<std::uint64_t>(node.axis));
      }
    }
  }
  for (const auto& pane_slot : session.panes) {
    if (pane_slot.pane == nullptr) {
      continue;
    }
    const auto& pane = *pane_slot.pane;
    hash = hash_mix(hash, id_code(pane.id));
    hash = hash_mix(hash, id_code(pane.tab));
    hash = hash_mix(hash, pane.rectangle.column);
    hash = hash_mix(hash, pane.rectangle.row);
    hash = hash_mix(hash, pane.rectangle.columns);
    hash = hash_mix(hash, pane.rectangle.rows);
    hash = hash_mix(hash, static_cast<std::uint64_t>(pane.exit_policy));
    if (pane.process_exit.has_value()) {
      hash = hash_mix(hash, static_cast<std::uint64_t>(pane.process_exit->kind) + 1U);
      hash = hash_mix(hash, pane.process_exit->value);
    }
  }
  return hash;
}

#ifdef __clang__
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic pop
#endif
#endif

} // namespace lemma::core
