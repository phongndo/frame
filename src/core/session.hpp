#ifndef LEMMA_CORE_SESSION_HPP
#define LEMMA_CORE_SESSION_HPP

#include "core/command_history.hpp"
#include "core/float_layer.hpp"
#include "core/layout.hpp"
#include "lemma/geometry.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lemma::core {

inline constexpr std::size_t panes_per_session_max = pane_layout_panes_max;
inline constexpr std::size_t tabs_per_session_max =
    static_cast<std::size_t>(limits::tabs_hard_max / limits::sessions_hard_max);
inline constexpr std::size_t panes_per_tab_max = pane_layout_panes_max;
inline constexpr std::size_t layout_nodes_per_tab_max = pane_layout_nodes_max;

static_assert(panes_per_session_max > 0);
static_assert(tabs_per_session_max > 0);

enum class LaunchEnvironmentMode : std::uint8_t {
  inherit,
  replace,
};

enum class PaneExitPolicy : std::uint8_t {
  close,
  hold,
};

enum class ProcessExitKind : std::uint8_t {
  unknown,
  exited,
  signaled,
};

struct ProcessExit final {
  ProcessExitKind kind{ProcessExitKind::unknown};
  std::uint32_t value{0};
};

struct PaneLaunchIntent final {
  std::vector<std::byte> bytes;
  std::string working_directory;
};

struct Pane final {
  [[nodiscard]] auto launch_command() const noexcept -> std::span<const std::byte> {
    return launch_intent == nullptr ? std::span<const std::byte>{}
                                    : std::span<const std::byte>(launch_intent->bytes);
  }
  [[nodiscard]] auto launch_working_directory() const noexcept -> std::string_view {
    return launch_intent == nullptr ? std::string_view{} : launch_intent->working_directory;
  }

  [[nodiscard]] auto commit_process_exit(const ProcessExit outcome) noexcept -> bool {
    if (exit_policy != PaneExitPolicy::hold || process_exit.has_value()) {
      return false;
    }
    process_exit = outcome;
    return true;
  }

  PaneId id;
  TabId tab;
  PaneRectangle rectangle{};
  // Pane-owned semantic launch intent. An empty argv means the account login shell. Keeping cold
  // argv and working-directory storage indirect avoids adding owning strings to every hot layout
  // Pane.
  std::unique_ptr<PaneLaunchIntent> launch_intent;
  // Core owns exit policy and the committed process outcome. Runtime retains the canonical
  // terminal for held panes but cannot independently decide semantic pane lifetime.
  std::optional<ProcessExit> process_exit;
  PaneExitPolicy exit_policy{PaneExitPolicy::close};
};

struct PaneSlot final {
  std::unique_ptr<Pane> pane;
  std::uint32_t generation{0};
};

// One bounded permutation owns display order. Tab storage slots and stable TabIds never encode
// presentation position, so reorder cannot invalidate identity or scheduler traversal.
class TabOrder final {
public:
  [[nodiscard]] auto append(TabId tab) noexcept -> bool;
  [[nodiscard]] auto erase(TabId tab) noexcept -> bool;
  [[nodiscard]] auto place_before(TabId moving, std::optional<TabId> anchor) noexcept -> bool;
  [[nodiscard]] auto at(std::size_t position) const noexcept -> std::optional<TabId>;
  [[nodiscard]] auto position_of(TabId tab) const noexcept -> std::optional<std::size_t>;
  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] constexpr auto empty() const noexcept -> bool { return size_ == 0; }

private:
  std::array<TabId, tabs_per_session_max> ids_{};
  std::size_t size_{0};
};

class TabTitleOverride final {
public:
  [[nodiscard]] auto view() const noexcept -> std::string_view;
  [[nodiscard]] auto set(std::string_view title) noexcept -> bool;

private:
  std::array<char, limits::tab_title_bytes_max> bytes_{};
  std::size_t size_{0};
};

// Tab keeps focus internals behind its layer-aware operations while SessionMachine owns the
// surrounding mutable layout state.
class TabFocus final {
  friend struct Tab;

  explicit constexpr TabFocus(const PaneId first_pane) noexcept : tiled_pane_(first_pane) {}

  PaneId tiled_pane_;
  bool floating_{false};
  bool floats_visible_{true};
};

struct Tab final {
  Tab(TabId assigned_id, PaneId first_pane) noexcept;

  Tab(const Tab&) = delete;
  auto operator=(const Tab&) -> Tab& = delete;
  Tab(Tab&&) = delete;
  auto operator=(Tab&&) -> Tab& = delete;
  ~Tab() = default;

  [[nodiscard]] auto title_override() const noexcept -> std::string_view;
  [[nodiscard]] auto set_title_override(std::string_view title) noexcept -> bool;

  // Focus has one owner per layer: the tiled focus is always a layout Pane, and the float layer
  // is either unfocused or focused on its top float. The effective focused Pane is derived.
  [[nodiscard]] auto focused_pane() const noexcept -> PaneId {
    const auto top = focus.floating_ ? floats.top() : std::nullopt;
    return top.value_or(focus.tiled_pane_);
  }
  [[nodiscard]] constexpr auto tiled_focus() const noexcept -> PaneId { return focus.tiled_pane_; }
  [[nodiscard]] constexpr auto float_focused() const noexcept -> bool { return focus.floating_; }
  [[nodiscard]] constexpr auto floats_visible() const noexcept -> bool {
    return focus.floats_visible_;
  }
  [[nodiscard]] auto is_float(const PaneId pane) const noexcept -> bool {
    return !floats.empty() && floats.contains(pane);
  }
  // A float can hold focus only while its layer is visible and its placement fits a usable Tab.
  [[nodiscard]] auto float_presentable(PaneId pane) const noexcept -> bool;
  // Moves the tiled focus without changing which layer holds keyboard focus.
  void set_tiled_focus(PaneId pane) noexcept;
  // Focuses a layout Pane; the float layer stays visible but loses focus.
  void focus_tiled(PaneId pane) noexcept;
  // Raises and focuses a presentable float.
  [[nodiscard]] auto focus_float(PaneId pane) noexcept -> bool;
  // Returns keyboard focus to the tiled layer.
  constexpr void blur_floats() noexcept { focus.floating_ = false; }
  // Hidden floats cannot hold focus.
  constexpr void set_floats_visible(const bool visible) noexcept {
    focus.floats_visible_ = visible;
    focus.floating_ = focus.floating_ && visible;
  }

  TabId id;
  PaneLayout layout;
  // Floating Panes above the tiled layout, back to front. Mutating it directly is reserved for
  // SessionMachine transitions, which re-establish float focus afterwards.
  FloatLayer floats;
  // Inactive tabs retain their last usable geometry while continuing to process PTY output.
  std::uint16_t layout_column{0};
  std::uint16_t layout_row{0};
  std::uint16_t layout_columns{80};
  std::uint16_t layout_rows{24};
  PaneId previous_pane;
  TabTitleOverride title;
  bool zoomed{false};
  bool layout_suspended{false};

  TabFocus focus;
};

struct TabSlot final {
  std::unique_ptr<Tab> tab;
  std::uint32_t generation{0};
};

enum class CopyModeFeedback : std::uint8_t {
  none,
  no_match,
  empty_selection,
  clipboard_busy,
  too_large,
  failed,
};

enum class CopySearchDirection : std::uint8_t {
  forward,
  backward,
};

enum class CopyModePhase : std::uint8_t {
  inactive,
  navigation,
  visual_character,
  visual_line,
  visual_block,
  search_prompt,
  searching,
};

struct CopyModeState final {
  std::array<char, limits::search_query_bytes_max> query{};
  std::array<char, limits::search_query_bytes_max> draft_query{};
  std::size_t query_size{0};
  std::size_t draft_query_size{0};
  // Explicit copy mode snapshots an absolute viewport policy across reflow/output mutations.
  // Ordinary wheel scrolling remains terminal-owned and does not populate this value.
  std::uint64_t viewport_offset{0};
  CopyModeFeedback feedback{CopyModeFeedback::none};
  CopySearchDirection search_direction{CopySearchDirection::forward};
  CopySearchDirection prompt_search_direction{CopySearchDirection::forward};
  CopyModePhase phase{CopyModePhase::inactive};
  CopyModePhase phase_before_search{CopyModePhase::navigation};

  [[nodiscard]] constexpr auto active() const noexcept -> bool {
    return phase != CopyModePhase::inactive;
  }
  [[nodiscard]] constexpr auto selecting() const noexcept -> bool {
    return phase == CopyModePhase::visual_character || phase == CopyModePhase::visual_line ||
           phase == CopyModePhase::visual_block;
  }
  [[nodiscard]] auto query_view() const noexcept -> std::string_view {
    return {query.data(), query_size};
  }
  [[nodiscard]] auto draft_query_view() const noexcept -> std::string_view {
    return {draft_query.data(), draft_query_size};
  }
};

enum class RenamePromptKind : std::uint8_t {
  inactive,
  session,
  tab,
};

enum class RenamePromptFeedback : std::uint8_t {
  none,
  invalid,
  conflict,
};

// The Attachment owns transient editing state. Stable semantic identity is captured when the
// prompt opens; no Session or Tab mutation occurs until the completed value is dispatched.
struct RenamePromptState final {
  std::array<char, limits::tab_title_bytes_max> text{};
  std::size_t size{0};
  std::size_t cursor{0};
  TabId tab;
  RenamePromptKind kind{RenamePromptKind::inactive};
  RenamePromptFeedback feedback{RenamePromptFeedback::none};

  [[nodiscard]] constexpr auto active() const noexcept -> bool {
    return kind != RenamePromptKind::inactive;
  }
  [[nodiscard]] auto view() const noexcept -> std::string_view { return {text.data(), size}; }
};

enum class StatusMessageKind : std::uint8_t {
  information,
  error,
};

struct StatusMessageEntry final {
  std::array<char, limits::status_message_bytes_max> text{};
  std::uint16_t size{0};
  std::int64_t unix_seconds{0};
  StatusMessageKind kind{StatusMessageKind::information};

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {text.data(), size}; }
};

struct StatusMessageLog final {
  // The log is attachment-owned and newest-first. Messages remain available after their transient
  // status projection expires without introducing an unbounded diagnostic sink.
  std::array<StatusMessageEntry, limits::status_message_history_max> entries{};
  std::uint8_t size{0};
};

struct MessageViewState final {
  std::uint8_t offset{0};
  bool active{false};
};

struct CommandLineState final {
  std::array<char, limits::command_line_bytes_max> text{};
  std::size_t size{0};
  std::size_t cursor{0};
  std::uint8_t history_index{0};
  bool active{false};
  bool submit_requested{false};
  bool completion_requested{false};

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {text.data(), size}; }
};

struct AttachmentPaneTarget final {
  TabId tab;
  PaneId pane;

  friend constexpr auto operator==(const AttachmentPaneTarget&,
                                   const AttachmentPaneTarget&) noexcept -> bool = default;
};

enum class MouseCaptureOwner : std::uint8_t {
  application,
  selection,
  divider,
  status_tab,
  discard_until_release,
};

struct MouseCapture final {
  AttachmentPaneTarget target;
  // Divider capture uses target.pane and peer_pane as generation-safe subtree representatives.
  // The Attachment owns this ephemeral gesture identity; PaneLayout remains the ratio authority.
  PaneId peer_pane;
  // Status-tab capture owns a non-authoritative live ordering preview. The stable source is
  // target.tab and this stable anchor has the same "place before" meaning as TabPlacementCommand;
  // an invalid anchor means the end.
  TabId status_tab_before;
  MouseCaptureOwner owner{MouseCaptureOwner::application};
  SplitAxis divider_axis{SplitAxis::left_right};
};

struct Attachment final {
  AttachmentId id;
  SessionId session;
  std::uint16_t columns{80};
  std::uint16_t rows{24};
  // The Pane viewport inside the physical geometry after native chrome and docks, as last
  // committed by SessionMachine. Tab activation and creation fit this rectangle directly, so a
  // docked Attachment never sizes a Tab to the full geometry first.
  PaneRectangle content_viewport{.columns = 80, .rows = 24};
  std::optional<AttachmentPaneTarget> selection_target;
  std::optional<MouseCapture> mouse_capture;
  CopyModeState copy_mode;
  RenamePromptState rename_prompt;
  CommandLineState command_line;
  CommandLineHistory command_history;
  StatusMessageLog status_messages;
  MessageViewState message_view;
  bool status_message_visible{false};
};

struct Session {
  Session(std::string_view session_name, std::string_view initial_working_directory,
          std::span<const std::byte> initial_environment,
          LaunchEnvironmentMode initial_environment_mode) noexcept;

  Session(const Session&) = delete;
  auto operator=(const Session&) -> Session& = delete;
  Session(Session&&) = delete;
  auto operator=(Session&&) -> Session& = delete;
  ~Session() = default;

  [[nodiscard]] auto session_name() const noexcept -> std::string_view;
  [[nodiscard]] auto rename(std::string_view session_name) noexcept -> bool;
  [[nodiscard]] auto cwd() const noexcept -> std::string_view;
  [[nodiscard]] auto launch_environment() const noexcept -> std::span<const std::byte>;

  SessionId id;
  std::array<char, limits::session_name_bytes_max> name{};
  std::size_t name_size{0};
  std::array<char, limits::working_directory_bytes_max + 1U> working_directory{};
  std::size_t working_directory_size{0};
  std::array<std::byte, limits::environment_bytes_max> environment{};
  std::size_t environment_size{0};
  LaunchEnvironmentMode environment_mode{LaunchEnvironmentMode::inherit};
  // Core-owned ordering used only to resolve an omitted CLI attach target.
  std::uint64_t activity_order{0};
  // Monotonic post-commit observation generation. It is metadata, not a second state authority.
  std::uint64_t mutation_generation{1};
  std::array<PaneSlot, panes_per_session_max> panes{};
  std::array<TabSlot, tabs_per_session_max> tabs{};
  // Attachment interaction state is semantic Core state. Connection decoders, output queues, and
  // presentation caches remain in AttachmentRuntime outside this model.
  Attachment attachment;
  TabOrder tab_order;
  TabId active_tab;
  TabId previous_tab;
  bool active{true};
  bool theme_bound{false};
};

} // namespace lemma::core

#endif // LEMMA_CORE_SESSION_HPP
