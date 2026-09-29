#ifndef FRAME_COMMAND_HPP
#define FRAME_COMMAND_HPP

#include "frame/id.hpp"
#include "frame/limits.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace frame {

inline constexpr std::uint16_t command_tab_slots_max = 16;
inline constexpr std::uint16_t command_resize_amount_max = 100;

enum class CommandKind : std::uint8_t {
  none = 0,
  detach_client,
  cancel_attachment_interaction,
  split_left_right,
  split_top_bottom,
  resize_left_right_divider,
  resize_top_bottom_divider,
  resize_left,
  resize_right,
  resize_up,
  resize_down,
  focus_left,
  focus_right,
  focus_up,
  focus_down,
  focus_next,
  focus_previous,
  focus_pane,
  close_pane,
  toggle_zoom,
  set_zoom,
  enter_copy_mode,
  enter_copy_search_forward,
  enter_copy_search_backward,
  copy_selection,
  create_tab,
  next_tab,
  previous_tab,
  close_tab,
  select_tab,
  begin_rename_session,
  begin_rename_tab,
  rename_session,
  rename_tab,
  place_tab,
  swap_panes,
  stop_session,
};

enum class CommandOrigin : std::uint8_t {
  none = 0,
  client,
  cli,
  keymap,
  internal,
};

// Invalid IDs mean "the current object". Explicit IDs are retained in the command value so CLI
// and remote transports can use the same dispatcher without exposing core pointers.
struct CommandTarget final {
  SessionId session;
  TabId tab;
  PaneId pane;
  // Divider commands identify both structural subtrees with generation-safe pane representatives.
  PaneId peer_pane;
  AttachmentId attachment;
};

class SessionNameValue final {
public:
  [[nodiscard]] static auto create(std::string_view value) noexcept
      -> std::optional<SessionNameValue>;
  [[nodiscard]] auto view() const noexcept -> std::string_view { return {bytes_.data(), size_}; }
  [[nodiscard]] constexpr auto valid() const noexcept -> bool { return size_ > 0; }

private:
  std::array<char, limits::session_name_bytes_max> bytes_{};
  std::uint8_t size_{0};
};

class TabTitleValue final {
public:
  [[nodiscard]] static auto create(std::string_view value) noexcept -> std::optional<TabTitleValue>;
  [[nodiscard]] auto view() const noexcept -> std::string_view { return {bytes_.data(), size_}; }
  [[nodiscard]] constexpr auto valid() const noexcept -> bool {
    return size_ <= limits::tab_title_bytes_max;
  }

private:
  std::array<char, limits::tab_title_bytes_max> bytes_{};
  std::uint8_t size_{0};
};

struct CommandCoordinate final {
  std::uint16_t value{0};
};

struct TabPlacementCommand final {
  // Invalid means place at the end.
  TabId before;
};

struct PaneSwapCommand final {
  PaneId other;
};

struct PaneZoomCommand final {
  bool enabled{false};
};

using CommandPayload =
    std::variant<std::monostate, CommandCoordinate, SessionNameValue, TabTitleValue,
                 TabPlacementCommand, PaneSwapCommand, PaneZoomCommand>;

struct Command final {
  CommandKind kind{CommandKind::none};
  CommandOrigin origin{CommandOrigin::none};
  CommandTarget target{};
  CommandPayload payload{}; // NOLINT(readability-redundant-member-init)
};

enum class CommandStatus : std::uint8_t {
  applied,
  no_effect,
  detach_requested,
  invalid_command,
  invalid_target,
  stale_target,
  wrong_owner,
  conflict,
  capacity,
  unavailable,
  failed,
};

struct CommandResult final {
  CommandStatus status{CommandStatus::failed};

  [[nodiscard]] constexpr auto succeeded() const noexcept -> bool {
    return status == CommandStatus::applied || status == CommandStatus::no_effect ||
           status == CommandStatus::detach_requested;
  }
};

using CommandExecutor = CommandResult (*)(void* context, const Command& command) noexcept;
using CommandObserver = void (*)(void* context, const Command& command,
                                 CommandResult result) noexcept;

// Validates bounded command values before invoking the reactor-owned state transition function.
// The dispatcher itself performs no allocation or I/O.
class CommandDispatcher final {
public:
  constexpr CommandDispatcher(const CommandExecutor executor, void* const context,
                              const CommandObserver observer = nullptr,
                              void* const observer_context = nullptr) noexcept
      : executor_(executor), context_(context), observer_(observer),
        observer_context_(observer_context) {}

  [[nodiscard]] auto dispatch(const Command& command) const noexcept -> CommandResult;

private:
  CommandExecutor executor_{nullptr};
  void* context_{nullptr};
  CommandObserver observer_{nullptr};
  void* observer_context_{nullptr};
};

} // namespace frame

#endif // FRAME_COMMAND_HPP
