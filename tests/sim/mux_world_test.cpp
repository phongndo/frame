#include "environment.hpp"
#include "mux_trace.hpp"
#include "random.hpp"
#include "reduce.hpp"

#include "core/layout.hpp"
#include "core/session.hpp"
#include "core/session_machine.hpp"
#include "lemma/command.hpp"
#include "lemma/geometry.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lemma::test::sim {

// Generated commands intentionally use default values for irrelevant target and launch fields.
#ifdef __clang__
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif
namespace {

using core::CreateTabOptions;
using core::PaneExitPolicy;
using core::ProcessExit;
using core::ProcessExitKind;
using core::ResizePaneEffect;
using core::RuntimeEffectStatus;
using core::Session;
using core::SessionMachine;
using core::SessionMachineOptions;
using core::SessionRuntimeEffects;
using core::SpawnPaneEffect;
using core::SplitAxis;
using core::SplitPaneOptions;

inline constexpr std::size_t mux_reduction_evaluations_default = 512;

struct SimRuntimeCoverage final {
  std::size_t spawns{0};
  std::size_t spawn_rejections{0};
  std::size_t resize_batches{0};
  std::size_t resize_rejections{0};
  std::size_t consistency_losses{0};
  std::size_t retired{0};
  std::size_t held{0};
};

class SimRuntime final {
  struct PaneState final {
    PaneId id;
    PaneRectangle rectangle;
    bool live{false};
    bool child_alive{false};
    bool held{false};
  };

public:
  [[nodiscard]] auto effects() noexcept -> SessionRuntimeEffects {
    return {.context = this,
            .spawn = &spawn_callback,
            .resize = &resize_callback,
            .retire = &retire_callback,
            .hold = &hold_callback};
  }

  void begin_operation(const RuntimeEffectStatus spawn, const RuntimeEffectStatus resize) noexcept {
    next_spawn_ = spawn;
    next_resize_ = resize;
  }
  void end_operation() noexcept {
    next_spawn_.reset();
    next_resize_.reset();
  }
  void heal() noexcept { end_operation(); }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto validate(const Session& session) const -> std::optional<std::string> {
    if (integrity_failed_) {
      return std::string{"simulated Runtime ownership operation failed"};
    }
    std::size_t semantic_count = 0;
    std::size_t runtime_count = 0;
    for (const auto& pane_slot : session.panes) {
      if (pane_slot.pane == nullptr) {
        continue;
      }
      ++semantic_count;
      const auto& pane = *pane_slot.pane;
      const auto* const runtime = get(pane.id);
      if (runtime == nullptr) {
        return std::string{"a semantic Pane has no simulated Runtime"};
      }
      if (runtime->rectangle != pane.rectangle) {
        return std::string{"Core and simulated Runtime Pane geometry differ"};
      }
      if (runtime->held != pane.process_exit.has_value() ||
          runtime->child_alive == pane.process_exit.has_value()) {
        return std::string{"child/hold Runtime state differs from Core process state"};
      }
    }
    for (const auto& runtime : panes_) {
      if (!runtime.live) {
        continue;
      }
      ++runtime_count;
      if (runtime.id.slot() >= session.panes.size()) {
        return std::string{"simulated Runtime Pane slot is out of bounds"};
      }
      const auto& pane_slot = std::span(session.panes).subspan(runtime.id.slot(), 1).front();
      if (pane_slot.pane == nullptr || pane_slot.pane->id != runtime.id) {
        return std::string{"a simulated Runtime exists for a stale Pane"};
      }
    }
    return semantic_count == runtime_count
               ? std::nullopt
               : std::optional<std::string>{"Core and Runtime Pane counts differ"};
  }

  [[nodiscard]] auto state_hash() const noexcept -> std::uint64_t {
    auto hash = std::uint64_t{14'695'981'039'346'656'037ULL};
    for (const auto& pane : panes_) {
      if (!pane.live) {
        continue;
      }
      hash ^= (static_cast<std::uint64_t>(pane.id.generation()) << 32U) | pane.id.slot();
      hash *= 1'099'511'628'211ULL;
      hash ^= pane.rectangle.columns;
      hash *= 1'099'511'628'211ULL;
      hash ^= pane.rectangle.rows;
      hash *= 1'099'511'628'211ULL;
      hash ^= static_cast<std::uint64_t>(pane.child_alive) |
              (static_cast<std::uint64_t>(pane.held) << 1U);
      hash *= 1'099'511'628'211ULL;
    }
    return hash;
  }

  [[nodiscard]] auto coverage() const noexcept -> const SimRuntimeCoverage& { return coverage_; }

private:
  [[nodiscard]] static auto selected_outcome(std::optional<RuntimeEffectStatus>& selected) noexcept
      -> RuntimeEffectStatus {
    if (!selected.has_value()) {
      return RuntimeEffectStatus::applied;
    }
    const auto outcome = *selected;
    selected.reset();
    return outcome;
  }

  [[nodiscard]] auto spawn(const SpawnPaneEffect& effect) noexcept -> RuntimeEffectStatus {
    ++coverage_.spawns;
    const auto outcome = selected_outcome(next_spawn_);
    if (outcome != RuntimeEffectStatus::applied) {
      if (outcome == RuntimeEffectStatus::rejected) {
        ++coverage_.spawn_rejections;
      } else {
        ++coverage_.consistency_losses;
      }
      return outcome;
    }
    if (!effect.pane.is_valid() || effect.pane.slot() >= panes_.size()) {
      ++coverage_.consistency_losses;
      return RuntimeEffectStatus::consistency_lost;
    }
    auto& pane = std::span(panes_).subspan(effect.pane.slot(), 1).front();
    if (pane.live) {
      ++coverage_.consistency_losses;
      return RuntimeEffectStatus::consistency_lost;
    }
    pane = {.id = effect.pane,
            .rectangle = effect.rectangle,
            .live = true,
            .child_alive = true,
            .held = false};
    return RuntimeEffectStatus::applied;
  }

  [[nodiscard]] auto resize(const std::span<const ResizePaneEffect> effects) noexcept
      -> RuntimeEffectStatus {
    ++coverage_.resize_batches;
    const auto outcome = selected_outcome(next_resize_);
    if (outcome != RuntimeEffectStatus::applied) {
      if (outcome == RuntimeEffectStatus::rejected) {
        ++coverage_.resize_rejections;
      } else {
        ++coverage_.consistency_losses;
      }
      return outcome;
    }
    for (const auto& effect : effects) {
      const auto* const pane = get(effect.pane);
      if (pane == nullptr || pane->rectangle != effect.previous) {
        ++coverage_.consistency_losses;
        return RuntimeEffectStatus::consistency_lost;
      }
    }
    for (const auto& effect : effects) {
      auto* const pane = get(effect.pane);
      if (pane == nullptr) {
        ++coverage_.consistency_losses;
        return RuntimeEffectStatus::consistency_lost;
      }
      pane->rectangle = effect.target;
    }
    return RuntimeEffectStatus::applied;
  }

  void retire(const PaneId pane_id) noexcept {
    auto* const pane = get(pane_id);
    if (pane == nullptr) {
      integrity_failed_ = true;
      return;
    }
    *pane = {};
    ++coverage_.retired;
  }

  void hold(const PaneId pane_id) noexcept {
    auto* const pane = get(pane_id);
    if (pane == nullptr || pane->held) {
      integrity_failed_ = true;
      return;
    }
    pane->child_alive = false;
    pane->held = true;
    ++coverage_.held;
  }

  [[nodiscard]] auto get(const PaneId pane) noexcept -> PaneState* {
    if (!pane.is_valid() || pane.slot() >= panes_.size()) {
      return nullptr;
    }
    auto& state = std::span(panes_).subspan(pane.slot(), 1).front();
    return state.live && state.id == pane ? &state : nullptr;
  }

  [[nodiscard]] auto get(const PaneId pane) const noexcept -> const PaneState* {
    if (!pane.is_valid() || pane.slot() >= panes_.size()) {
      return nullptr;
    }
    const auto& state = std::span(panes_).subspan(pane.slot(), 1).front();
    return state.live && state.id == pane ? &state : nullptr;
  }

  static auto spawn_callback(void* const context, const SpawnPaneEffect& effect) noexcept
      -> RuntimeEffectStatus {
    return static_cast<SimRuntime*>(context)->spawn(effect);
  }

  static auto resize_callback(void* const context,
                              const std::span<const ResizePaneEffect> effects) noexcept
      -> RuntimeEffectStatus {
    return static_cast<SimRuntime*>(context)->resize(effects);
  }

  static void retire_callback(void* const context, [[maybe_unused]] const SessionId session,
                              const PaneId pane) noexcept {
    static_cast<SimRuntime*>(context)->retire(pane);
  }

  static void hold_callback(void* const context, [[maybe_unused]] const SessionId session,
                            const PaneId pane,
                            [[maybe_unused]] const ProcessExit process) noexcept {
    static_cast<SimRuntime*>(context)->hold(pane);
  }

  std::array<PaneState, core::panes_per_session_max> panes_{};
  std::optional<RuntimeEffectStatus> next_spawn_;
  std::optional<RuntimeEffectStatus> next_resize_;
  SimRuntimeCoverage coverage_;
  bool integrity_failed_{false};
};

inline constexpr std::size_t mux_operation_kind_count =
    static_cast<std::size_t>(MuxOperationKind::idle) + 1U;
inline constexpr std::size_t mux_outcome_class_count = 4;
inline constexpr std::size_t mux_fault_class_count = 4;
inline constexpr std::size_t mux_state_class_count = 13;
inline constexpr std::array<std::string_view, mux_outcome_class_count> mux_outcome_names{
    "applied", "no_effect", "stale", "rejected"};
inline constexpr std::array<std::string_view, mux_fault_class_count> mux_fault_names{
    "none", "spawn", "resize", "spawn_and_resize"};
inline constexpr std::array<std::string_view, mux_state_class_count> mux_state_names{
    "single_pane",        "multiple_panes",    "pane_capacity", "single_tab",
    "multiple_tabs",      "tab_capacity",      "zoomed",        "layout_suspended",
    "stale_id_retained",  "generation_reused", "held_child",    "minimum_attachment",
    "maximum_attachment",
};

inline constexpr std::size_t outcome_applied_index = 0;
inline constexpr std::size_t outcome_stale_index = 2;
inline constexpr std::size_t outcome_rejected_index = 3;
inline constexpr std::size_t fault_none_index = 0;
inline constexpr std::size_t fault_spawn_index = 1;
inline constexpr std::size_t fault_resize_index = 2;
inline constexpr std::size_t state_multiple_panes_index = 1;
inline constexpr std::size_t state_pane_capacity_index = 2;
inline constexpr std::size_t state_tab_capacity_index = 5;
inline constexpr std::size_t state_stale_id_retained_index = 8;
inline constexpr std::size_t state_held_child_index = 10;
inline constexpr std::size_t state_minimum_attachment_index = 11;
inline constexpr std::size_t state_maximum_attachment_index = 12;
inline constexpr std::uint16_t state_single_pane = 1U << 0U;
inline constexpr std::uint16_t state_multiple_panes = 1U << 1U;
inline constexpr std::uint16_t state_pane_capacity = 1U << state_pane_capacity_index;
inline constexpr std::uint16_t state_single_tab = 1U << 3U;
inline constexpr std::uint16_t state_multiple_tabs = 1U << 4U;
inline constexpr std::uint16_t state_tab_capacity = 1U << state_tab_capacity_index;
inline constexpr std::uint16_t state_zoomed = 1U << 6U;
inline constexpr std::uint16_t state_layout_suspended = 1U << 7U;
inline constexpr std::uint16_t state_stale_id_retained = 1U << 8U;
inline constexpr std::uint16_t state_generation_reused = 1U << 9U;
inline constexpr std::uint16_t state_held_child = 1U << 10U;
inline constexpr std::uint16_t state_minimum_attachment = 1U << 11U;
inline constexpr std::uint16_t state_maximum_attachment = 1U << state_maximum_attachment_index;

using MuxStateSignatureCounts = std::array<
    std::array<std::array<std::array<std::size_t, mux_state_class_count>, mux_fault_class_count>,
               mux_outcome_class_count>,
    mux_operation_kind_count>;

struct WorldCoverage final {
  std::size_t applied{0};
  std::size_t no_effect{0};
  std::size_t rejected{0};
  std::size_t stale{0};
  std::size_t child_exits{0};
  std::size_t runtime_errors{0};
  std::array<std::size_t, mux_operation_kind_count> operations{};
  std::array<std::array<std::size_t, mux_outcome_class_count>, mux_operation_kind_count> outcomes{};
  std::array<std::array<std::size_t, mux_operation_kind_count>, mux_operation_kind_count>
      transitions{};
  std::array<std::array<std::size_t, mux_fault_class_count>, mux_operation_kind_count> faults{};
  std::array<std::array<std::size_t, mux_state_class_count>, mux_operation_kind_count>
      operation_states{};
  MuxStateSignatureCounts signatures{};
  std::array<std::size_t, mux_operation_kind_count> spawn_faults{};
  std::array<std::size_t, mux_operation_kind_count> resize_faults{};
  std::uint64_t states{0};

  void merge_operation(const WorldCoverage& other, const std::size_t operation) {
    operations.at(operation) += other.operations.at(operation);
    spawn_faults.at(operation) += other.spawn_faults.at(operation);
    resize_faults.at(operation) += other.resize_faults.at(operation);
    for (std::size_t outcome = 0; outcome < mux_outcome_class_count; ++outcome) {
      outcomes.at(operation).at(outcome) += other.outcomes.at(operation).at(outcome);
    }
    for (std::size_t fault = 0; fault < mux_fault_class_count; ++fault) {
      faults.at(operation).at(fault) += other.faults.at(operation).at(fault);
      for (std::size_t outcome = 0; outcome < mux_outcome_class_count; ++outcome) {
        for (std::size_t state = 0; state < mux_state_class_count; ++state) {
          signatures.at(operation).at(outcome).at(fault).at(state) +=
              other.signatures.at(operation).at(outcome).at(fault).at(state);
        }
      }
    }
    for (std::size_t state = 0; state < mux_state_class_count; ++state) {
      operation_states.at(operation).at(state) += other.operation_states.at(operation).at(state);
    }
    for (std::size_t next = 0; next < mux_operation_kind_count; ++next) {
      transitions.at(operation).at(next) += other.transitions.at(operation).at(next);
    }
  }

  void merge(const WorldCoverage& other) {
    applied += other.applied;
    no_effect += other.no_effect;
    rejected += other.rejected;
    stale += other.stale;
    child_exits += other.child_exits;
    runtime_errors += other.runtime_errors;
    states |= other.states;
    for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
      merge_operation(other, operation);
    }
  }
};

class MuxWorld final {
public:
  MuxWorld() : session_("sim", {}, {}, core::LaunchEnvironmentMode::inherit) {
    session_.id = SessionId::from_parts(0, 1);
    session_.attachment.id = AttachmentId::from_parts(0, 1);
    session_.attachment.session = session_.id;
    session_.attachment.columns = 120;
    session_.attachment.rows = 40;
    runtime_.begin_operation(RuntimeEffectStatus::applied, RuntimeEffectStatus::applied);
    SessionMachine machine(session_, machine_options());
    const auto created = machine.create_tab();
    runtime_.end_operation();
    if (created.result.status != CommandStatus::applied) {
      std::abort();
    }
  }

  // Generation is state-aware, but its output is a complete operation value. Execution and replay
  // consume only that value and never depend on generator draw order.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto generate(Random& operations, Random& faults) -> MuxOperation {
    MuxOperation operation{
        .spawn_outcome =
            faults.index(32U) == 0 ? RuntimeEffectStatus::rejected : RuntimeEffectStatus::applied,
        .resize_outcome =
            faults.index(24U) == 0 ? RuntimeEffectStatus::rejected : RuntimeEffectStatus::applied,
    };

    // Every 256-operation epoch first drives the world through maximum geometry, Tab capacity,
    // Pane capacity, and rejected-at-capacity transitions. Random histories then explore paths out
    // of those states. This guarantees deep-state search without making replay depend on a model.
    const auto directed_step = generated_operations_++ % 256U;
    if (directed_step == 0U) {
      operation.kind = MuxOperationKind::attachment_resize;
      operation.argument_0 = limits::terminal_columns_hard_max;
      operation.argument_1 = limits::terminal_rows_hard_max;
      return operation;
    }
    if (directed_step <= core::tabs_per_session_max &&
        session_.tab_order.size() < core::tabs_per_session_max) {
      operation.kind = MuxOperationKind::create_tab;
      operation.argument_0 = static_cast<std::uint16_t>(
          directed_step % 4U == 0 ? PaneExitPolicy::hold : PaneExitPolicy::close);
      operation.spawn_outcome = RuntimeEffectStatus::applied;
      operation.resize_outcome = RuntimeEffectStatus::applied;
      return operation;
    }
    if (directed_step <= core::tabs_per_session_max + core::panes_per_session_max &&
        pane_count() < core::panes_per_session_max) {
      operation.kind = MuxOperationKind::split;
      select_tab_and_pane(operations, operation);
      operation.argument_0 = static_cast<std::uint16_t>(
          operations.boolean() ? SplitAxis::left_right : SplitAxis::top_bottom);
      operation.argument_1 = static_cast<std::uint16_t>(
          directed_step % 5U == 0 ? PaneExitPolicy::hold : PaneExitPolicy::close);
      operation.spawn_outcome = RuntimeEffectStatus::applied;
      operation.resize_outcome = RuntimeEffectStatus::applied;
      return operation;
    }
    if (directed_step == core::tabs_per_session_max + core::panes_per_session_max + 1U) {
      operation.kind = MuxOperationKind::create_tab;
      operation.argument_0 = static_cast<std::uint16_t>(PaneExitPolicy::close);
      operation.spawn_outcome = RuntimeEffectStatus::applied;
      operation.resize_outcome = RuntimeEffectStatus::applied;
      return operation;
    }
    if (directed_step == core::tabs_per_session_max + core::panes_per_session_max + 2U) {
      operation.kind = MuxOperationKind::idle;
      return operation;
    }

    switch (operations.index(mux_operation_kind_count)) {
    case 0:
    case 1: {
      operation.kind = MuxOperationKind::split;
      auto* const tab = random_tab(operations);
      auto* const pane = tab == nullptr ? nullptr : random_pane(*tab, operations);
      if (tab != nullptr && pane != nullptr) {
        operation.tab = tab->id;
        operation.pane = pane->id;
        operation.argument_0 = static_cast<std::uint16_t>(
            operations.boolean() ? SplitAxis::left_right : SplitAxis::top_bottom);
        operation.argument_1 = static_cast<std::uint16_t>(
            operations.index(5) == 0 ? PaneExitPolicy::hold : PaneExitPolicy::close);
      }
      break;
    }
    case 2: {
      operation.kind = MuxOperationKind::close_pane;
      if (pane_count() > 1U) {
        auto* const tab = random_tab(operations);
        auto* const pane = tab == nullptr ? nullptr : random_pane(*tab, operations);
        if (tab != nullptr && pane != nullptr) {
          operation.tab = tab->id;
          operation.pane = pane->id;
        }
      }
      break;
    }
    case 3: {
      operation.kind = MuxOperationKind::focus;
      select_tab_and_pane(operations, operation);
      break;
    }
    case 4: {
      operation.kind = MuxOperationKind::zoom;
      select_tab_and_pane(operations, operation);
      break;
    }
    case 5:
      operation.kind = MuxOperationKind::create_tab;
      operation.argument_0 = static_cast<std::uint16_t>(
          operations.index(4) == 0 ? PaneExitPolicy::hold : PaneExitPolicy::close);
      break;
    case 6: {
      operation.kind = MuxOperationKind::close_tab;
      if (session_.tab_order.size() > 1U) {
        auto* const tab = random_tab(operations);
        if (tab != nullptr) {
          operation.tab = tab->id;
        }
      }
      break;
    }
    case 7: {
      operation.kind = MuxOperationKind::select_tab;
      auto* const tab = random_tab(operations);
      if (tab != nullptr) {
        operation.tab = tab->id;
      }
      break;
    }
    case 8: {
      operation.kind = MuxOperationKind::place_tab;
      auto* const tab = random_tab(operations);
      auto* const anchor = random_tab(operations);
      if (tab != nullptr) {
        operation.tab = tab->id;
        operation.peer_tab = anchor == nullptr ? TabId{} : anchor->id;
      }
      break;
    }
    case 9: {
      operation.kind = MuxOperationKind::swap;
      auto* const tab = random_tab_with_multiple_panes(operations);
      if (tab != nullptr) {
        auto* const first = random_pane(*tab, operations);
        auto* second = random_pane(*tab, operations);
        if (first != nullptr && second != nullptr && first->id == second->id) {
          second = next_pane(*tab, first->id);
        }
        if (first != nullptr && second != nullptr) {
          operation.tab = tab->id;
          operation.pane = first->id;
          operation.peer_pane = second->id;
        }
      }
      break;
    }
    case 10: {
      operation.kind = MuxOperationKind::resize;
      select_tab_and_pane(operations, operation);
      constexpr std::array kinds{CommandKind::resize_left, CommandKind::resize_right,
                                 CommandKind::resize_up, CommandKind::resize_down};
      operation.argument_0 = static_cast<std::uint16_t>(
          std::span(kinds).subspan(operations.index(kinds.size()), 1).front());
      constexpr std::array<std::uint16_t, 4> resize_boundaries{
          1,
          2,
          command_resize_amount_max - 1U,
          command_resize_amount_max,
      };
      operation.argument_1 = operations.index(3U) == 0
                                 ? operations.between(1, command_resize_amount_max)
                                 : resize_boundaries.at(operations.index(resize_boundaries.size()));
      break;
    }
    case 11: {
      operation.kind = MuxOperationKind::stale_focus;
      auto* const tab = random_tab(operations);
      if (tab != nullptr) {
        operation.tab = tab->id;
        operation.pane =
            stale_count_ == 0
                ? PaneId::from_parts(tab->focused_pane.slot(), tab->focused_pane.generation() + 1U)
                : std::span(stale_panes_).subspan(operations.index(stale_count_), 1).front();
      }
      break;
    }
    case 12: {
      operation.kind = MuxOperationKind::spawn_failure;
      operation.spawn_outcome = RuntimeEffectStatus::rejected;
      select_tab_and_pane(operations, operation);
      break;
    }
    case 13: {
      operation.kind = MuxOperationKind::resize_failure;
      operation.resize_outcome = RuntimeEffectStatus::rejected;
      select_tab_and_pane(operations, operation);
      break;
    }
    case 14:
      operation.kind = MuxOperationKind::child_exit;
      if (pane_count() > 1U) {
        select_tab_and_pane(operations, operation);
        operation.argument_0 = static_cast<std::uint16_t>(operations.index(128));
      }
      break;
    case 15:
      operation.kind = MuxOperationKind::runtime_error;
      if (pane_count() > 1U) {
        select_tab_and_pane(operations, operation);
      }
      break;
    case 16: {
      operation.kind = MuxOperationKind::attachment_resize;
      constexpr std::array<std::uint16_t, 6> column_boundaries{
          1, 2, 20, 180, limits::terminal_columns_hard_max - 1U, limits::terminal_columns_hard_max};
      constexpr std::array<std::uint16_t, 6> row_boundaries{
          1, 2, 3, 80, limits::terminal_rows_hard_max - 1U, limits::terminal_rows_hard_max};
      operation.argument_0 = column_boundaries.at(operations.index(column_boundaries.size()));
      operation.argument_1 = row_boundaries.at(operations.index(row_boundaries.size()));
      break;
    }
    default:
      operation.kind = MuxOperationKind::idle;
      break;
    }
    return operation;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto apply(const MuxOperation& operation) -> std::optional<std::string> {
    last_operation_ = mux_operation_name(operation.kind);
    if (!session_.active) {
      last_transition_ = {};
      return check_all_invariants();
    }
    runtime_.begin_operation(operation.spawn_outcome, operation.resize_outcome);
    SessionMachine machine(session_, machine_options());
    core::SessionTransition transition;
    std::optional<std::string> error;
    switch (operation.kind) {
    case MuxOperationKind::split: {
      auto* const tab = find_tab(operation.tab);
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (tab != nullptr && pane != nullptr) {
        transition = machine.split_pane(
            tab->id, pane->id, static_cast<SplitAxis>(operation.argument_0),
            SplitPaneOptions{.exit_policy = static_cast<PaneExitPolicy>(operation.argument_1)});
      }
      break;
    }
    case MuxOperationKind::close_pane: {
      auto* const tab = find_tab(operation.tab);
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (pane_count() > 1U && tab != nullptr && pane != nullptr) {
        retain_stale(pane->id);
        transition = dispatch(machine, CommandKind::close_pane, tab->id, pane->id);
      }
      break;
    }
    case MuxOperationKind::focus: {
      auto* const tab = find_tab(operation.tab);
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (tab != nullptr && pane != nullptr) {
        transition = dispatch(machine, CommandKind::focus_pane, tab->id, pane->id);
      }
      break;
    }
    case MuxOperationKind::zoom: {
      auto* const tab = find_tab(operation.tab);
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (tab != nullptr && pane != nullptr) {
        transition = dispatch(machine, CommandKind::toggle_zoom, tab->id, pane->id);
      }
      break;
    }
    case MuxOperationKind::create_tab:
      transition = machine.create_tab(
          CreateTabOptions{.exit_policy = static_cast<PaneExitPolicy>(operation.argument_0)});
      break;
    case MuxOperationKind::close_tab: {
      auto* const tab = find_tab(operation.tab);
      if (session_.tab_order.size() > 1U && tab != nullptr) {
        retain_tab_panes(*tab);
        transition = dispatch(machine, CommandKind::close_tab, tab->id, {});
      }
      break;
    }
    case MuxOperationKind::select_tab: {
      auto* const tab = find_tab(operation.tab);
      if (tab != nullptr) {
        transition = dispatch(machine, CommandKind::select_tab, tab->id, {});
      }
      break;
    }
    case MuxOperationKind::place_tab: {
      auto* const tab = find_tab(operation.tab);
      if (tab != nullptr) {
        Command command{
            .kind = CommandKind::place_tab,
            .origin = CommandOrigin::internal,
            .target = {.session = session_.id, .tab = tab->id},
            .payload = TabPlacementCommand{.before = operation.peer_tab},
        };
        transition = machine.dispatch(command);
      }
      break;
    }
    case MuxOperationKind::swap: {
      auto* const tab = find_tab(operation.tab);
      auto* const pane = find_pane(operation.pane, operation.tab);
      auto* const peer = find_pane(operation.peer_pane, operation.tab);
      if (tab != nullptr && pane != nullptr && peer != nullptr) {
        Command command{
            .kind = CommandKind::swap_panes,
            .origin = CommandOrigin::internal,
            .target = {.session = session_.id, .tab = tab->id, .pane = pane->id},
            .payload = PaneSwapCommand{.other = peer->id},
        };
        transition = machine.dispatch(command);
      }
      break;
    }
    case MuxOperationKind::resize: {
      auto* const tab = find_tab(operation.tab);
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (tab != nullptr && pane != nullptr) {
        Command command{
            .kind = static_cast<CommandKind>(operation.argument_0),
            .origin = CommandOrigin::internal,
            .target = {.session = session_.id, .tab = tab->id, .pane = pane->id},
            .payload = CommandCoordinate{.value = operation.argument_1},
        };
        transition = machine.dispatch(command);
      }
      break;
    }
    case MuxOperationKind::stale_focus: {
      auto* const tab = find_tab(operation.tab);
      if (tab != nullptr) {
        const auto before = state_hash();
        transition = dispatch(machine, CommandKind::focus_pane, tab->id, operation.pane);
        if (transition.result.status != CommandStatus::stale_target || state_hash() != before) {
          error = "stale Pane command resolved or mutated state";
        }
      }
      break;
    }
    case MuxOperationKind::spawn_failure: {
      auto* const tab = find_tab(operation.tab);
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (tab != nullptr && pane != nullptr && pane_count() < core::panes_per_session_max) {
        const auto before = state_hash();
        transition = machine.split_pane(tab->id, pane->id, SplitAxis::left_right);
        if (transition.result.status != CommandStatus::unavailable || state_hash() != before) {
          error = "spawn rejection did not preserve atomic Core/Runtime state";
        }
      }
      break;
    }
    case MuxOperationKind::resize_failure: {
      auto* const tab = find_tab(operation.tab);
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (tab != nullptr && pane != nullptr) {
        const auto before = state_hash();
        Command command{
            .kind = CommandKind::set_zoom,
            .origin = CommandOrigin::internal,
            .target = {.session = session_.id, .tab = tab->id, .pane = pane->id},
            .payload = PaneZoomCommand{.enabled = !tab->zoomed},
        };
        transition = machine.dispatch(command);
        if (transition.result.status != CommandStatus::unavailable || state_hash() != before) {
          error = "resize rejection did not preserve atomic Core/Runtime state";
        }
      }
      break;
    }
    case MuxOperationKind::child_exit: {
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (pane_count() > 1U && pane != nullptr) {
        const auto stale = pane->id;
        const bool held = pane->exit_policy == PaneExitPolicy::hold;
        transition = machine.runtime_failed(
            pane->id, {.kind = ProcessExitKind::exited, .value = operation.argument_0}, true);
        ++coverage_.child_exits;
        if (!held) {
          retain_stale(stale);
        }
      }
      break;
    }
    case MuxOperationKind::runtime_error: {
      auto* const pane = find_pane(operation.pane, operation.tab);
      if (pane_count() > 1U && pane != nullptr) {
        retain_stale(pane->id);
        transition = machine.runtime_failed(pane->id, {}, false);
        ++coverage_.runtime_errors;
      }
      break;
    }
    case MuxOperationKind::attachment_resize:
      transition = machine.resize_attachment(operation.argument_0, operation.argument_1);
      break;
    case MuxOperationKind::idle:
      break;
    }
    runtime_.end_operation();
    last_transition_ = transition;
    observe(operation, transition);
    if (error.has_value()) {
      return error;
    }
    return check_all_invariants();
  }

  [[nodiscard]] auto check_all_invariants() const -> std::optional<std::string> {
    if (const auto invariant = core::check_session_invariants(session_); invariant.has_value()) {
      std::ostringstream diagnostic;
      diagnostic << core::session_invariant_name(*invariant)
                 << "; attachment=" << session_.attachment.columns << 'x'
                 << session_.attachment.rows;
      for (const auto& tab_slot : session_.tabs) {
        if (tab_slot.tab == nullptr) {
          continue;
        }
        const auto& tab = *tab_slot.tab;
        diagnostic << "; tab=" << tab.id.slot() << ':' << tab.id.generation()
                   << " layout=" << tab.layout_column << ',' << tab.layout_row << '+'
                   << tab.layout_columns << 'x' << tab.layout_rows
                   << " focus=" << tab.focused_pane.slot() << ':' << tab.focused_pane.generation()
                   << " previous=" << tab.previous_pane.slot() << ':'
                   << tab.previous_pane.generation() << " zoom=" << tab.zoomed
                   << " suspended=" << tab.layout_suspended;
      }
      for (const auto& pane_slot : session_.panes) {
        if (pane_slot.pane == nullptr) {
          continue;
        }
        const auto& pane = *pane_slot.pane;
        diagnostic << "; pane=" << pane.id.slot() << ':' << pane.id.generation()
                   << " tab=" << pane.tab.slot() << ':' << pane.tab.generation()
                   << " rectangle=" << pane.rectangle.column << ',' << pane.rectangle.row << '+'
                   << pane.rectangle.columns << 'x' << pane.rectangle.rows
                   << " exited=" << pane.process_exit.has_value();
      }
      return diagnostic.str();
    }
    return runtime_.validate(session_);
  }

  [[nodiscard]] auto state_hash() const noexcept -> std::uint64_t {
    return core::session_state_hash(session_) ^ (runtime_.state_hash() * 0x9E3779B97F4A7C15ULL);
  }

  [[nodiscard]] auto last_operation() const noexcept -> std::string_view { return last_operation_; }
  [[nodiscard]] auto last_transition() const noexcept -> const core::SessionTransition& {
    return last_transition_;
  }
  [[nodiscard]] auto coverage() const noexcept -> const WorldCoverage& { return coverage_; }
  [[nodiscard]] auto runtime_coverage() const noexcept -> const SimRuntimeCoverage& {
    return runtime_.coverage();
  }

  [[nodiscard]] auto heal() -> std::optional<std::string> {
    runtime_.heal();
    SessionMachine machine(session_, machine_options());
    if (session_.tab_order.size() == core::tabs_per_session_max) {
      const auto tab = session_.tab_order.at(session_.tab_order.size() - 1U);
      if (!tab.has_value() || machine.dispatch({.kind = CommandKind::close_tab,
                                                .origin = CommandOrigin::internal,
                                                .target = {.session = session_.id, .tab = *tab}})
                                      .result.status != CommandStatus::applied) {
        return std::string{"healed world could not release Tab capacity"};
      }
    }
    if (pane_count() == core::panes_per_session_max) {
      auto* const tab = first_tab_with_multiple_panes();
      auto* const pane = tab == nullptr ? nullptr : first_pane(*tab);
      if (tab == nullptr || pane == nullptr ||
          dispatch(machine, CommandKind::close_pane, tab->id, pane->id).result.status !=
              CommandStatus::applied) {
        return std::string{"healed world could not release Pane capacity"};
      }
    }
    const auto progress = machine.create_tab();
    if (progress.result.status != CommandStatus::applied) {
      return std::string{"healed world did not make bounded lifecycle progress"};
    }
    return check_all_invariants();
  }

  [[nodiscard]] auto force_consistency_loss() -> core::SessionTransition {
    auto* const tab = active_tab();
    auto* const pane = tab == nullptr ? nullptr : first_pane(*tab);
    if (pane == nullptr) {
      return {};
    }
    runtime_.begin_operation(RuntimeEffectStatus::applied, RuntimeEffectStatus::consistency_lost);
    Command command{
        .kind = CommandKind::set_zoom,
        .origin = CommandOrigin::internal,
        .target = {.session = session_.id, .tab = tab->id, .pane = pane->id},
        .payload = PaneZoomCommand{.enabled = !tab->zoomed},
    };
    SessionMachine machine(session_, machine_options());
    const auto transition = machine.dispatch(command);
    runtime_.end_operation();
    return transition;
  }

private:
  [[nodiscard]] auto machine_options() noexcept -> SessionMachineOptions {
    return {.runtime = runtime_.effects(),
            .name_conflict = &no_name_conflict,
            .name_conflict_context = this};
  }

  static auto no_name_conflict([[maybe_unused]] void* context, [[maybe_unused]] SessionId renamed,
                               [[maybe_unused]] std::string_view candidate) noexcept -> bool {
    return false;
  }

  [[nodiscard]] auto active_tab() noexcept -> core::Tab* { return find_tab(session_.active_tab); }

  [[nodiscard]] auto find_tab(const TabId id) noexcept -> core::Tab* {
    if (!id.is_valid() || id.slot() >= session_.tabs.size()) {
      return nullptr;
    }
    auto& slot = std::span(session_.tabs).subspan(id.slot(), 1).front();
    return slot.tab != nullptr && slot.tab->id == id ? slot.tab.get() : nullptr;
  }

  [[nodiscard]] auto find_pane(const PaneId id, const TabId tab) noexcept -> core::Pane* {
    if (!id.is_valid() || id.slot() >= session_.panes.size()) {
      return nullptr;
    }
    auto& slot = std::span(session_.panes).subspan(id.slot(), 1).front();
    return slot.pane != nullptr && slot.pane->id == id && slot.pane->tab == tab ? slot.pane.get()
                                                                                : nullptr;
  }

  [[nodiscard]] auto random_tab(Random& random) noexcept -> core::Tab* {
    if (session_.tab_order.empty()) {
      return nullptr;
    }
    const auto id = session_.tab_order.at(random.index(session_.tab_order.size()));
    return id.has_value() ? find_tab(*id) : nullptr;
  }

  [[nodiscard]] auto random_tab_with_multiple_panes(Random& random) noexcept -> core::Tab* {
    std::array<core::Tab*, core::tabs_per_session_max> candidates{};
    std::size_t count = 0;
    for (auto& tab_slot : session_.tabs) {
      if (tab_slot.tab != nullptr && tab_slot.tab->layout.pane_count() > 1U) {
        std::span(candidates).subspan(count, 1).front() = tab_slot.tab.get();
        ++count;
      }
    }
    return count == 0 ? nullptr : std::span(candidates).subspan(random.index(count), 1).front();
  }

  [[nodiscard]] auto first_tab_with_multiple_panes() noexcept -> core::Tab* {
    for (auto& tab_slot : session_.tabs) {
      if (tab_slot.tab != nullptr && tab_slot.tab->layout.pane_count() > 1U) {
        return tab_slot.tab.get();
      }
    }
    return nullptr;
  }

  [[nodiscard]] auto random_pane(const core::Tab& tab, Random& random) noexcept -> core::Pane* {
    std::array<core::Pane*, core::panes_per_session_max> candidates{};
    std::size_t count = 0;
    for (auto& pane_slot : session_.panes) {
      if (pane_slot.pane != nullptr && pane_slot.pane->tab == tab.id) {
        std::span(candidates).subspan(count, 1).front() = pane_slot.pane.get();
        ++count;
      }
    }
    return count == 0 ? nullptr : std::span(candidates).subspan(random.index(count), 1).front();
  }

  [[nodiscard]] auto first_pane(const core::Tab& tab) noexcept -> core::Pane* {
    for (auto& pane_slot : session_.panes) {
      if (pane_slot.pane != nullptr && pane_slot.pane->tab == tab.id) {
        return pane_slot.pane.get();
      }
    }
    return nullptr;
  }

  [[nodiscard]] auto next_pane(const core::Tab& tab, const PaneId excluded) noexcept
      -> core::Pane* {
    for (auto& pane_slot : session_.panes) {
      if (pane_slot.pane != nullptr && pane_slot.pane->tab == tab.id &&
          pane_slot.pane->id != excluded) {
        return pane_slot.pane.get();
      }
    }
    return nullptr;
  }

  void select_tab_and_pane(Random& random, MuxOperation& operation) noexcept {
    auto* const tab = random_tab(random);
    auto* const pane = tab == nullptr ? nullptr : random_pane(*tab, random);
    if (tab != nullptr && pane != nullptr) {
      operation.tab = tab->id;
      operation.pane = pane->id;
    }
  }

  [[nodiscard]] auto pane_count() const noexcept -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        session_.panes, [](const core::PaneSlot& slot) { return slot.pane != nullptr; }));
  }

  void retain_stale(const PaneId pane) noexcept {
    std::span(stale_panes_).subspan(stale_next_, 1).front() = pane;
    stale_next_ = (stale_next_ + 1U) % stale_panes_.size();
    stale_count_ = std::min(stale_count_ + 1U, stale_panes_.size());
  }

  void retain_tab_panes(const core::Tab& tab) noexcept {
    for (const auto& pane_slot : session_.panes) {
      if (pane_slot.pane != nullptr && pane_slot.pane->tab == tab.id) {
        retain_stale(pane_slot.pane->id);
      }
    }
  }

  [[nodiscard]] auto dispatch(SessionMachine& machine, const CommandKind kind, const TabId tab,
                              const PaneId pane) noexcept -> core::SessionTransition {
    return machine.dispatch({.kind = kind,
                             .origin = CommandOrigin::internal,
                             .target = {.session = session_.id, .tab = tab, .pane = pane}});
  }

  [[nodiscard]] static constexpr auto outcome_class(const CommandStatus status) noexcept
      -> std::size_t {
    switch (status) {
    case CommandStatus::applied:
      return 0;
    case CommandStatus::no_effect:
      return 1;
    case CommandStatus::stale_target:
      return 2;
    case CommandStatus::detach_requested:
    case CommandStatus::invalid_command:
    case CommandStatus::invalid_target:
    case CommandStatus::wrong_owner:
    case CommandStatus::conflict:
    case CommandStatus::capacity:
    case CommandStatus::unavailable:
    case CommandStatus::failed:
      return 3;
    }
    return 3;
  }

  // This test-only bitset deliberately states every independently accountable bucket.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto state_coverage() const noexcept -> std::uint64_t {
    std::uint64_t states = 0;
    const auto panes = pane_count();
    const auto tabs = session_.tab_order.size();
    states |= panes == 1U ? state_single_pane : 0U;
    states |= panes > 1U ? state_multiple_panes : 0U;
    states |= panes == core::panes_per_session_max ? state_pane_capacity : 0U;
    states |= tabs == 1U ? state_single_tab : 0U;
    states |= tabs > 1U ? state_multiple_tabs : 0U;
    states |= tabs == core::tabs_per_session_max ? state_tab_capacity : 0U;
    states |= stale_count_ > 0 ? state_stale_id_retained : 0U;
    states |= session_.attachment.columns == 1U || session_.attachment.rows == 1U
                  ? state_minimum_attachment
                  : 0U;
    states |= session_.attachment.columns == limits::terminal_columns_hard_max &&
                      session_.attachment.rows == limits::terminal_rows_hard_max
                  ? state_maximum_attachment
                  : 0U;
    for (const auto& tab_slot : session_.tabs) {
      if (tab_slot.tab != nullptr) {
        states |= tab_slot.tab->zoomed ? state_zoomed : 0U;
        states |= tab_slot.tab->layout_suspended ? state_layout_suspended : 0U;
      }
    }
    for (const auto& pane_slot : session_.panes) {
      if (pane_slot.pane == nullptr) {
        continue;
      }
      states |= pane_slot.pane->process_exit.has_value() ? state_held_child : 0U;
      for (const auto stale : std::span(stale_panes_).first(stale_count_)) {
        if (stale.is_valid() && stale.slot() == pane_slot.pane->id.slot() &&
            stale.generation() != pane_slot.pane->id.generation()) {
          states |= state_generation_reused;
        }
      }
    }
    return states;
  }

  void observe(const MuxOperation& operation, const core::SessionTransition& transition) {
    const auto operation_index = static_cast<std::size_t>(operation.kind);
    const auto operation_outcome = outcome_class(transition.result.status);
    ++coverage_.operations.at(operation_index);
    ++coverage_.outcomes.at(operation_index).at(operation_outcome);
    if (previous_operation_.has_value()) {
      ++coverage_.transitions.at(static_cast<std::size_t>(*previous_operation_))
            .at(operation_index);
    }
    previous_operation_ = operation.kind;
    const bool spawn_fault = operation.spawn_outcome != RuntimeEffectStatus::applied;
    const bool resize_fault = operation.resize_outcome != RuntimeEffectStatus::applied;
    if (spawn_fault) {
      ++coverage_.spawn_faults.at(operation_index);
    }
    if (resize_fault) {
      ++coverage_.resize_faults.at(operation_index);
    }
    const auto fault_class =
        static_cast<std::size_t>(spawn_fault) | (static_cast<std::size_t>(resize_fault) << 1U);
    ++coverage_.faults.at(operation_index).at(fault_class);
    const auto current_states = state_coverage();
    coverage_.states |= current_states;
    for (std::size_t state = 0; state < mux_state_class_count; ++state) {
      if ((current_states & (std::uint64_t{1} << state)) != 0U) {
        ++coverage_.operation_states.at(operation_index).at(state);
        ++coverage_.signatures.at(operation_index).at(operation_outcome).at(fault_class).at(state);
      }
    }
    switch (transition.result.status) {
    case CommandStatus::applied:
      ++coverage_.applied;
      break;
    case CommandStatus::no_effect:
      ++coverage_.no_effect;
      break;
    case CommandStatus::stale_target:
      ++coverage_.stale;
      break;
    case CommandStatus::capacity:
    case CommandStatus::unavailable:
    case CommandStatus::failed:
      ++coverage_.rejected;
      break;
    case CommandStatus::detach_requested:
    case CommandStatus::invalid_command:
    case CommandStatus::invalid_target:
    case CommandStatus::wrong_owner:
    case CommandStatus::conflict:
      break;
    }
  }

  Session session_;
  SimRuntime runtime_;
  std::array<PaneId, 1'024> stale_panes_{};
  std::size_t stale_next_{0};
  std::size_t stale_count_{0};
  std::size_t generated_operations_{0};
  std::optional<MuxOperationKind> previous_operation_;
  WorldCoverage coverage_;
  core::SessionTransition last_transition_;
  std::string_view last_operation_{"bootstrap"};
};

enum class MuxFailurePhase : std::uint8_t {
  setup,
  operation,
  checkpoint,
  healing,
};

struct MuxFailure final {
  MuxFailurePhase phase{MuxFailurePhase::setup};
  std::size_t operation{0};
  std::string reason;
};

struct MuxRunResult final {
  std::vector<MuxTraceEntry> entries;
  std::optional<MuxFailure> failure;
  WorldCoverage world_coverage;
  SimRuntimeCoverage runtime_coverage;
  std::uint64_t final_hash{0};
};

class LiveTraceWriter final {
public:
  [[nodiscard]] auto open(const std::optional<std::filesystem::path>& path, std::string& error)
      -> bool {
    if (!path.has_value()) {
      return true;
    }
    enabled_ = true;
    path_ = *path;
    if (!write_mux_trace_file(path_, {}, error)) {
      return false;
    }
    stream_.open(path_, std::ios::app);
    if (!stream_.is_open()) {
      error = "could not append to " + path_.string();
      return false;
    }
    return true;
  }

  [[nodiscard]] auto append(const MuxOperation& operation, std::string& error) -> bool {
    if (!enabled_) {
      return true;
    }
    write_mux_operation(stream_, operation);
    return flush(error);
  }

  [[nodiscard]] auto append(const MuxCheckpoint& checkpoint, std::string& error) -> bool {
    if (!enabled_) {
      return true;
    }
    write_mux_checkpoint(stream_, checkpoint);
    return flush(error);
  }

private:
  [[nodiscard]] auto flush(std::string& error) -> bool {
    stream_.flush();
    if (!stream_.good()) {
      error = "could not update live mux trace " + path_.string();
      return false;
    }
    return true;
  }

  std::filesystem::path path_;
  std::ofstream stream_;
  bool enabled_{false};
};

[[nodiscard]] auto checkpoint(const MuxWorld& world) noexcept -> MuxCheckpoint {
  return {.status = world.last_transition().result.status,
          .mutated = world.last_transition().mutated,
          .state_hash = world.state_hash()};
}

void finish_run(MuxWorld& world, MuxRunResult& result) {
  if (!result.failure.has_value()) {
    if (const auto error = world.heal(); error.has_value()) {
      result.failure = MuxFailure{
          .phase = MuxFailurePhase::healing, .operation = result.entries.size(), .reason = *error};
    }
  }
  result.final_hash = world.state_hash();
  result.world_coverage = world.coverage();
  result.runtime_coverage = world.runtime_coverage();
}

[[nodiscard]] auto checkpoints_match(const MuxCheckpoint& expected, const MuxCheckpoint& actual,
                                     std::string& error) -> bool {
  if (expected.status != actual.status) {
    error = "recorded command status changed";
    return false;
  }
  if (expected.mutated != actual.mutated) {
    error = "recorded mutation decision changed";
    return false;
  }
  if (expected.state_hash != actual.state_hash) {
    error = "recorded Core/Runtime state hash changed";
    return false;
  }
  return true;
}

[[nodiscard]] auto
execute_entries(const std::span<const MuxTraceEntry> entries,
                const std::optional<std::filesystem::path>& live_trace = std::nullopt,
                const bool validate_checkpoints = true) -> MuxRunResult {
  MuxRunResult result;
  result.entries.reserve(entries.size());
  auto world = std::make_unique<MuxWorld>();
  LiveTraceWriter writer;
  std::string writer_error;
  if (!writer.open(live_trace, writer_error)) {
    result.failure = MuxFailure{.phase = MuxFailurePhase::setup, .reason = writer_error};
    finish_run(*world, result);
    return result;
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& requested = entries.subspan(index, 1).front();
    result.entries.push_back({.operation = requested.operation});
    if (!writer.append(requested.operation, writer_error)) {
      result.failure =
          MuxFailure{.phase = MuxFailurePhase::setup, .operation = index, .reason = writer_error};
      break;
    }
    const auto operation_error = world->apply(requested.operation);
    const auto actual = checkpoint(*world);
    result.entries.back().checkpoint = actual;
    if (!writer.append(actual, writer_error)) {
      result.failure =
          MuxFailure{.phase = MuxFailurePhase::setup, .operation = index, .reason = writer_error};
      break;
    }
    if (validate_checkpoints && requested.checkpoint.has_value()) {
      std::string mismatch;
      if (!checkpoints_match(*requested.checkpoint, actual, mismatch)) {
        result.failure = MuxFailure{.phase = MuxFailurePhase::checkpoint,
                                    .operation = index,
                                    .reason = std::move(mismatch)};
        break;
      }
    }
    if (operation_error.has_value()) {
      result.failure = MuxFailure{
          .phase = MuxFailurePhase::operation, .operation = index, .reason = *operation_error};
      break;
    }
  }
  finish_run(*world, result);
  return result;
}

[[nodiscard]] auto
execute_generated(const std::uint64_t seed, const std::size_t operation_count,
                  const std::optional<std::filesystem::path>& live_trace = std::nullopt)
    -> MuxRunResult {
  MuxRunResult result;
  result.entries.reserve(operation_count);
  auto world = std::make_unique<MuxWorld>();
  Random operations(seed);
  Random faults(seed ^ 0xF417'5EED'D15C'A11EULL);
  LiveTraceWriter writer;
  std::string writer_error;
  if (!writer.open(live_trace, writer_error)) {
    result.failure = MuxFailure{.phase = MuxFailurePhase::setup, .reason = writer_error};
    finish_run(*world, result);
    return result;
  }
  for (std::size_t index = 0; index < operation_count; ++index) {
    const auto operation = world->generate(operations, faults);
    result.entries.push_back({.operation = operation});
    if (!writer.append(operation, writer_error)) {
      result.failure =
          MuxFailure{.phase = MuxFailurePhase::setup, .operation = index, .reason = writer_error};
      break;
    }
    const auto operation_error = world->apply(operation);
    const auto actual = checkpoint(*world);
    result.entries.back().checkpoint = actual;
    if (!writer.append(actual, writer_error)) {
      result.failure =
          MuxFailure{.phase = MuxFailurePhase::setup, .operation = index, .reason = writer_error};
      break;
    }
    if (operation_error.has_value()) {
      result.failure = MuxFailure{
          .phase = MuxFailurePhase::operation, .operation = index, .reason = *operation_error};
      break;
    }
  }
  finish_run(*world, result);
  return result;
}

[[nodiscard]] auto same_failure(const MuxFailure& expected,
                                const std::optional<MuxFailure>& actual) noexcept -> bool {
  return actual.has_value() && actual->phase == expected.phase && actual->reason == expected.reason;
}

[[nodiscard]] auto operations_from_entries(const std::span<const MuxTraceEntry> entries)
    -> std::vector<MuxOperation> {
  std::vector<MuxOperation> operations;
  operations.reserve(entries.size());
  for (const auto& entry : entries) {
    operations.push_back(entry.operation);
  }
  return operations;
}

[[nodiscard]] auto entries_without_checkpoints(const std::span<const MuxOperation> operations)
    -> std::vector<MuxTraceEntry> {
  std::vector<MuxTraceEntry> entries;
  entries.reserve(operations.size());
  for (const auto& operation : operations) {
    entries.push_back({.operation = operation});
  }
  return entries;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto shrink_mux_operation(const MuxOperation& operation, const std::size_t attempt)
    -> std::optional<MuxOperation> {
  auto candidate = operation;
  switch (attempt) {
  case 0:
    if (candidate.argument_0 != 0) {
      candidate.argument_0 = 0;
      return candidate;
    }
    break;
  case 1:
    if (candidate.argument_1 != 0) {
      candidate.argument_1 = 0;
      return candidate;
    }
    break;
  case 2:
    if (candidate.spawn_outcome != RuntimeEffectStatus::applied) {
      candidate.spawn_outcome = RuntimeEffectStatus::applied;
      return candidate;
    }
    break;
  case 3:
    if (candidate.resize_outcome != RuntimeEffectStatus::applied) {
      candidate.resize_outcome = RuntimeEffectStatus::applied;
      return candidate;
    }
    break;
  case 4:
    if (candidate.peer_pane.is_valid()) {
      candidate.peer_pane = {};
      return candidate;
    }
    break;
  case 5:
    if (candidate.peer_tab.is_valid()) {
      candidate.peer_tab = {};
      return candidate;
    }
    break;
  case 6:
    if (candidate.pane.is_valid()) {
      candidate.pane = {};
      return candidate;
    }
    break;
  case 7:
    if (candidate.tab.is_valid()) {
      candidate.tab = {};
      return candidate;
    }
    break;
  default:
    break;
  }
  return std::nullopt;
}

[[nodiscard]] auto reduction_evaluations(std::string& error) -> std::size_t {
  std::uint64_t evaluations = mux_reduction_evaluations_default;
  if (!environment_u64("LEMMA_MUX_SIM_REDUCTION_EVALUATIONS", evaluations) ||
      evaluations > trace_reduction_evaluations_max) {
    error = "LEMMA_MUX_SIM_REDUCTION_EVALUATIONS must be an integer no greater than " +
            std::to_string(trace_reduction_evaluations_max);
    return 0;
  }
  return static_cast<std::size_t>(evaluations);
}

[[nodiscard]] auto minimized_path(const std::filesystem::path& original) -> std::filesystem::path {
  auto path = original;
  path.replace_extension(".min.trace");
  return path;
}

struct FailureArtifacts final {
  std::filesystem::path original;
  std::filesystem::path minimized;
  TraceReductionStats reduction;
  std::string error;
};

[[nodiscard]] auto persist_failure(const MuxRunResult& failed, const std::filesystem::path& path,
                                   const bool reduce = true) -> FailureArtifacts {
  FailureArtifacts artifacts{.original = path, .minimized = minimized_path(path)};
  if (!write_mux_trace_file(artifacts.original, failed.entries, artifacts.error)) {
    return artifacts;
  }
  if (!reduce || !failed.failure.has_value() ||
      (failed.failure->phase != MuxFailurePhase::operation &&
       failed.failure->phase != MuxFailurePhase::healing)) {
    return artifacts;
  }
  const auto evaluations = reduction_evaluations(artifacts.error);
  if (!artifacts.error.empty() || evaluations == 0) {
    return artifacts;
  }
  const auto original = operations_from_entries(failed.entries);
  const auto reproduces = [&failure =
                               *failed.failure](const std::span<const MuxOperation> candidate) {
    const auto entries = entries_without_checkpoints(candidate);
    return same_failure(failure, execute_entries(entries).failure);
  };
  const auto reduced =
      reduce_trace<MuxOperation>(original, reproduces, &shrink_mux_operation, evaluations);
  artifacts.reduction = reduced.stats;
  const auto minimized_entries = entries_without_checkpoints(reduced.operations);
  const auto minimized_run = execute_entries(minimized_entries);
  if (!same_failure(*failed.failure, minimized_run.failure)) {
    artifacts.error = "minimized mux trace did not reproduce the original failure";
    return artifacts;
  }
  if (!write_mux_trace_file(artifacts.minimized, minimized_run.entries, artifacts.error)) {
    return artifacts;
  }
  return artifacts;
}

[[nodiscard]] auto phase_name(const MuxFailurePhase phase) noexcept -> std::string_view {
  switch (phase) {
  case MuxFailurePhase::setup:
    return "setup";
  case MuxFailurePhase::operation:
    return "operation";
  case MuxFailurePhase::checkpoint:
    return "checkpoint";
  case MuxFailurePhase::healing:
    return "healing";
  }
  return "unknown";
}

[[nodiscard]] auto default_failure_path(const std::uint64_t seed) -> std::filesystem::path {
  std::ostringstream name;
  name << "mux-0x" << std::hex << seed << ".trace";
  return std::filesystem::path{"build/mux-sim-failures"} / name.str();
}

[[nodiscard]] auto assertion_for_run(const MuxRunResult& result,
                                     const std::filesystem::path& failure_path,
                                     const std::string_view replay) -> testing::AssertionResult {
  if (!result.failure.has_value()) {
    return testing::AssertionSuccess();
  }
  const auto artifacts = persist_failure(result, failure_path);
  const auto& failure = *result.failure;
  auto assertion = testing::AssertionFailure();
  assertion << failure.reason << " during " << phase_name(failure.phase);
  if (failure.phase != MuxFailurePhase::setup) {
    assertion << " at operation " << failure.operation;
  }
  assertion << '\n' << "trace: " << artifacts.original.string() << '\n';
  if (artifacts.reduction.original_operations != 0) {
    assertion << "minimized: " << artifacts.minimized.string() << " ("
              << artifacts.reduction.original_operations << " -> "
              << artifacts.reduction.reduced_operations << " operations, "
              << artifacts.reduction.evaluations << " evaluations)\n";
  }
  if (!artifacts.error.empty()) {
    assertion << "artifact error: " << artifacts.error << '\n';
  }
  assertion << "replay: " << replay << '\n';
  return assertion;
}

[[nodiscard]] auto configured_trace_output() -> std::optional<std::filesystem::path> {
  const auto* const path = std::getenv("LEMMA_MUX_SIM_TRACE_OUT");
  return path == nullptr || *path == '\0' ? std::nullopt
                                          : std::optional<std::filesystem::path>{path};
}

// JSON makes every search dimension inspectable without adding a second reporting model.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto write_coverage_report(const std::uint64_t seed,
                                         const std::size_t operation_count,
                                         const WorldCoverage& world,
                                         const SimRuntimeCoverage& runtime, std::string& error)
    -> bool {
  const auto* const configured = std::getenv("LEMMA_MUX_SIM_COVERAGE_OUT");
  if (configured == nullptr || *configured == '\0') {
    return true;
  }
  const std::filesystem::path path{configured};
  std::error_code directory_error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), directory_error);
  }
  if (directory_error) {
    error = "could not create mux coverage directory: " + directory_error.message();
    return false;
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output.is_open()) {
    error = "could not create mux coverage report " + path.string();
    return false;
  }
  output << "{\n  \"schema\": 3,\n  \"seed\": \"0x" << std::hex << seed << std::dec
         << "\",\n  \"operations_requested\": " << operation_count << ",\n  \"state_mask\": \"0x"
         << std::hex << world.states << std::dec << "\",\n  \"operation_counts\": {\n";
  for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
    output << "    \"" << mux_operation_name(static_cast<MuxOperationKind>(operation))
           << "\": " << world.operations.at(operation)
           << (operation + 1U == mux_operation_kind_count ? "\n" : ",\n");
  }
  output << "  },\n  \"outcome_counts\": {\n";
  for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
    output << "    \"" << mux_operation_name(static_cast<MuxOperationKind>(operation)) << "\": {";
    for (std::size_t outcome = 0; outcome < mux_outcome_class_count; ++outcome) {
      output << "\"" << mux_outcome_names.at(outcome)
             << "\": " << world.outcomes.at(operation).at(outcome)
             << (outcome + 1U == mux_outcome_class_count ? "" : ", ");
    }
    output << "}" << (operation + 1U == mux_operation_kind_count ? "\n" : ",\n");
  }
  output << "  },\n  \"fault_counts\": {\n";
  for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
    output << "    \"" << mux_operation_name(static_cast<MuxOperationKind>(operation)) << "\": {";
    for (std::size_t fault = 0; fault < mux_fault_class_count; ++fault) {
      output << "\"" << mux_fault_names.at(fault) << "\": " << world.faults.at(operation).at(fault)
             << (fault + 1U == mux_fault_class_count ? "" : ", ");
    }
    output << "}" << (operation + 1U == mux_operation_kind_count ? "\n" : ",\n");
  }
  output << "  },\n  \"operation_state_counts\": {\n";
  for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
    output << "    \"" << mux_operation_name(static_cast<MuxOperationKind>(operation)) << "\": {";
    for (std::size_t state = 0; state < mux_state_class_count; ++state) {
      output << "\"" << mux_state_names.at(state)
             << "\": " << world.operation_states.at(operation).at(state)
             << (state + 1U == mux_state_class_count ? "" : ", ");
    }
    output << "}" << (operation + 1U == mux_operation_kind_count ? "\n" : ",\n");
  }
  output << "  },\n  \"joint_signatures\": [\n";
  bool first_signature = true;
  for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
    for (std::size_t outcome = 0; outcome < mux_outcome_class_count; ++outcome) {
      for (std::size_t fault = 0; fault < mux_fault_class_count; ++fault) {
        for (std::size_t state = 0; state < mux_state_class_count; ++state) {
          const auto count = world.signatures.at(operation).at(outcome).at(fault).at(state);
          if (count == 0U) {
            continue;
          }
          output << (first_signature ? "" : ",\n") << R"(    {"operation": ")"
                 << mux_operation_name(static_cast<MuxOperationKind>(operation))
                 << R"(", "outcome": ")" << mux_outcome_names.at(outcome) << R"(", "fault": ")"
                 << mux_fault_names.at(fault) << R"(", "state": ")" << mux_state_names.at(state)
                 << R"(", "count": )" << count << "}";
          first_signature = false;
        }
      }
    }
  }
  output << "\n  ],\n  \"transition_counts\": {\n";
  for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
    output << "    \"" << mux_operation_name(static_cast<MuxOperationKind>(operation)) << "\": {";
    for (std::size_t next = 0; next < mux_operation_kind_count; ++next) {
      output << "\"" << mux_operation_name(static_cast<MuxOperationKind>(next))
             << "\": " << world.transitions.at(operation).at(next)
             << (next + 1U == mux_operation_kind_count ? "" : ", ");
    }
    output << "}" << (operation + 1U == mux_operation_kind_count ? "\n" : ",\n");
  }
  output << "  },\n  \"runtime\": {\n"
         << "    \"spawns\": " << runtime.spawns << ",\n"
         << "    \"spawn_rejections\": " << runtime.spawn_rejections << ",\n"
         << "    \"resize_batches\": " << runtime.resize_batches << ",\n"
         << "    \"resize_rejections\": " << runtime.resize_rejections << ",\n"
         << "    \"consistency_losses\": " << runtime.consistency_losses << ",\n"
         << "    \"retired\": " << runtime.retired << ",\n"
         << "    \"held\": " << runtime.held << "\n  }\n}\n";
  output.flush();
  if (!output.good()) {
    error = "could not write mux coverage report " + path.string();
    return false;
  }
  return true;
}

[[nodiscard]] auto
run_mux_world(const std::uint64_t seed, const std::size_t operation_count,
              std::uint64_t* const final_hash = nullptr,
              WorldCoverage* const world_coverage = nullptr,
              SimRuntimeCoverage* const runtime_coverage = nullptr,
              const std::optional<std::filesystem::path>& trace_output = std::nullopt)
    -> testing::AssertionResult {
  const auto result = execute_generated(seed, operation_count, trace_output);
  if (final_hash != nullptr) {
    *final_hash = result.final_hash;
  }
  if (world_coverage != nullptr) {
    *world_coverage = result.world_coverage;
  }
  if (runtime_coverage != nullptr) {
    *runtime_coverage = result.runtime_coverage;
  }
  std::ostringstream replay;
  replay << "LEMMA_MUX_SIM_SEED=0x" << std::hex << seed << std::dec
         << " LEMMA_MUX_SIM_OPERATIONS=" << operation_count << " ./test sim";
  return assertion_for_run(result, trace_output.value_or(default_failure_path(seed)), replay.str());
}

// Characterization records coverage, not a discovered bug. Only regressions name a fixing revision.
[[nodiscard]] auto trace_has_corpus_metadata(std::istream& input) -> bool {
  constexpr std::array<std::string_view, 6> prefixes{
      "# corpus-kind: ",   "# description: ", "# source-trace: ",
      "# introduced-at: ", "# fixed-at: ",    "# regression: "};
  std::array<std::string, prefixes.size()> fields;
  std::string line;
  while (std::getline(input, line)) {
    for (std::size_t index = 0; index < prefixes.size(); ++index) {
      if (!line.starts_with(prefixes.at(index))) {
        continue;
      }
      auto value = line.substr(prefixes.at(index).size());
      if (!fields.at(index).empty() || value.find_first_not_of(" \t\r") == std::string::npos) {
        return false;
      }
      fields.at(index) = std::move(value);
    }
  }
  const bool common = !fields.at(1).empty() && !fields.at(2).empty() && fields.at(5).empty();
  const bool characterization =
      fields.at(0) == "characterization" && !fields.at(3).empty() && fields.at(4).empty();
  const bool regression =
      fields.at(0) == "regression" && fields.at(3).empty() && !fields.at(4).empty();
  return common && (characterization || regression);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(MuxTraceTest, CorpusProvenanceSeparatesCharacterizationFromDiscoveredRegressions) {
  constexpr std::string_view common = "# description: lifecycle\n# source-trace: seed 0\n";
  const auto valid = [common](const std::string_view metadata) {
    std::istringstream input(std::string(common) + std::string(metadata));
    return trace_has_corpus_metadata(input);
  };
  EXPECT_TRUE(valid("# corpus-kind: characterization\n# introduced-at: abc123\n"));
  EXPECT_TRUE(valid("# corpus-kind: regression\n# fixed-at: abc123\n"));
  EXPECT_FALSE(valid("# corpus-kind: characterization\n# fixed-at: abc123\n"));
  EXPECT_FALSE(valid("# corpus-kind: regression\n# introduced-at: abc123\n"));
  EXPECT_FALSE(valid("# corpus-kind: regression\n# fixed-at: abc123\n# introduced-at: def456\n"));
  EXPECT_FALSE(valid("# corpus-kind: unknown\n# fixed-at: abc123\n"));
  EXPECT_FALSE(valid("# corpus-kind: regression\n# fixed-at: \t\n"));
  EXPECT_FALSE(valid("# corpus-kind: regression\n# fixed-at: abc123\n# fixed-at: def456\n"));
  EXPECT_FALSE(
      valid("# corpus-kind: regression\n# fixed-at: abc123\n# regression: old metadata\n"));
  EXPECT_FALSE(valid("# fixed-at: abc123\n"));
  std::istringstream missing_description("# corpus-kind: regression\n# fixed-at: abc123\n");
  EXPECT_FALSE(trace_has_corpus_metadata(missing_description));
}

[[nodiscard]] auto
run_mux_trace(const std::filesystem::path& path, const std::span<const MuxTraceEntry> entries,
              const std::optional<std::filesystem::path>& trace_output = std::nullopt,
              const bool validate_checkpoints = true) -> testing::AssertionResult {
  const auto result = execute_entries(entries, trace_output, validate_checkpoints);
  const auto failure_path =
      trace_output.value_or(std::filesystem::path{"build/mux-sim-failures"} / path.filename());
  return assertion_for_run(result, failure_path,
                           "LEMMA_MUX_SIM_TRACE=" + path.string() + " ./test sim");
}

TEST(MuxTraceTest, ConcreteTraceRoundTripsWithoutGeneratorState) {
  const std::array entries{
      MuxTraceEntry{.operation = {.kind = MuxOperationKind::split,
                                  .tab = TabId::from_parts(1, 2),
                                  .pane = PaneId::from_parts(3, 4),
                                  .argument_0 = 1,
                                  .argument_1 = 1,
                                  .spawn_outcome = RuntimeEffectStatus::rejected,
                                  .resize_outcome = RuntimeEffectStatus::consistency_lost},
                    .checkpoint = MuxCheckpoint{.status = CommandStatus::unavailable,
                                                .mutated = false,
                                                .state_hash = 0xABCDEFULL}},
  };
  std::ostringstream encoded;
  write_mux_trace_header(encoded);
  write_mux_operation(encoded, entries.front().operation);
  write_mux_checkpoint(encoded, entries.front().checkpoint.value_or(MuxCheckpoint{}));
  std::istringstream input(encoded.str());
  std::vector<MuxTraceEntry> decoded;
  std::string error;
  ASSERT_TRUE(read_mux_trace(input, decoded, error)) << error;
  EXPECT_EQ(decoded, std::vector(entries.begin(), entries.end()));
}

TEST(MuxTraceTest, LogicalFailureProducesReplayableMinimizedArtifact) {
  const std::array operations{
      MuxTraceEntry{.operation = {.kind = MuxOperationKind::idle}},
      MuxTraceEntry{.operation = {.kind = MuxOperationKind::focus,
                                  .tab = TabId::from_parts(0, 1),
                                  .pane = PaneId::from_parts(0, 1)}},
      MuxTraceEntry{.operation = {.kind = MuxOperationKind::spawn_failure,
                                  .tab = TabId::from_parts(0, 1),
                                  .pane = PaneId::from_parts(0, 1),
                                  .spawn_outcome = RuntimeEffectStatus::applied}},
  };
  const auto failed = execute_entries(operations);
  ASSERT_TRUE(failed.failure.has_value());
  const auto expected_failure = failed.failure.value_or(MuxFailure{});
  ASSERT_EQ(expected_failure.phase, MuxFailurePhase::operation);

  const std::filesystem::path artifact{"build/mux-sim-failures/reducer-self-test.trace"};
  const auto minimized = minimized_path(artifact);
  std::error_code remove_error;
  std::filesystem::remove(artifact, remove_error);
  std::filesystem::remove(minimized, remove_error);
  const auto artifacts = persist_failure(failed, artifact);
  ASSERT_TRUE(artifacts.error.empty()) << artifacts.error;
  EXPECT_LT(artifacts.reduction.reduced_operations, artifacts.reduction.original_operations);

  std::vector<MuxTraceEntry> replay;
  std::string error;
  ASSERT_TRUE(read_mux_trace_file(minimized, replay, error)) << error;
  EXPECT_TRUE(same_failure(expected_failure, execute_entries(replay).failure));
  std::filesystem::remove(artifact, remove_error);
  std::filesystem::remove(minimized, remove_error);
}

TEST(MuxSimulationTest, SameSeedAndFaultScheduleReachTheSameState) {
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  ASSERT_TRUE(run_mux_world(0x4D555853494DULL, 512, &first));
  ASSERT_TRUE(run_mux_world(0x4D555853494DULL, 512, &second));
  EXPECT_EQ(first, second);
}

TEST(MuxSimulationTest, ConsistencyLossFailsClosedWithoutBreakingSemanticOrRuntimeInvariants) {
  MuxWorld world;
  const auto transition = world.force_consistency_loss();
  EXPECT_EQ(transition.result.status, CommandStatus::failed);
  EXPECT_TRUE(transition.mutated);
  EXPECT_FALSE(world.check_all_invariants().has_value());
}

TEST(MuxSimulationTest, RejectedResizeSuspendsLayoutAfterClosingZoomedPane) {
  const std::array operations{
      MuxTraceEntry{.operation = {.kind = MuxOperationKind::split,
                                  .tab = TabId::from_parts(0, 1),
                                  .pane = PaneId::from_parts(0, 1)}},
      MuxTraceEntry{.operation = {.kind = MuxOperationKind::zoom,
                                  .tab = TabId::from_parts(0, 1),
                                  .pane = PaneId::from_parts(1, 1)}},
      MuxTraceEntry{.operation = {.kind = MuxOperationKind::attachment_resize,
                                  .argument_0 = 3,
                                  .argument_1 = 1}},
      MuxTraceEntry{.operation = {.kind = MuxOperationKind::child_exit,
                                  .tab = TabId::from_parts(0, 1),
                                  .pane = PaneId::from_parts(1, 1),
                                  .resize_outcome = RuntimeEffectStatus::rejected}},
  };
  EXPECT_TRUE(run_mux_trace("rejected-zoomed-child-exit.trace", operations));
}

// Directional swap targets are resolved by the input bridge from the same Core rule that
// directional focus executes, so both must agree for every source Pane, zoomed or not.
// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(MuxSimulationTest, DirectionalFocusFollowsTheSharedTiledNeighborRule) {
  using core::PaneDirection;
  SimRuntime runtime;
  Session session("directional", {}, {}, core::LaunchEnvironmentMode::inherit);
  session.id = SessionId::from_parts(0, 1);
  session.attachment.id = AttachmentId::from_parts(0, 1);
  session.attachment.session = session.id;
  session.attachment.columns = 120;
  session.attachment.rows = 40;
  SessionMachine machine(session, {.runtime = runtime.effects()});
  const auto created = machine.create_tab();
  ASSERT_EQ(created.result.status, CommandStatus::applied);
  const auto tab_id = created.created_tab;
  const auto left = created.created_pane;
  const auto split_right = machine.split_pane(tab_id, left, SplitAxis::left_right);
  ASSERT_EQ(split_right.result.status, CommandStatus::applied);
  const auto top_right = split_right.created_pane;
  const auto split_down = machine.split_pane(tab_id, top_right, SplitAxis::top_bottom);
  ASSERT_EQ(split_down.result.status, CommandStatus::applied);
  const auto bottom_right = split_down.created_pane;
  const auto& tab_slot = std::span(session.tabs).subspan(tab_id.slot(), 1).front();
  const auto* const tab_owner = tab_slot.tab.get();
  ASSERT_NE(tab_owner, nullptr);
  const auto& tab = *tab_owner;
  const auto dispatch = [&](const CommandKind kind, const PaneId pane,
                            const CommandPayload payload = {}) {
    return machine.dispatch({.kind = kind,
                             .origin = CommandOrigin::internal,
                             .target = {.session = session.id, .tab = tab_id, .pane = pane},
                             .payload = payload});
  };

  // left | top_right
  //      | bottom_right
  const auto neighbor = [&](const PaneId source, const PaneDirection direction) {
    return core::pane_in_direction(session, tab, source, direction);
  };
  EXPECT_EQ(neighbor(left, PaneDirection::left), std::nullopt);
  EXPECT_EQ(neighbor(left, PaneDirection::up), std::nullopt);
  EXPECT_EQ(neighbor(left, PaneDirection::right), std::optional{top_right});
  EXPECT_EQ(neighbor(top_right, PaneDirection::left), std::optional{left});
  EXPECT_EQ(neighbor(top_right, PaneDirection::down), std::optional{bottom_right});
  EXPECT_EQ(neighbor(bottom_right, PaneDirection::up), std::optional{top_right});
  EXPECT_EQ(neighbor(bottom_right, PaneDirection::left), std::optional{left});
  EXPECT_EQ(neighbor(bottom_right, PaneDirection::down), std::nullopt);

  constexpr std::array directions{
      std::pair{PaneDirection::left, CommandKind::focus_left},
      std::pair{PaneDirection::right, CommandKind::focus_right},
      std::pair{PaneDirection::up, CommandKind::focus_up},
      std::pair{PaneDirection::down, CommandKind::focus_down},
  };
  const std::array panes{left, top_right, bottom_right};
  std::array<std::array<std::optional<PaneId>, directions.size()>, panes.size()> tiled{};
  for (std::size_t source = 0; source < panes.size(); ++source) {
    for (std::size_t direction = 0; direction < directions.size(); ++direction) {
      tiled.at(source).at(direction) = neighbor(panes.at(source), directions.at(direction).first);
    }
  }
  for (const bool zoomed : {false, true}) {
    for (std::size_t source_index = 0; source_index < panes.size(); ++source_index) {
      for (std::size_t direction_index = 0; direction_index < directions.size();
           ++direction_index) {
        const auto source = panes.at(source_index);
        const auto [direction, kind] = directions.at(direction_index);
        SCOPED_TRACE(testing::Message() << "zoomed=" << zoomed << " source=" << source.slot()
                                        << " direction=" << static_cast<int>(direction));
        if (tab.focused_pane != source) {
          ASSERT_EQ(dispatch(CommandKind::focus_pane, source).result.status,
                    CommandStatus::applied);
        }
        if (tab.zoomed != zoomed) {
          ASSERT_EQ(dispatch(CommandKind::set_zoom, source, PaneZoomCommand{.enabled = zoomed})
                        .result.status,
                    CommandStatus::applied);
        }
        const auto expected = neighbor(source, direction);
        EXPECT_EQ(expected, tiled.at(source_index).at(direction_index));
        const auto focused = dispatch(kind, source);
        if (expected.has_value()) {
          EXPECT_EQ(focused.result.status, CommandStatus::applied);
          EXPECT_EQ(tab.focused_pane, *expected);
          EXPECT_EQ(tab.previous_pane, source);
        } else {
          EXPECT_EQ(focused.result.status, CommandStatus::no_effect);
          EXPECT_EQ(tab.focused_pane, source);
        }
        EXPECT_EQ(tab.zoomed, zoomed);
        EXPECT_FALSE(core::check_session_invariants(session).has_value());
      }
    }
  }
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(MuxSimulationTest, ReplaysCheckedInCorpus) {
  const std::filesystem::path corpus{"tests/sim/corpus/mux"};
  std::error_code directory_error;
  std::vector<std::filesystem::path> traces;
  for (std::filesystem::directory_iterator iterator(corpus, directory_error), end;
       !directory_error && iterator != end; ++iterator) {
    if (iterator->is_regular_file() && iterator->path().extension() == ".trace") {
      traces.push_back(iterator->path());
    }
  }
  ASSERT_FALSE(directory_error) << directory_error.message();
  std::ranges::sort(traces);
  ASSERT_FALSE(traces.empty()) << "mux corpus must contain at least one trace";
  for (const auto& path : traces) {
    SCOPED_TRACE(path.string());
    std::ifstream metadata(path);
    ASSERT_TRUE(trace_has_corpus_metadata(metadata))
        << "traces require a corpus kind, description, source, and kind-specific revision";
    std::vector<MuxTraceEntry> entries;
    std::string error;
    ASSERT_TRUE(read_mux_trace_file(path, entries, error)) << error;
    ASSERT_FALSE(entries.empty());
    EXPECT_TRUE(run_mux_trace(path, entries));
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(MuxSimulationTest, GeneratedCommandsAndRuntimeFaultsPreserveAllInvariants) {
  constexpr std::array<std::uint64_t, 6> seeds{0ULL,
                                               1ULL,
                                               0xC0FFEEULL,
                                               0x51A7E123ULL,
                                               0xDEADBEEFCAFEBABEULL,
                                               std::numeric_limits<std::uint64_t>::max()};
  if (const auto* const trace_path = std::getenv("LEMMA_MUX_SIM_TRACE"); trace_path != nullptr) {
    std::vector<MuxTraceEntry> entries;
    std::string error;
    ASSERT_TRUE(read_mux_trace_file(trace_path, entries, error)) << error;
    ASSERT_FALSE(entries.empty());
    const bool refresh = std::getenv("LEMMA_MUX_SIM_REFRESH_TRACE") != nullptr;
    const auto trace_output = configured_trace_output();
    ASSERT_FALSE(refresh && !trace_output.has_value())
        << "LEMMA_MUX_SIM_REFRESH_TRACE requires LEMMA_MUX_SIM_TRACE_OUT";
    ASSERT_TRUE(run_mux_trace(trace_path, entries, trace_output, !refresh));
    return;
  }

  std::uint64_t selected_seed = 0;
  std::uint64_t selected_operations = 1'024;
  const bool configured = std::getenv("LEMMA_MUX_SIM_SEED") != nullptr;
  ASSERT_TRUE(environment_u64("LEMMA_MUX_SIM_SEED", selected_seed));
  ASSERT_TRUE(environment_u64("LEMMA_MUX_SIM_OPERATIONS", selected_operations));
  ASSERT_GT(selected_operations, 0U);
  ASSERT_LE(selected_operations, mux_trace_operations_max);
  if (configured) {
    WorldCoverage configured_world;
    SimRuntimeCoverage configured_runtime;
    const auto generated =
        run_mux_world(selected_seed, selected_operations, nullptr, &configured_world,
                      &configured_runtime, configured_trace_output());
    std::string coverage_error;
    const bool coverage_written = write_coverage_report(
        selected_seed, selected_operations, configured_world, configured_runtime, coverage_error);
    ASSERT_TRUE(generated);
    ASSERT_TRUE(coverage_written) << coverage_error;
    return;
  }

  WorldCoverage world_coverage;
  SimRuntimeCoverage runtime_coverage;
  for (const auto seed : seeds) {
    WorldCoverage current_world;
    SimRuntimeCoverage current_runtime;
    ASSERT_TRUE(
        run_mux_world(seed, selected_operations, nullptr, &current_world, &current_runtime));
    world_coverage.merge(current_world);
    runtime_coverage.spawns += current_runtime.spawns;
    runtime_coverage.spawn_rejections += current_runtime.spawn_rejections;
    runtime_coverage.resize_batches += current_runtime.resize_batches;
    runtime_coverage.resize_rejections += current_runtime.resize_rejections;
    runtime_coverage.consistency_losses += current_runtime.consistency_losses;
    runtime_coverage.retired += current_runtime.retired;
    runtime_coverage.held += current_runtime.held;
  }
  EXPECT_GT(world_coverage.applied, 0U);
  EXPECT_GT(world_coverage.no_effect, 0U);
  EXPECT_GT(world_coverage.rejected, 0U);
  EXPECT_GT(world_coverage.stale, 0U);
  EXPECT_GT(world_coverage.child_exits, 0U);
  EXPECT_GT(world_coverage.runtime_errors, 0U);
  EXPECT_GT(runtime_coverage.spawns, 0U);
  EXPECT_GT(runtime_coverage.spawn_rejections, 0U);
  EXPECT_GT(runtime_coverage.resize_batches, 0U);
  EXPECT_GT(runtime_coverage.resize_rejections, 0U);
  EXPECT_GT(runtime_coverage.retired, 0U);
  EXPECT_GT(runtime_coverage.held, 0U);
  for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
    EXPECT_GT(world_coverage.operations.at(operation), 0U)
        << "uncovered operation " << mux_operation_name(static_cast<MuxOperationKind>(operation));
  }
  const auto operation_index = [](const MuxOperationKind operation) {
    return static_cast<std::size_t>(operation);
  };
  EXPECT_GT(world_coverage.transitions.at(operation_index(MuxOperationKind::attachment_resize))
                .at(operation_index(MuxOperationKind::create_tab)),
            0U);
  EXPECT_GT(world_coverage.transitions.at(operation_index(MuxOperationKind::create_tab))
                .at(operation_index(MuxOperationKind::split)),
            0U);
  EXPECT_GT(world_coverage.transitions.at(operation_index(MuxOperationKind::split))
                .at(operation_index(MuxOperationKind::split)),
            0U);
  EXPECT_GT(world_coverage.transitions.at(operation_index(MuxOperationKind::create_tab))
                .at(operation_index(MuxOperationKind::idle)),
            0U);
  EXPECT_GT(world_coverage.operation_states.at(operation_index(MuxOperationKind::attachment_resize))
                .at(state_maximum_attachment_index),
            0U);
  EXPECT_GT(world_coverage.operation_states.at(operation_index(MuxOperationKind::create_tab))
                .at(state_tab_capacity_index),
            0U);
  EXPECT_GT(world_coverage.operation_states.at(operation_index(MuxOperationKind::split))
                .at(state_pane_capacity_index),
            0U);
  std::array<std::size_t, mux_fault_class_count> fault_totals{};
  for (const auto& operation_faults : world_coverage.faults) {
    for (std::size_t fault = 0; fault < fault_totals.size(); ++fault) {
      fault_totals.at(fault) += operation_faults.at(fault);
    }
  }
  EXPECT_GT(fault_totals.at(0), 0U);
  EXPECT_GT(fault_totals.at(1), 0U);
  EXPECT_GT(fault_totals.at(2), 0U);
  EXPECT_GT(fault_totals.at(3), 0U);
  struct RequiredJointSignature final {
    constexpr RequiredJointSignature(const MuxOperationKind operation_value,
                                     const std::size_t outcome_value, const std::size_t fault_value,
                                     const std::size_t state_value) noexcept
        : operation(operation_value), outcome(outcome_value), fault(fault_value),
          state(state_value) {}

    MuxOperationKind operation;
    std::size_t outcome;
    std::size_t fault;
    std::size_t state;
  };
  // These signatures are the reviewed cross-dimensional search obligations, not four independent
  // coverage totals. They pin capacity rejection, generation safety, injected Runtime failures,
  // child holding, and both attachment boundaries to their exact operation outcomes.
  constexpr std::array required_signatures{
      RequiredJointSignature{MuxOperationKind::split, outcome_rejected_index, fault_none_index,
                             state_pane_capacity_index},
      RequiredJointSignature{MuxOperationKind::create_tab, outcome_rejected_index, fault_none_index,
                             state_tab_capacity_index},
      RequiredJointSignature{MuxOperationKind::stale_focus, outcome_stale_index, fault_none_index,
                             state_stale_id_retained_index},
      RequiredJointSignature{MuxOperationKind::spawn_failure, outcome_rejected_index,
                             fault_spawn_index, state_multiple_panes_index},
      RequiredJointSignature{MuxOperationKind::resize_failure, outcome_rejected_index,
                             fault_resize_index, state_multiple_panes_index},
      RequiredJointSignature{MuxOperationKind::child_exit, outcome_applied_index, fault_none_index,
                             state_held_child_index},
      RequiredJointSignature{MuxOperationKind::runtime_error, outcome_applied_index,
                             fault_none_index, state_multiple_panes_index},
      RequiredJointSignature{MuxOperationKind::attachment_resize, outcome_applied_index,
                             fault_none_index, state_minimum_attachment_index},
      RequiredJointSignature{MuxOperationKind::attachment_resize, outcome_applied_index,
                             fault_none_index, state_maximum_attachment_index},
  };
  for (const auto required : required_signatures) {
    EXPECT_GT(world_coverage.signatures.at(operation_index(required.operation))
                  .at(required.outcome)
                  .at(required.fault)
                  .at(required.state),
              0U)
        << "missing required joint signature operation=" << mux_operation_name(required.operation)
        << " outcome=" << mux_outcome_names.at(required.outcome)
        << " fault=" << mux_fault_names.at(required.fault)
        << " state=" << mux_state_names.at(required.state);
  }
  for (std::size_t operation = 0; operation < mux_operation_kind_count; ++operation) {
    std::size_t signature_count = 0;
    for (const auto& outcome : world_coverage.signatures.at(operation)) {
      for (const auto& fault : outcome) {
        for (const auto count : fault) {
          signature_count += count;
        }
      }
    }
    EXPECT_GT(signature_count, 0U) << "operation has no joint coverage signature: "
                                   << mux_operation_name(static_cast<MuxOperationKind>(operation));
  }
  constexpr auto required_states = state_single_pane | state_multiple_panes | state_pane_capacity |
                                   state_single_tab | state_multiple_tabs | state_tab_capacity |
                                   state_zoomed | state_layout_suspended | state_stale_id_retained |
                                   state_generation_reused | state_held_child |
                                   state_minimum_attachment | state_maximum_attachment;
  EXPECT_EQ(world_coverage.states & required_states, required_states)
      << "generated histories did not visit every required deep-state bucket";
}

} // namespace
#ifdef __clang__
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic pop
#endif
#endif
} // namespace lemma::test::sim
