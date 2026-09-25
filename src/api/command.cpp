#include "api/command.hpp"

#include "api/json.hpp"
#include "lemma/command.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace lemma::api {
namespace {

template <typename Id>
[[nodiscard]] auto parse_id(const std::string_view value) -> std::optional<Id> {
  const auto separator = value.find(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1U == value.size()) {
    return std::nullopt;
  }
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;
  const auto slot_text = value.substr(0, separator);
  const auto generation_text = value.substr(separator + 1U);
  const auto slot_result = std::from_chars(slot_text.begin(), slot_text.end(), slot);
  const auto generation_result =
      std::from_chars(generation_text.begin(), generation_text.end(), generation);
  return slot_result.ec == std::errc{} && slot_result.ptr == slot_text.end() &&
                 generation_result.ec == std::errc{} &&
                 generation_result.ptr == generation_text.end()
             ? Id::try_from_parts(slot, generation)
             : std::nullopt;
}

[[nodiscard]] auto unknown_field(const JsonValue& object,
                                 const std::initializer_list<std::string_view> allowed) noexcept
    -> std::optional<std::string_view> {
  for (const auto& entry : object.object) {
    if (std::ranges::find(allowed, std::string_view(entry.key)) == allowed.end()) {
      return entry.key;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto decode_session_selector(const JsonValue& document)
    -> std::optional<SessionSelector> {
  const auto* const value = json_member(document, "session");
  if (value == nullptr || value->kind != JsonKind::object ||
      unknown_field(*value, {"id", "name"}).has_value()) {
    return std::nullopt;
  }
  const auto id_text = json_string(*value, "id");
  const auto name = json_string(*value, "name");
  if (id_text.has_value() == name.has_value() || value->object.size() != 1U) {
    return std::nullopt;
  }
  if (id_text.has_value()) {
    const auto id = parse_id<SessionId>(*id_text);
    return id.has_value() ? std::optional{SessionSelector{.id = *id, .name = {}}} : std::nullopt;
  }
  return name.has_value() && SessionNameValue::create(*name).has_value()
             ? std::optional{SessionSelector{.id = {}, .name = std::string(*name)}}
             : std::nullopt;
}

[[nodiscard]] auto decode_tab_selector(const JsonValue& document) -> std::optional<TabSelector> {
  const auto* const value = json_member(document, "tab");
  if (value == nullptr || value->kind != JsonKind::object ||
      unknown_field(*value, {"id", "position"}).has_value()) {
    return std::nullopt;
  }
  const auto id_text = json_string(*value, "id");
  const auto position = json_unsigned(*value, "position");
  if (id_text.has_value() == position.has_value() || value->object.size() != 1U) {
    return std::nullopt;
  }
  if (id_text.has_value()) {
    const auto id = parse_id<TabId>(*id_text);
    return id.has_value() ? std::optional{TabSelector{.id = *id, .position = 0}} : std::nullopt;
  }
  return position.has_value() && *position > 0 && *position <= command_tab_slots_max
             ? std::optional{TabSelector{.id = {},
                                         .position = static_cast<std::uint16_t>(*position)}}
             : std::nullopt;
}

[[nodiscard]] auto decode_pane_selector(const JsonValue& document, const std::string_view field)
    -> std::optional<PaneSelector> {
  const auto* const value = json_member(document, field);
  if (value == nullptr || value->kind != JsonKind::object || value->object.size() != 1U ||
      unknown_field(*value, {"id"}).has_value()) {
    return std::nullopt;
  }
  const auto id_text = json_string(*value, "id");
  const auto id = id_text.has_value() ? parse_id<PaneId>(*id_text) : std::nullopt;
  return id.has_value() ? std::optional{PaneSelector{.id = *id}} : std::nullopt;
}

[[nodiscard]] auto decode_surface_selector(const JsonValue& document)
    -> std::optional<SurfaceSelector> {
  const auto* const value = json_member(document, "surface");
  if (value == nullptr || value->kind != JsonKind::object || value->object.size() != 1U ||
      unknown_field(*value, {"id"}).has_value()) {
    return std::nullopt;
  }
  const auto id_text = json_string(*value, "id");
  const auto id = id_text.has_value() ? parse_id<SurfaceId>(*id_text) : std::nullopt;
  return id.has_value() ? std::optional{SurfaceSelector{.id = *id}} : std::nullopt;
}

[[nodiscard]] auto decode_dock_placement(const JsonValue& value, const std::string_view kind)
    -> std::optional<SurfacePlacement> {
  if (unknown_field(value, {"kind", "size"}).has_value() || value.object.size() != 2U) {
    return std::nullopt;
  }
  auto placement_kind = SurfacePlacementKind::dock_left;
  if (kind == "dock.left") {
    placement_kind = SurfacePlacementKind::dock_left;
  } else if (kind == "dock.right") {
    placement_kind = SurfacePlacementKind::dock_right;
  } else if (kind == "dock.top") {
    placement_kind = SurfacePlacementKind::dock_top;
  } else if (kind == "dock.bottom") {
    placement_kind = SurfacePlacementKind::dock_bottom;
  } else {
    return std::nullopt;
  }
  const bool horizontal = placement_kind == SurfacePlacementKind::dock_left ||
                          placement_kind == SurfacePlacementKind::dock_right;
  const auto size = json_unsigned(value, "size");
  const auto maximum =
      horizontal ? limits::terminal_columns_hard_max : limits::terminal_rows_hard_max;
  if (!size.has_value() || *size == 0 || *size > maximum) {
    return std::nullopt;
  }
  return horizontal
             ? SurfacePlacement{.kind = placement_kind,
                                .columns = static_cast<std::uint16_t>(*size)}
             : SurfacePlacement{.kind = placement_kind, .rows = static_cast<std::uint16_t>(*size)};
}

[[nodiscard]] auto decode_floating_placement(const JsonValue& value, const std::string_view kind)
    -> std::optional<SurfacePlacement> {
  if ((kind != "float" && kind != "overlay") ||
      unknown_field(value, {"kind", "column", "row", "columns", "rows"}).has_value() ||
      value.object.size() != 5U) {
    return std::nullopt;
  }
  const auto column = json_unsigned(value, "column");
  const auto row = json_unsigned(value, "row");
  const auto columns = json_unsigned(value, "columns");
  const auto rows = json_unsigned(value, "rows");
  if (!column.has_value() || !row.has_value() || !columns.has_value() || !rows.has_value() ||
      *column > std::numeric_limits<std::uint16_t>::max() ||
      *row > std::numeric_limits<std::uint16_t>::max() || *columns == 0 ||
      *columns > limits::terminal_columns_hard_max || *rows == 0 ||
      *rows > limits::terminal_rows_hard_max) {
    return std::nullopt;
  }
  return SurfacePlacement{
      .kind = kind == "float" ? SurfacePlacementKind::float_surface : SurfacePlacementKind::overlay,
      .column = static_cast<std::uint16_t>(*column),
      .row = static_cast<std::uint16_t>(*row),
      .columns = static_cast<std::uint16_t>(*columns),
      .rows = static_cast<std::uint16_t>(*rows),
  };
}

[[nodiscard]] auto decode_surface_placement(const JsonValue& document)
    -> std::optional<SurfacePlacement> {
  const auto* const value = json_member(document, "placement");
  if (value == nullptr || value->kind != JsonKind::object) {
    return std::nullopt;
  }
  const auto kind = json_string(*value, "kind");
  if (!kind.has_value()) {
    return std::nullopt;
  }
  const auto dock = decode_dock_placement(*value, *kind);
  return dock.has_value() ? dock : decode_floating_placement(*value, *kind);
}

[[nodiscard]] auto decode_arguments(const JsonValue& document, Command& command) -> bool {
  const auto* const value = json_member(document, "argv");
  if (value == nullptr) {
    return true;
  }
  if (value->kind != JsonKind::array || value->array.empty() ||
      value->array.size() > limits::command_arguments_hard_max) {
    return false;
  }
  std::size_t bytes = 0;
  for (const auto& argument : value->array) {
    if (argument.kind != JsonKind::string || argument.string.contains('\0') ||
        (command.arguments.empty() && argument.string.empty()) ||
        argument.string.size() + 1U > limits::command_bytes_hard_max - bytes) {
      return false;
    }
    bytes += argument.string.size() + 1U;
    command.arguments.push_back(argument.string);
  }
  return true;
}

[[nodiscard]] auto decode_environment(const JsonValue& document, Command& command) -> bool {
  const auto* const value = json_member(document, "environment");
  if (value == nullptr) {
    return true;
  }
  if (value->kind != JsonKind::array || value->array.size() > limits::environment_entries_max) {
    return false;
  }
  std::size_t bytes = 0;
  for (const auto& entry : value->array) {
    if (entry.kind != JsonKind::string || entry.string.contains('\0')) {
      return false;
    }
    const auto separator = entry.string.find('=');
    if (separator == 0 || separator == std::string::npos ||
        entry.string.size() + 1U > limits::environment_bytes_max - bytes) {
      return false;
    }
    bytes += entry.string.size() + 1U;
    command.environment.push_back(entry.string);
  }
  command.environment_set = true;
  return true;
}

[[nodiscard]] auto decode_launch(const JsonValue& document, Command& command,
                                 const bool title_allowed) -> std::optional<std::string_view> {
  if (const auto* const cwd = json_member(document, "cwd"); cwd != nullptr) {
    if (cwd->kind != JsonKind::string || cwd->string.empty() || cwd->string.front() != '/' ||
        cwd->string.contains('\0') || cwd->string.size() > limits::working_directory_bytes_max) {
      return "cwd";
    }
    command.working_directory = cwd->string;
  }
  if (const auto* const hold = json_member(document, "hold"); hold != nullptr) {
    if (hold->kind != JsonKind::boolean) {
      return "hold";
    }
    command.hold = hold->boolean;
  }
  if (const auto* const title = json_member(document, "title"); title != nullptr) {
    if (!title_allowed || title->kind != JsonKind::string ||
        !TabTitleValue::create(title->string).has_value()) {
      return "title";
    }
    command.title = title->string;
  }
  return decode_arguments(document, command) ? std::nullopt
                                             : std::optional<std::string_view>{"argv"};
}

[[nodiscard]] auto decode_focus_policy(const JsonValue& document, Command& command) noexcept
    -> bool {
  const auto* const value = json_member(document, "focus");
  if (value == nullptr) {
    return true;
  }
  const auto name = json_string(document, "focus");
  if (name == std::optional<std::string_view>{"created"}) {
    command.focus = FocusPolicy::created;
    return true;
  }
  if (name == std::optional<std::string_view>{"preserve"}) {
    command.focus = FocusPolicy::preserve;
    return true;
  }
  return false;
}

constexpr std::array input_key_names{
    std::string_view{"a"},       std::string_view{"b"},         std::string_view{"c"},
    std::string_view{"d"},       std::string_view{"e"},         std::string_view{"f"},
    std::string_view{"g"},       std::string_view{"h"},         std::string_view{"i"},
    std::string_view{"j"},       std::string_view{"k"},         std::string_view{"l"},
    std::string_view{"m"},       std::string_view{"n"},         std::string_view{"o"},
    std::string_view{"p"},       std::string_view{"q"},         std::string_view{"r"},
    std::string_view{"s"},       std::string_view{"t"},         std::string_view{"u"},
    std::string_view{"v"},       std::string_view{"w"},         std::string_view{"x"},
    std::string_view{"y"},       std::string_view{"z"},         std::string_view{"enter"},
    std::string_view{"tab"},     std::string_view{"backspace"}, std::string_view{"escape"},
    std::string_view{"space"},   std::string_view{"up"},        std::string_view{"down"},
    std::string_view{"left"},    std::string_view{"right"},     std::string_view{"home"},
    std::string_view{"end"},     std::string_view{"insert"},    std::string_view{"delete"},
    std::string_view{"page_up"}, std::string_view{"page_down"}, std::string_view{"f1"},
    std::string_view{"f2"},      std::string_view{"f3"},        std::string_view{"f4"},
    std::string_view{"f5"},      std::string_view{"f6"},        std::string_view{"f7"},
    std::string_view{"f8"},      std::string_view{"f9"},        std::string_view{"f10"},
    std::string_view{"f11"},     std::string_view{"f12"},
};
static_assert(input_key_names.size() == static_cast<std::size_t>(InputKey::f12) + 1U);

// Branches mirror the closed text, paste, and logical-key event schemas.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto decode_input_events(const JsonValue& document, Command& command) -> bool {
  const auto* const value = json_member(document, "events");
  if (value == nullptr || value->kind != JsonKind::array || value->array.empty() ||
      value->array.size() > input_events_max) {
    return false;
  }
  std::size_t text_bytes = 0;
  for (const auto& encoded : value->array) {
    if (encoded.kind != JsonKind::object) {
      return false;
    }
    const auto kind = json_string(encoded, "kind");
    if (kind == std::optional<std::string_view>{"text"} ||
        kind == std::optional<std::string_view>{"paste"}) {
      if (unknown_field(encoded, {"kind", "text"}).has_value()) {
        return false;
      }
      const auto text = json_string(encoded, "text");
      if (!text.has_value() || text->empty() ||
          text->size() > limits::environment_bytes_max - text_bytes) {
        return false;
      }
      text_bytes += text->size();
      command.input_events.push_back(
          {.kind = *kind == "text" ? InputEventKind::text : InputEventKind::paste,
           .text = std::string(*text)});
      continue;
    }
    if (kind != std::optional<std::string_view>{"key"} ||
        unknown_field(encoded, {"kind", "key", "modifiers", "phase"}).has_value()) {
      return false;
    }
    const auto key = json_string(encoded, "key");
    const auto parsed_key = key.has_value() ? parse_input_key_name(*key) : std::nullopt;
    if (!parsed_key.has_value()) {
      return false;
    }
    InputEvent event{.kind = InputEventKind::key, .text = {}, .key = *parsed_key};
    if (const auto* const modifiers = json_member(encoded, "modifiers"); modifiers != nullptr) {
      if (modifiers->kind != JsonKind::array || modifiers->array.size() > 4U) {
        return false;
      }
      for (const auto& modifier : modifiers->array) {
        if (modifier.kind != JsonKind::string) {
          return false;
        }
        std::uint16_t bit = 0;
        if (modifier.string == "shift") {
          bit = input_modifier_shift;
        } else if (modifier.string == "control") {
          bit = input_modifier_control;
        } else if (modifier.string == "alt") {
          bit = input_modifier_alt;
        } else if (modifier.string == "super") {
          bit = input_modifier_super;
        } else {
          return false;
        }
        if ((event.modifiers & bit) != 0) {
          return false;
        }
        event.modifiers |= bit;
      }
    }
    if (const auto* const key_phase = json_member(encoded, "phase"); key_phase != nullptr) {
      const auto name = json_string(encoded, "phase");
      if (name == std::optional<std::string_view>{"press"}) {
        event.phase = InputKeyPhase::press;
      } else if (name == std::optional<std::string_view>{"repeat"}) {
        event.phase = InputKeyPhase::repeat;
      } else if (name == std::optional<std::string_view>{"release"}) {
        event.phase = InputKeyPhase::release;
      } else {
        return false;
      }
    }
    command.input_events.push_back(std::move(event));
  }
  return true;
}

[[nodiscard]] auto failure(const std::string_view reason, const std::string_view field = {})
    -> CommandDecodeResult {
  return {.command = std::nullopt, .error = {.reason = reason, .field = field}};
}

[[nodiscard]] auto require_session(const JsonValue& document, Command& command)
    -> std::optional<CommandDecodeResult> {
  const auto selector = decode_session_selector(document);
  if (!selector.has_value()) {
    return failure("invalid_selector", "session");
  }
  command.session = *selector;
  return std::nullopt;
}

[[nodiscard]] auto require_tab(const JsonValue& document, Command& command)
    -> std::optional<CommandDecodeResult> {
  const auto selector = decode_tab_selector(document);
  if (!selector.has_value()) {
    return failure("invalid_selector", "tab");
  }
  command.tab = *selector;
  return std::nullopt;
}

[[nodiscard]] auto require_pane(const JsonValue& document, Command& command,
                                const std::string_view field = "pane")
    -> std::optional<CommandDecodeResult> {
  const auto selector = decode_pane_selector(document, field);
  if (!selector.has_value()) {
    return failure("invalid_selector", field);
  }
  if (field == "pane") {
    command.pane = *selector;
  } else {
    command.other = *selector;
  }
  return std::nullopt;
}

} // namespace

auto parse_input_key_name(const std::string_view value) noexcept -> std::optional<InputKey> {
  const auto* const found = std::ranges::find(input_key_names, value);
  if (found == input_key_names.end()) {
    return std::nullopt;
  }
  return static_cast<InputKey>(static_cast<std::size_t>(found - input_key_names.begin()));
}

auto input_key_name(const InputKey key) noexcept -> std::string_view {
  const auto index = static_cast<std::size_t>(key);
  return index < input_key_names.size() ? std::span(input_key_names).subspan(index, 1).front()
                                        : std::string_view{};
}

auto wait_condition_name(const WaitCondition condition) noexcept -> std::string_view {
  switch (condition) {
  case WaitCondition::process_exit:
    return "process-exit";
  case WaitCondition::exit_code:
    return "exit-code";
  case WaitCondition::signal:
    return "signal";
  case WaitCondition::contains:
    return "contains";
  case WaitCondition::prompt:
    return "prompt";
  case WaitCondition::command:
    return "command";
  }
  return {};
}

auto command_name(const CommandKind kind) noexcept -> std::string_view {
  switch (kind) {
  case CommandKind::daemon_inspect:
    return "daemon.inspect";
  case CommandKind::config_reload:
    return "config.reload";
  case CommandKind::attachment_switch:
    return "attachment.switch";
  case CommandKind::session_list:
    return "session.list";
  case CommandKind::session_inspect:
    return "session.inspect";
  case CommandKind::session_start:
    return "session.start";
  case CommandKind::session_rename:
    return "session.rename";
  case CommandKind::session_kill:
    return "session.kill";
  case CommandKind::tab_list:
    return "tab.list";
  case CommandKind::tab_inspect:
    return "tab.inspect";
  case CommandKind::tab_new:
    return "tab.new";
  case CommandKind::tab_select:
    return "tab.select";
  case CommandKind::tab_move:
    return "tab.move";
  case CommandKind::tab_rename:
    return "tab.rename";
  case CommandKind::tab_kill:
    return "tab.kill";
  case CommandKind::pane_list:
    return "pane.list";
  case CommandKind::pane_inspect:
    return "pane.inspect";
  case CommandKind::pane_split:
    return "pane.split";
  case CommandKind::pane_focus:
    return "pane.focus";
  case CommandKind::pane_swap:
    return "pane.swap";
  case CommandKind::pane_resize:
    return "pane.resize";
  case CommandKind::pane_zoom:
    return "pane.zoom";
  case CommandKind::pane_send:
    return "pane.send";
  case CommandKind::pane_input:
    return "pane.input";
  case CommandKind::pane_paste_image:
    return "pane.paste-image";
  case CommandKind::pane_capture:
    return "pane.capture";
  case CommandKind::pane_wait:
    return "pane.wait";
  case CommandKind::pane_kill:
    return "pane.kill";
  case CommandKind::surface_create:
    return "surface.create";
  case CommandKind::surface_configure:
    return "surface.configure";
  case CommandKind::surface_focus:
    return "surface.focus";
  case CommandKind::surface_close:
    return "surface.close";
  }
  return {};
}

[[nodiscard]] auto append_selector(std::string& output, const std::string_view field,
                                   const SessionSelector& selector) -> bool {
  try {
    output += ",\"";
    output += field;
    output += "\":{";
    if (selector.id.is_valid()) {
      output += R"("id":")" + std::to_string(selector.id.slot()) + ":" +
                std::to_string(selector.id.generation()) + "\"}";
      return true;
    }
    output += "\"name\":";
    return append_json_string(output, selector.name) && (output += "}", true);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto append_selector(std::string& output, const std::string_view field,
                                   const TabSelector& selector) -> bool {
  try {
    output += ",\"";
    output += field;
    if (selector.id.is_valid()) {
      output += R"(":{"id":")" + std::to_string(selector.id.slot()) + ":" +
                std::to_string(selector.id.generation()) + "\"}";
    } else {
      output += R"(":{"position":)" + std::to_string(selector.position) + "}";
    }
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto append_selector(std::string& output, const std::string_view field,
                                   const PaneSelector& selector) -> bool {
  if (!selector.id.is_valid()) {
    return false;
  }
  try {
    output += ",\"";
    output += field;
    output += R"(":{"id":")" + std::to_string(selector.id.slot()) + ":" +
              std::to_string(selector.id.generation()) + "\"}";
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto append_selector(std::string& output, const std::string_view field,
                                   const SurfaceSelector& selector) -> bool {
  if (!selector.id.is_valid()) {
    return false;
  }
  try {
    output += ",\"";
    output += field;
    output += R"(\":{\"id\":\")" + std::to_string(selector.id.slot()) + ":" +
              std::to_string(selector.id.generation()) + "\"}";
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] constexpr auto surface_placement_name(const SurfacePlacementKind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case SurfacePlacementKind::dock_left:
    return "dock.left";
  case SurfacePlacementKind::dock_right:
    return "dock.right";
  case SurfacePlacementKind::dock_top:
    return "dock.top";
  case SurfacePlacementKind::dock_bottom:
    return "dock.bottom";
  case SurfacePlacementKind::float_surface:
    return "float";
  case SurfacePlacementKind::overlay:
    return "overlay";
  }
  return {};
}

[[nodiscard]] auto append_surface_placement(std::string& output, const SurfacePlacement placement)
    -> bool {
  try {
    output += R"(,"placement":{"kind":)";
    if (!append_json_string(output, surface_placement_name(placement.kind))) {
      return false;
    }
    if (placement.kind == SurfacePlacementKind::dock_left ||
        placement.kind == SurfacePlacementKind::dock_right) {
      output += R"(,"size":)" + std::to_string(placement.columns) + "}";
    } else if (placement.kind == SurfacePlacementKind::dock_top ||
               placement.kind == SurfacePlacementKind::dock_bottom) {
      output += R"(,"size":)" + std::to_string(placement.rows) + "}";
    } else {
      output += R"(,"column":)" + std::to_string(placement.column) + R"(,"row":)" +
                std::to_string(placement.row) + R"(,"columns":)" +
                std::to_string(placement.columns) + R"(,"rows":)" + std::to_string(placement.rows) +
                "}";
    }
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto append_string_field(std::string& output, const std::string_view field,
                                       const std::string_view value) -> bool {
  try {
    output += ",\"";
    output += field;
    output += "\":";
    return append_json_string(output, value);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto append_string_array(std::string& output, const std::string_view field,
                                       const std::span<const std::string> values) -> bool {
  if (values.empty()) {
    return true;
  }
  try {
    output += ",\"";
    output += field;
    output += "\":[";
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index > 0) {
        output += ",";
      }
      if (!append_json_string(output, values.subspan(index, 1).front())) {
        return false;
      }
    }
    output += "]";
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto direction_name(const Direction direction) noexcept -> std::string_view {
  switch (direction) {
  case Direction::left:
    return "left";
  case Direction::right:
    return "right";
  case Direction::up:
    return "up";
  case Direction::down:
    return "down";
  case Direction::none:
    return {};
  }
  return {};
}

[[nodiscard]] constexpr auto focus_name(const FocusPolicy focus) noexcept -> std::string_view {
  return focus == FocusPolicy::preserve ? std::string_view{"preserve"}
                                        : std::string_view{"created"};
}

[[nodiscard]] constexpr auto capture_source_name(const CaptureSource source) noexcept
    -> std::string_view {
  switch (source) {
  case CaptureSource::visible:
    return "visible";
  case CaptureSource::recent:
    return "recent";
  case CaptureSource::last_command:
    return "last-command";
  }
  return {};
}

auto append_capture(std::string& output, const CaptureSource source, const CaptureFormat format,
                    const CaptureWrap wrap, const std::uint64_t terminal_generation,
                    const bool truncated, const std::string_view text) -> bool {
  try {
    output += R"({"source":)";
    if (!append_json_string(output, capture_source_name(source))) {
      return false;
    }
    output += R"(,"format":)";
    if (!append_json_string(output, format == CaptureFormat::plain ? "plain" : "ansi")) {
      return false;
    }
    output += R"(,"wrap":)";
    if (!append_json_string(output, wrap == CaptureWrap::logical ? "logical" : "rendered")) {
      return false;
    }
    output += R"(,"terminal_generation":)" + std::to_string(terminal_generation) +
              R"(,"truncated":)" + (truncated ? "true" : "false") + R"(,"text":)";
    return append_json_string(output, text) && (output += "}", true);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] constexpr auto input_event_kind_name(const InputEventKind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case InputEventKind::text:
    return "text";
  case InputEventKind::paste:
    return "paste";
  case InputEventKind::key:
    return "key";
  }
  return {};
}

// Branches mirror the closed text, paste, and logical-key event schemas.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto append_input_events(std::string& output,
                                       const std::span<const InputEvent> events) -> bool {
  try {
    output += R"(,"events":[)";
    for (std::size_t index = 0; index < events.size(); ++index) {
      if (index > 0) {
        output += ',';
      }
      const auto& event = events.subspan(index, 1).front();
      output += R"({"kind":)";
      if (!append_json_string(output, input_event_kind_name(event.kind))) {
        return false;
      }
      if (event.kind != InputEventKind::key) {
        if (!append_string_field(output, "text", event.text)) {
          return false;
        }
      } else {
        if (!append_string_field(output, "key", input_key_name(event.key))) {
          return false;
        }
        if (event.modifiers != 0) {
          output += R"(,"modifiers":[)";
          bool separator = false;
          for (const auto [bit, name] :
               std::array{std::pair{input_modifier_shift, std::string_view{"shift"}},
                          std::pair{input_modifier_control, std::string_view{"control"}},
                          std::pair{input_modifier_alt, std::string_view{"alt"}},
                          std::pair{input_modifier_super, std::string_view{"super"}}}) {
            if ((event.modifiers & bit) == 0) {
              continue;
            }
            if (separator) {
              output += ',';
            }
            separator = true;
            if (!append_json_string(output, name)) {
              return false;
            }
          }
          output += ']';
        }
        if (event.phase != InputKeyPhase::press) {
          if (!append_string_field(output, "phase",
                                   event.phase == InputKeyPhase::repeat
                                       ? std::string_view{"repeat"}
                                       : std::string_view{"release"})) {
            return false;
          }
        }
      }
      output += '}';
    }
    output += ']';
    return true;
  } catch (...) {
    return false;
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto encode_command(const Command& command) -> std::optional<std::string> {
  try {
    std::string output = R"({"command":)";
    if (!append_json_string(output, command_name(command.kind))) {
      return std::nullopt;
    }
    const bool surface_command = command.kind == CommandKind::surface_create ||
                                 command.kind == CommandKind::surface_configure ||
                                 command.kind == CommandKind::surface_focus ||
                                 command.kind == CommandKind::surface_close;
    const bool has_session = command.kind != CommandKind::daemon_inspect &&
                             command.kind != CommandKind::config_reload &&
                             command.kind != CommandKind::session_list &&
                             command.kind != CommandKind::session_start && !surface_command;
    if (has_session && !append_selector(output, "session", command.session)) {
      return std::nullopt;
    }
    if (command.expected_session_revision.has_value()) {
      if (command.kind == CommandKind::pane_wait) {
        return std::nullopt;
      }
      output += R"(,"if_session_revision":)" + std::to_string(*command.expected_session_revision);
    }
    if (command.kind == CommandKind::attachment_switch) {
      if (!command.connection.is_valid() ||
          !append_string_field(output, "connection",
                               std::to_string(command.connection.slot()) + ":" +
                                   std::to_string(command.connection.generation()))) {
        return std::nullopt;
      }
    }
    if (command.kind == CommandKind::session_start && !command.name.empty() &&
        !append_string_field(output, "name", command.name)) {
      return std::nullopt;
    }
    if (command.kind == CommandKind::session_rename &&
        !append_string_field(output, "name", command.name)) {
      return std::nullopt;
    }
    if (command.kind == CommandKind::tab_inspect || command.kind == CommandKind::tab_select ||
        command.kind == CommandKind::tab_move || command.kind == CommandKind::tab_rename ||
        command.kind == CommandKind::tab_kill) {
      if (!append_selector(output, "tab", command.tab)) {
        return std::nullopt;
      }
    }
    if (command.kind == CommandKind::pane_inspect || command.kind == CommandKind::pane_split ||
        command.kind == CommandKind::pane_focus || command.kind == CommandKind::pane_swap ||
        command.kind == CommandKind::pane_resize || command.kind == CommandKind::pane_zoom ||
        command.kind == CommandKind::pane_send || command.kind == CommandKind::pane_input ||
        command.kind == CommandKind::pane_paste_image ||
        command.kind == CommandKind::pane_capture || command.kind == CommandKind::pane_wait ||
        command.kind == CommandKind::pane_kill) {
      if (!append_selector(output, "pane", command.pane)) {
        return std::nullopt;
      }
    }
    if (command.kind == CommandKind::pane_swap &&
        !append_selector(output, "other", command.other)) {
      return std::nullopt;
    }
    if (command.kind == CommandKind::session_start || command.kind == CommandKind::tab_new ||
        command.kind == CommandKind::pane_split) {
      if (!command.working_directory.empty() &&
          !append_string_field(output, "cwd", command.working_directory)) {
        return std::nullopt;
      }
      if (command.hold) {
        output += R"(,"hold":true)";
      }
      if (!append_string_array(output, "argv", command.arguments)) {
        return std::nullopt;
      }
    }
    if (command.kind == CommandKind::session_start && command.environment_set) {
      if (command.environment.empty()) {
        output += R"(,"environment":[])";
      } else if (!append_string_array(output, "environment", command.environment)) {
        return std::nullopt;
      }
    }
    if (command.kind == CommandKind::tab_new && !command.title.empty() &&
        !append_string_field(output, "title", command.title)) {
      return std::nullopt;
    }
    if ((command.kind == CommandKind::tab_new || command.kind == CommandKind::pane_split) &&
        command.focus != FocusPolicy::created &&
        !append_string_field(output, "focus", focus_name(command.focus))) {
      return std::nullopt;
    }
    if (command.kind == CommandKind::tab_move) {
      output += R"(,"to_position":)" + std::to_string(command.to_position);
    }
    if (command.kind == CommandKind::tab_rename &&
        !append_string_field(output, "title", command.title)) {
      return std::nullopt;
    }
    if (command.kind == CommandKind::pane_split || command.kind == CommandKind::pane_resize) {
      if (!append_string_field(output, "direction", direction_name(command.direction))) {
        return std::nullopt;
      }
    }
    if (command.kind == CommandKind::pane_resize) {
      output += R"(,"amount":)" + std::to_string(command.amount);
    }
    if (command.kind == CommandKind::pane_zoom) {
      output += command.enabled ? R"(,"enabled":true)" : R"(,"enabled":false)";
    }
    if (command.kind == CommandKind::pane_send &&
        !append_string_field(output, "text", command.text)) {
      return std::nullopt;
    }
    if (command.kind == CommandKind::pane_input &&
        !append_input_events(output, command.input_events)) {
      return std::nullopt;
    }
    if (command.kind == CommandKind::pane_capture) {
      if (command.lines > 0) {
        output += R"(,"lines":)" + std::to_string(command.lines);
      }
      if (command.capture_source != CaptureSource::visible &&
          !append_string_field(output, "source", capture_source_name(command.capture_source))) {
        return std::nullopt;
      }
      if (command.capture_format == CaptureFormat::ansi &&
          !append_string_field(output, "format", "ansi")) {
        return std::nullopt;
      }
      if (command.capture_wrap == CaptureWrap::logical &&
          !append_string_field(output, "wrap", "logical")) {
        return std::nullopt;
      }
    }
    if (command.kind == CommandKind::pane_wait) {
      if (command.wait_condition == WaitCondition::exit_code) {
        output += R"(,"exit_code":)" + std::to_string(command.wait_value);
      } else if (command.wait_condition == WaitCondition::signal) {
        output += R"(,"signal":)" + std::to_string(command.wait_value);
      } else if (command.wait_condition == WaitCondition::contains) {
        if (!append_string_field(output, "contains", command.contains)) {
          return std::nullopt;
        }
      } else if (command.wait_condition == WaitCondition::prompt) {
        output += R"(,"until_prompt":true)";
      } else if (command.wait_condition == WaitCondition::command) {
        output += R"(,"until_command":true)";
      }
      if (command.after_terminal_generation > 0) {
        output += R"(,"after_generation":)" + std::to_string(command.after_terminal_generation);
      }
      if (command.after_commands.has_value()) {
        output += R"(,"after_commands":)" + std::to_string(*command.after_commands);
      }
      if (command.wait_timeout_milliseconds != wait_timeout_default_milliseconds) {
        output += R"(,"timeout_ms":)" + std::to_string(command.wait_timeout_milliseconds);
      }
    }
    if ((command.kind == CommandKind::surface_configure ||
         command.kind == CommandKind::surface_focus ||
         command.kind == CommandKind::surface_close) &&
        !append_selector(output, "surface", command.surface)) {
      return std::nullopt;
    }
    if ((command.kind == CommandKind::surface_create ||
         command.kind == CommandKind::surface_configure) &&
        !append_surface_placement(output, command.surface_placement)) {
      return std::nullopt;
    }
    if (command.kind == CommandKind::surface_create) {
      if (!command.focusable) {
        output += R"(,"focusable":false)";
      }
      if (!command.opaque) {
        output += R"(,"opaque":false)";
      }
    }
    if (command.kind == CommandKind::surface_configure &&
        command.configured_focusable.has_value()) {
      output += *command.configured_focusable ? R"(,"focusable":true)" : R"(,"focusable":false)";
    }
    output += "}";
    return output;
  } catch (...) {
    return std::nullopt;
  }
}

// A Proc Command is closed and kind-specific. Admission decodes it once before execution.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto decode_command(const JsonValue& document) -> CommandDecodeResult {
  if (document.kind != JsonKind::object) {
    return failure("invalid_document");
  }
  const auto name = json_string(document, "command");
  if (!name.has_value()) {
    return failure("missing_or_invalid_field", "command");
  }
  Command command;
  if (const auto* const revision = json_member(document, "if_session_revision");
      revision != nullptr) {
    const auto value = json_unsigned(document, "if_session_revision");
    if (!value.has_value() || *value == 0) {
      return failure("invalid_field", "if_session_revision");
    }
    command.expected_session_revision = value;
  }
  const auto reject_unknown = [&](const std::initializer_list<std::string_view> fields)
      -> std::optional<CommandDecodeResult> {
    const auto field = unknown_field(document, fields);
    return field.has_value() ? std::optional{failure("unknown_field", *field)} : std::nullopt;
  };

  if (*name == "daemon.inspect" || *name == "config.reload") {
    if (auto rejected = reject_unknown({"command"}); rejected.has_value()) {
      return *rejected;
    }
    command.kind =
        *name == "config.reload" ? CommandKind::config_reload : CommandKind::daemon_inspect;
  } else if (*name == "attachment.switch") {
    if (auto rejected = reject_unknown({"command", "connection", "session", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::attachment_switch;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto text = json_string(document, "connection");
    const auto connection = text.has_value() ? parse_id<ConnectionId>(*text) : std::nullopt;
    if (!connection.has_value()) {
      return failure("invalid_field", "connection");
    }
    command.connection = *connection;
  } else if (*name == "session.list") {
    if (auto rejected = reject_unknown({"command"}); rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::session_list;
  } else if (*name == "session.inspect") {
    if (auto rejected = reject_unknown({"command", "session", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::session_inspect;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
  } else if (*name == "session.start") {
    if (auto rejected = reject_unknown({"command", "name", "cwd", "hold", "argv", "environment"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::session_start;
    if (const auto* const candidate = json_member(document, "name"); candidate != nullptr) {
      if (candidate->kind != JsonKind::string ||
          !SessionNameValue::create(candidate->string).has_value()) {
        return failure("invalid_field", "name");
      }
      command.name = candidate->string;
    }
    if (const auto field = decode_launch(document, command, false); field.has_value()) {
      return failure("invalid_field", *field);
    }
    if (!decode_environment(document, command)) {
      return failure("invalid_field", "environment");
    }
  } else if (*name == "session.rename") {
    if (auto rejected = reject_unknown({"command", "session", "name", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::session_rename;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto candidate = json_string(document, "name");
    if (!candidate.has_value() || !SessionNameValue::create(*candidate).has_value()) {
      return failure("invalid_field", "name");
    }
    command.name = *candidate;
  } else if (*name == "session.kill") {
    if (auto rejected = reject_unknown({"command", "session", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::session_kill;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
  } else if (*name == "tab.list" || *name == "pane.list") {
    if (auto rejected = reject_unknown({"command", "session", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = *name == "tab.list" ? CommandKind::tab_list : CommandKind::pane_list;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
  } else if (*name == "tab.inspect") {
    if (auto rejected = reject_unknown({"command", "session", "tab", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::tab_inspect;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_tab(document, command); invalid.has_value()) {
      return *invalid;
    }
  } else if (*name == "pane.inspect") {
    if (auto rejected = reject_unknown({"command", "session", "pane", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_inspect;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
  } else if (*name == "tab.new") {
    if (auto rejected = reject_unknown(
            {"command", "session", "title", "cwd", "hold", "argv", "focus", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::tab_new;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (const auto field = decode_launch(document, command, true); field.has_value()) {
      return failure("invalid_field", *field);
    }
    if (!decode_focus_policy(document, command)) {
      return failure("invalid_field", "focus");
    }
  } else if (*name == "tab.select" || *name == "tab.kill") {
    if (auto rejected = reject_unknown({"command", "session", "tab", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = *name == "tab.select" ? CommandKind::tab_select : CommandKind::tab_kill;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_tab(document, command); invalid.has_value()) {
      return *invalid;
    }
  } else if (*name == "tab.move") {
    if (auto rejected =
            reject_unknown({"command", "session", "tab", "to_position", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::tab_move;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_tab(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto destination = json_unsigned(document, "to_position");
    if (!destination.has_value() || *destination == 0 || *destination > command_tab_slots_max) {
      return failure("invalid_field", "to_position");
    }
    command.to_position = static_cast<std::uint16_t>(*destination);
  } else if (*name == "tab.rename") {
    if (auto rejected =
            reject_unknown({"command", "session", "tab", "title", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::tab_rename;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_tab(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto title = json_string(document, "title").value_or(std::string_view{});
    if (!TabTitleValue::create(title).has_value()) {
      return failure("invalid_field", "title");
    }
    command.title = title;
  } else if (*name == "pane.split") {
    if (auto rejected = reject_unknown({"command", "session", "pane", "direction", "cwd", "hold",
                                        "argv", "focus", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_split;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto direction = json_string(document, "direction");
    if (direction == std::optional<std::string_view>{"right"}) {
      command.direction = Direction::right;
    } else if (direction == std::optional<std::string_view>{"down"}) {
      command.direction = Direction::down;
    } else {
      return failure("invalid_field", "direction");
    }
    if (const auto field = decode_launch(document, command, false); field.has_value()) {
      return failure("invalid_field", *field);
    }
    if (!decode_focus_policy(document, command)) {
      return failure("invalid_field", "focus");
    }
  } else if (*name == "pane.focus" || *name == "pane.kill" || *name == "pane.paste-image") {
    if (auto rejected = reject_unknown({"command", "session", "pane", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = *name == "pane.focus" ? CommandKind::pane_focus : CommandKind::pane_kill;
    if (*name == "pane.paste-image") {
      command.kind = CommandKind::pane_paste_image;
    }
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
  } else if (*name == "pane.swap") {
    if (auto rejected =
            reject_unknown({"command", "session", "pane", "other", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_swap;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command, "other"); invalid.has_value()) {
      return *invalid;
    }
  } else if (*name == "pane.resize") {
    if (auto rejected = reject_unknown(
            {"command", "session", "pane", "direction", "amount", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_resize;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto direction = json_string(document, "direction");
    if (direction == std::optional<std::string_view>{"left"}) {
      command.direction = Direction::left;
    } else if (direction == std::optional<std::string_view>{"right"}) {
      command.direction = Direction::right;
    } else if (direction == std::optional<std::string_view>{"up"}) {
      command.direction = Direction::up;
    } else if (direction == std::optional<std::string_view>{"down"}) {
      command.direction = Direction::down;
    } else {
      return failure("invalid_field", "direction");
    }
    const auto amount = json_unsigned(document, "amount").value_or(1);
    if (amount == 0 || amount > command_resize_amount_max) {
      return failure("invalid_field", "amount");
    }
    command.amount = static_cast<std::uint16_t>(amount);
  } else if (*name == "pane.zoom") {
    if (auto rejected =
            reject_unknown({"command", "session", "pane", "enabled", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_zoom;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto enabled = json_boolean(document, "enabled");
    if (!enabled.has_value()) {
      return failure("invalid_field", "enabled");
    }
    command.enabled = *enabled;
  } else if (*name == "pane.send") {
    if (auto rejected =
            reject_unknown({"command", "session", "pane", "text", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_send;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto text = json_string(document, "text");
    if (!text.has_value() || text->empty() || text->size() > limits::environment_bytes_max - 8U) {
      return failure("invalid_field", "text");
    }
    command.text = *text;
  } else if (*name == "pane.input") {
    if (auto rejected =
            reject_unknown({"command", "session", "pane", "events", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_input;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (!decode_input_events(document, command)) {
      return failure("invalid_field", "events");
    }
  } else if (*name == "pane.capture") {
    if (auto rejected = reject_unknown({"command", "session", "pane", "lines", "source", "format",
                                        "wrap", "if_session_revision"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_capture;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
    const auto* const lines_value = json_member(document, "lines");
    if (lines_value != nullptr) {
      const auto lines = json_unsigned(document, "lines");
      if (!lines.has_value() || *lines == 0 || *lines > std::numeric_limits<std::uint16_t>::max()) {
        return failure("invalid_field", "lines");
      }
      command.lines = static_cast<std::uint16_t>(*lines);
    }
    const auto source_value = json_string(document, "source");
    if (json_member(document, "source") != nullptr && !source_value.has_value()) {
      return failure("invalid_field", "source");
    }
    const auto source = source_value.value_or("visible");
    if (source == "visible") {
      command.capture_source = CaptureSource::visible;
    } else if (source == "recent") {
      command.capture_source = CaptureSource::recent;
    } else if (source == "last-command") {
      command.capture_source = CaptureSource::last_command;
    } else {
      return failure("invalid_field", "source");
    }
    const auto format_value = json_string(document, "format");
    if (json_member(document, "format") != nullptr && !format_value.has_value()) {
      return failure("invalid_field", "format");
    }
    const auto format = format_value.value_or("plain");
    if (format == "plain") {
      command.capture_format = CaptureFormat::plain;
    } else if (format == "ansi") {
      command.capture_format = CaptureFormat::ansi;
    } else {
      return failure("invalid_field", "format");
    }
    const auto wrap_value = json_string(document, "wrap");
    if (json_member(document, "wrap") != nullptr && !wrap_value.has_value()) {
      return failure("invalid_field", "wrap");
    }
    const auto wrap = wrap_value.value_or("rendered");
    if (wrap == "rendered") {
      command.capture_wrap = CaptureWrap::rendered;
    } else if (wrap == "logical") {
      command.capture_wrap = CaptureWrap::logical;
    } else {
      return failure("invalid_field", "wrap");
    }
    if (command.capture_source == CaptureSource::last_command && command.lines > 0) {
      return failure("invalid_field", "lines");
    }
  } else if (*name == "pane.wait") {
    if (auto rejected = reject_unknown({"command", "session", "pane", "exit_code", "signal",
                                        "contains", "until_prompt", "until_command",
                                        "after_generation", "after_commands", "timeout_ms"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::pane_wait;
    if (auto invalid = require_session(document, command); invalid.has_value()) {
      return *invalid;
    }
    if (auto invalid = require_pane(document, command); invalid.has_value()) {
      return *invalid;
    }
    std::size_t condition_count = 0;
    if (const auto* const exit_code = json_member(document, "exit_code"); exit_code != nullptr) {
      const auto value = json_unsigned(document, "exit_code");
      if (!value.has_value() || *value > 255U) {
        return failure("invalid_field", "exit_code");
      }
      command.wait_condition = WaitCondition::exit_code;
      command.wait_value = static_cast<std::uint32_t>(*value);
      ++condition_count;
    }
    if (const auto* const signal = json_member(document, "signal"); signal != nullptr) {
      const auto value = json_unsigned(document, "signal");
      if (!value.has_value() || *value == 0 || *value > 127U) {
        return failure("invalid_field", "signal");
      }
      command.wait_condition = WaitCondition::signal;
      command.wait_value = static_cast<std::uint32_t>(*value);
      ++condition_count;
    }
    if (const auto* const contains = json_member(document, "contains"); contains != nullptr) {
      const auto value = json_string(document, "contains");
      if (!value.has_value() || value->empty() || value->contains('\0') ||
          value->size() > std::numeric_limits<std::uint16_t>::max()) {
        return failure("invalid_field", "contains");
      }
      command.wait_condition = WaitCondition::contains;
      command.contains = *value;
      ++condition_count;
    }
    if (const auto* const prompt = json_member(document, "until_prompt"); prompt != nullptr) {
      const auto value = json_boolean(document, "until_prompt");
      if (value != std::optional{true}) {
        return failure("invalid_field", "until_prompt");
      }
      command.wait_condition = WaitCondition::prompt;
      ++condition_count;
    }
    if (const auto* const until = json_member(document, "until_command"); until != nullptr) {
      if (json_boolean(document, "until_command") != std::optional{true}) {
        return failure("invalid_field", "until_command");
      }
      command.wait_condition = WaitCondition::command;
      ++condition_count;
    }
    if (condition_count > 1U) {
      return failure("conflicting_fields", "condition");
    }
    if (const auto* const generation = json_member(document, "after_generation");
        generation != nullptr) {
      const auto value = json_unsigned(document, "after_generation");
      if (!value.has_value() || (command.wait_condition != WaitCondition::contains &&
                                 command.wait_condition != WaitCondition::prompt)) {
        return failure("invalid_field", "after_generation");
      }
      command.after_terminal_generation = *value;
    }
    if (const auto* const commands = json_member(document, "after_commands"); commands != nullptr) {
      const auto value = json_unsigned(document, "after_commands");
      if (!value.has_value() || command.wait_condition != WaitCondition::command) {
        return failure("invalid_field", "after_commands");
      }
      command.after_commands = value;
    }
    if (const auto* const timeout = json_member(document, "timeout_ms"); timeout != nullptr) {
      const auto value = json_unsigned(document, "timeout_ms");
      if (!value.has_value() || *value == 0 || *value > wait_timeout_max_milliseconds) {
        return failure("invalid_field", "timeout_ms");
      }
      command.wait_timeout_milliseconds = static_cast<std::uint32_t>(*value);
    }
  } else if (*name == "surface.create") {
    if (auto rejected = reject_unknown({"command", "placement", "focusable", "opaque"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::surface_create;
    const auto placement = decode_surface_placement(document);
    if (!placement.has_value()) {
      return failure("invalid_field", "placement");
    }
    command.surface_placement = *placement;
    if (const auto* const focusable = json_member(document, "focusable"); focusable != nullptr) {
      const auto value = json_boolean(document, "focusable");
      if (!value.has_value()) {
        return failure("invalid_field", "focusable");
      }
      command.focusable = *value;
    }
    if (const auto* const opaque = json_member(document, "opaque"); opaque != nullptr) {
      const auto value = json_boolean(document, "opaque");
      if (!value.has_value()) {
        return failure("invalid_field", "opaque");
      }
      command.opaque = *value;
    }
  } else if (*name == "surface.configure") {
    if (auto rejected = reject_unknown({"command", "surface", "placement", "focusable"});
        rejected.has_value()) {
      return *rejected;
    }
    command.kind = CommandKind::surface_configure;
    const auto surface = decode_surface_selector(document);
    const auto placement = decode_surface_placement(document);
    if (!surface.has_value()) {
      return failure("invalid_selector", "surface");
    }
    if (!placement.has_value()) {
      return failure("invalid_field", "placement");
    }
    command.surface = *surface;
    command.surface_placement = *placement;
    if (json_member(document, "focusable") != nullptr) {
      command.configured_focusable = json_boolean(document, "focusable");
      if (!command.configured_focusable.has_value()) {
        return failure("invalid_field", "focusable");
      }
    }
  } else if (*name == "surface.focus" || *name == "surface.close") {
    if (auto rejected = reject_unknown({"command", "surface"}); rejected.has_value()) {
      return *rejected;
    }
    command.kind =
        *name == "surface.focus" ? CommandKind::surface_focus : CommandKind::surface_close;
    const auto surface = decode_surface_selector(document);
    if (!surface.has_value()) {
      return failure("invalid_selector", "surface");
    }
    command.surface = *surface;
  } else {
    return failure("unknown_command", "command");
  }
  return {.command = std::move(command), .error = {}};
}

// Legacy one-Pane and bounded multi-Pane selectors share one closed subscription grammar.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto decode_event_subscription(const JsonValue& document) -> EventSubscriptionDecodeResult {
  if (document.kind != JsonKind::object) {
    return {.subscription = std::nullopt, .error = {.reason = "invalid_document", .field = {}}};
  }
  if (json_string(document, "schema") != std::optional<std::string_view>{events_schema}) {
    return {.subscription = std::nullopt, .error = {.reason = "invalid_schema", .field = "schema"}};
  }
  if (const auto field = unknown_field(
          document, {"schema", "session", "pane", "panes", "screen", "presentation", "signals"});
      field.has_value()) {
    return {.subscription = std::nullopt, .error = {.reason = "unknown_field", .field = *field}};
  }
  EventSubscription result;
  if (json_member(document, "session") != nullptr) {
    result.session = decode_session_selector(document);
    if (!result.session.has_value()) {
      return {.subscription = std::nullopt,
              .error = {.reason = "invalid_selector", .field = "session"}};
    }
  }
  const bool has_pane = json_member(document, "pane") != nullptr;
  const auto* const panes = json_member(document, "panes");
  if (has_pane && panes != nullptr) {
    return {.subscription = std::nullopt,
            .error = {.reason = "conflicting_fields", .field = "panes"}};
  }
  if (has_pane) {
    const auto pane = decode_pane_selector(document, "pane");
    if (!pane.has_value() || !result.session.has_value()) {
      return {.subscription = std::nullopt,
              .error = {.reason = "invalid_selector", .field = "pane"}};
    }
    result.panes.push_back(*pane);
  } else if (panes != nullptr) {
    if (panes->kind != JsonKind::array || panes->array.empty() ||
        panes->array.size() > event_panes_max || !result.session.has_value()) {
      return {.subscription = std::nullopt,
              .error = {.reason = "invalid_selector", .field = "panes"}};
    }
    for (const auto& candidate : panes->array) {
      JsonValue wrapper{.kind = JsonKind::object,
                        .boolean = false,
                        .number = 0,
                        .string = {},
                        .array = {},
                        .object = {}};
      wrapper.object.push_back({.key = "pane", .value = candidate});
      const auto pane = decode_pane_selector(wrapper, "pane");
      if (!pane.has_value() || std::ranges::any_of(result.panes, [&](const PaneSelector& existing) {
            return existing.id == pane->id;
          })) {
        return {.subscription = std::nullopt,
                .error = {.reason = "invalid_selector", .field = "panes"}};
      }
      result.panes.push_back(*pane);
    }
  }
  if (const auto* const screen = json_member(document, "screen"); screen != nullptr) {
    if (screen->kind != JsonKind::boolean) {
      return {.subscription = std::nullopt,
              .error = {.reason = "invalid_field", .field = "screen"}};
    }
    result.screen = screen->boolean;
  }
  if (result.screen && result.panes.empty()) {
    return {.subscription = std::nullopt, .error = {.reason = "invalid_selector", .field = "pane"}};
  }
  if (const auto* const presentation = json_member(document, "presentation");
      presentation != nullptr) {
    if (presentation->kind != JsonKind::boolean || !result.session.has_value()) {
      return {.subscription = std::nullopt,
              .error = {.reason = "invalid_field", .field = "presentation"}};
    }
    result.presentation = presentation->boolean;
  }
  if (const auto* const signals = json_member(document, "signals"); signals != nullptr) {
    if (signals->kind != JsonKind::boolean) {
      return {.subscription = std::nullopt,
              .error = {.reason = "invalid_field", .field = "signals"}};
    }
    result.signals = signals->boolean;
  }
  return {.subscription = std::move(result), .error = {}};
}

} // namespace lemma::api
