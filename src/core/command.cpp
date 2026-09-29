#include "frame/command.hpp"

#include "frame/limits.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <variant>

namespace frame {
namespace {

[[nodiscard]] constexpr auto valid_kind(const CommandKind kind) noexcept -> bool {
  switch (kind) {
  case CommandKind::none:
    return false;
  case CommandKind::detach_client:
  case CommandKind::cancel_attachment_interaction:
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
  case CommandKind::enter_copy_mode:
  case CommandKind::enter_copy_search_forward:
  case CommandKind::enter_copy_search_backward:
  case CommandKind::copy_selection:
  case CommandKind::create_tab:
  case CommandKind::next_tab:
  case CommandKind::previous_tab:
  case CommandKind::close_tab:
  case CommandKind::select_tab:
  case CommandKind::begin_rename_session:
  case CommandKind::begin_rename_tab:
  case CommandKind::rename_session:
  case CommandKind::rename_tab:
  case CommandKind::place_tab:
  case CommandKind::swap_panes:
  case CommandKind::stop_session:
    return true;
  }
  return false;
}

[[nodiscard]] constexpr auto valid_origin(const CommandOrigin origin) noexcept -> bool {
  switch (origin) {
  case CommandOrigin::none:
    return false;
  case CommandOrigin::client:
  case CommandOrigin::cli:
  case CommandOrigin::keymap:
  case CommandOrigin::internal:
    return true;
  }
  return false;
}

[[nodiscard]] constexpr auto divider_resize_command(const CommandKind kind) noexcept -> bool {
  return kind == CommandKind::resize_left_right_divider ||
         kind == CommandKind::resize_top_bottom_divider;
}

[[nodiscard]] constexpr auto directional_resize_command(const CommandKind kind) noexcept -> bool {
  return kind == CommandKind::resize_left || kind == CommandKind::resize_right ||
         kind == CommandKind::resize_up || kind == CommandKind::resize_down;
}

[[nodiscard]] auto valid_payload(const Command& command) noexcept -> bool {
  if (command.kind == CommandKind::select_tab) {
    const auto* const coordinate = std::get_if<CommandCoordinate>(&command.payload);
    return coordinate != nullptr && coordinate->value < command_tab_slots_max;
  }
  if (divider_resize_command(command.kind)) {
    return std::holds_alternative<CommandCoordinate>(command.payload);
  }
  if (directional_resize_command(command.kind)) {
    const auto* const amount = std::get_if<CommandCoordinate>(&command.payload);
    return std::holds_alternative<std::monostate>(command.payload) ||
           (amount != nullptr && amount->value > 0 && amount->value <= command_resize_amount_max);
  }
  if (command.kind == CommandKind::rename_session) {
    const auto* const name = std::get_if<SessionNameValue>(&command.payload);
    return name != nullptr && name->valid();
  }
  if (command.kind == CommandKind::rename_tab) {
    return std::holds_alternative<TabTitleValue>(command.payload);
  }
  if (command.kind == CommandKind::place_tab) {
    return std::holds_alternative<TabPlacementCommand>(command.payload);
  }
  if (command.kind == CommandKind::swap_panes) {
    const auto* const swap = std::get_if<PaneSwapCommand>(&command.payload);
    return swap != nullptr && swap->other.is_valid() && swap->other != command.target.pane;
  }
  if (command.kind == CommandKind::set_zoom) {
    return std::holds_alternative<PaneZoomCommand>(command.payload);
  }
  return std::holds_alternative<std::monostate>(command.payload);
}

[[nodiscard]] constexpr auto valid_target_hierarchy(const CommandTarget& target) noexcept -> bool {
  const bool has_session = target.session.is_valid();
  const bool has_tab = target.tab.is_valid();
  const bool has_pane = target.pane.is_valid();
  const bool has_peer_pane = target.peer_pane.is_valid();
  const bool has_attachment = target.attachment.is_valid();
  return (!has_tab || has_session) && (!has_pane || (has_session && has_tab)) &&
         (!has_peer_pane || (has_session && has_tab && has_pane)) &&
         (!has_attachment || has_session);
}

// Target-shape validation is deliberately exhaustive over command-specific ID requirements.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] constexpr auto valid_target_shape(const Command& command) noexcept -> bool {
  const bool has_tab = command.target.tab.is_valid();
  const bool has_pane = command.target.pane.is_valid();
  const bool has_peer_pane = command.target.peer_pane.is_valid();
  const bool has_attachment = command.target.attachment.is_valid();
  const bool divider_resize = divider_resize_command(command.kind);
  if (divider_resize != has_peer_pane ||
      (has_peer_pane && command.target.peer_pane == command.target.pane)) {
    return false;
  }
  if (command.kind == CommandKind::stop_session &&
      (has_tab || has_pane || has_peer_pane || has_attachment)) {
    return false;
  }
  if (command.kind == CommandKind::rename_session && (has_tab || has_pane || has_peer_pane)) {
    return false;
  }
  if (command.kind == CommandKind::begin_rename_session ||
      command.kind == CommandKind::cancel_attachment_interaction) {
    return !has_tab && !has_pane && !has_peer_pane && has_attachment;
  }
  if (command.kind == CommandKind::detach_client) {
    return !has_tab && !has_pane && !has_peer_pane;
  }
  if (command.kind == CommandKind::begin_rename_tab) {
    return has_tab && !has_pane && !has_peer_pane && has_attachment;
  }
  if (command.kind == CommandKind::rename_tab || command.kind == CommandKind::place_tab) {
    return has_tab && !has_pane && !has_peer_pane;
  }
  if (command.kind == CommandKind::swap_panes) {
    return has_tab && has_pane && !has_peer_pane;
  }
  return true;
}

[[nodiscard]] constexpr auto valid_target(const Command& command) noexcept -> bool {
  return valid_target_hierarchy(command.target) && valid_target_shape(command);
}

} // namespace

auto SessionNameValue::create(const std::string_view value) noexcept
    -> std::optional<SessionNameValue> {
  if (value.empty() || value.front() == '-' || value.size() > limits::session_name_bytes_max ||
      !std::ranges::all_of(value, [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
      })) {
    return std::nullopt;
  }
  SessionNameValue result;
  std::memcpy(result.bytes_.data(), value.data(), value.size());
  result.size_ = static_cast<std::uint8_t>(value.size());
  return result;
}

auto TabTitleValue::create(const std::string_view value) noexcept -> std::optional<TabTitleValue> {
  if (value.size() > limits::tab_title_bytes_max ||
      !std::ranges::all_of(value, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte <= 0x7eU;
      })) {
    return std::nullopt;
  }
  TabTitleValue result;
  if (!value.empty()) {
    std::memcpy(result.bytes_.data(), value.data(), value.size());
  }
  result.size_ = static_cast<std::uint8_t>(value.size());
  return result;
}

auto CommandDispatcher::dispatch(const Command& command) const noexcept -> CommandResult {
  const auto complete = [&](const CommandResult result) {
    if (observer_ != nullptr) {
      observer_(observer_context_, command, result);
    }
    return result;
  };
  if (!valid_kind(command.kind) || !valid_origin(command.origin) || !valid_payload(command)) {
    return complete({.status = CommandStatus::invalid_command});
  }
  if (!valid_target(command)) {
    return complete({.status = CommandStatus::invalid_target});
  }
  if (executor_ == nullptr) {
    return complete({.status = CommandStatus::failed});
  }
  return complete(executor_(context_, command));
}

} // namespace frame
