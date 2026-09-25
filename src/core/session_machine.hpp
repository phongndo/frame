#ifndef LEMMA_CORE_SESSION_MACHINE_HPP
#define LEMMA_CORE_SESSION_MACHINE_HPP

#include "core/layout.hpp"
#include "core/session.hpp"
#include "lemma/command.hpp"
#include "lemma/geometry.hpp"
#include "lemma/id.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace lemma::core {

enum class RuntimeEffectStatus : std::uint8_t {
  applied,
  rejected,
  consistency_lost,
};

// Runtime effects are synchronous, bounded values. Core stages every fallible semantic owner before
// requesting an effect and publishes semantic state only after required effects report success.
struct SpawnPaneEffect final {
  SessionId session;
  TabId tab;
  PaneId pane;
  PaneRectangle rectangle;
  std::string_view working_directory;
  std::span<const std::byte> command;
};

struct ResizePaneEffect final {
  SessionId session;
  PaneId pane;
  PaneRectangle previous;
  PaneRectangle target;
};

using SpawnPaneEffectOperation = RuntimeEffectStatus (*)(void* context,
                                                         const SpawnPaneEffect& effect) noexcept;
using ResizePaneEffectsOperation =
    RuntimeEffectStatus (*)(void* context, std::span<const ResizePaneEffect> effects) noexcept;
using RetirePaneEffectOperation = void (*)(void* context, SessionId session, PaneId pane) noexcept;
using HoldPaneEffectOperation = void (*)(void* context, SessionId session, PaneId pane,
                                         ProcessExit process) noexcept;

struct SessionRuntimeEffects final {
  void* context{nullptr};
  SpawnPaneEffectOperation spawn{nullptr};
  ResizePaneEffectsOperation resize{nullptr};
  RetirePaneEffectOperation retire{nullptr};
  HoldPaneEffectOperation hold{nullptr};

  [[nodiscard]] constexpr auto valid() const noexcept -> bool {
    return spawn != nullptr && resize != nullptr && retire != nullptr && hold != nullptr;
  }
};

using SessionNameConflict = bool (*)(void* context, SessionId renamed,
                                     std::string_view candidate) noexcept;

struct SessionMachineOptions final {
  SessionRuntimeEffects runtime;
  SessionNameConflict name_conflict{nullptr};
  void* name_conflict_context{nullptr};
};

struct SessionChange final {
  PaneId invalidate_terminal;
  bool frame_requested{false};
  bool force_full_frame{false};
  bool status_changed{false};
  bool layout_changed{false};
};

struct SessionTransition final {
  CommandResult result;
  SessionChange change;
  TabId created_tab;
  PaneId created_pane;
  bool handled{false};
  bool mutated{false};
};

struct CreateTabOptions final {
  std::span<const std::byte> command;
  std::string_view working_directory;
  PaneExitPolicy exit_policy{PaneExitPolicy::close};
  bool activate{true};
};

struct SplitPaneOptions final {
  std::span<const std::byte> command;
  std::string_view working_directory;
  PaneExitPolicy exit_policy{PaneExitPolicy::close};
  bool focus_created{true};
};

enum class SessionInvariantError : std::uint8_t {
  invalid_session_id,
  tab_count,
  tab_slot_identity,
  tab_order_identity,
  active_tab,
  previous_tab,
  pane_count,
  pane_slot_identity,
  pane_tab,
  pane_layout_membership,
  pane_rectangle_empty,
  pane_rectangle_out_of_bounds,
  layout_invalid,
  focused_pane,
  previous_pane,
  process_exit_policy,
  attachment_identity,
  attachment_target,
  mouse_capture_target,
};

[[nodiscard]] auto session_invariant_name(SessionInvariantError error) noexcept -> std::string_view;
[[nodiscard]] auto check_session_invariants(const Session& session) noexcept
    -> std::optional<SessionInvariantError>;
[[nodiscard]] auto session_state_hash(const Session& session) noexcept -> std::uint64_t;
[[nodiscard]] auto session_lifecycle_command(CommandKind kind) noexcept -> bool;

enum class PaneDirection : std::uint8_t {
  left,
  right,
  up,
  down,
};

// The nearest Pane of `tab` in `direction` from `source`, scored over the unzoomed tiled layout in
// the Tab's viewport. Directional focus and directional swap share this rule so a modifier never
// retargets a different Pane; zoom and Runtime geometry never participate.
[[nodiscard]] auto pane_in_direction(const Session& session, const Tab& tab, PaneId source,
                                     PaneDirection direction) noexcept -> std::optional<PaneId>;

// The authoritative, deterministic Session -> Tab -> Pane state machine. The reactor and the
// simulator supply different Runtime effect implementations while sharing these exact transitions.
class SessionMachine final {
public:
  explicit constexpr SessionMachine(Session& session, SessionMachineOptions options) noexcept
      : session_(session), options_(options) {}

  [[nodiscard]] auto dispatch(const Command& command) noexcept -> SessionTransition;
  [[nodiscard]] auto create_tab(CreateTabOptions options = {}) noexcept -> SessionTransition;
  [[nodiscard]] auto split_pane(TabId tab, PaneId source, SplitAxis axis,
                                SplitPaneOptions options = {}) noexcept -> SessionTransition;
  [[nodiscard]] auto resize_attachment(std::uint16_t columns, std::uint16_t rows) noexcept
      -> SessionTransition;
  [[nodiscard]] auto resize_attachment(std::uint16_t columns, std::uint16_t rows,
                                       PaneRectangle content_viewport) noexcept
      -> SessionTransition;
  [[nodiscard]] auto runtime_failed(PaneId pane, ProcessExit process, bool child_exit) noexcept
      -> SessionTransition;

private:
  [[nodiscard]] auto finish(SessionTransition transition) noexcept -> SessionTransition;

  // SessionMachine is a synchronous borrowed facade; it never outlives this referenced owner.
  Session& session_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  SessionMachineOptions options_;
};

} // namespace lemma::core

#endif // LEMMA_CORE_SESSION_MACHINE_HPP
