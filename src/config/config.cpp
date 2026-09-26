#include "config/config.hpp"

#include "api/json.hpp"
#include "input/input_router.hpp"
#include "lemma/limits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lemma::config {
namespace {

using input::CommandContextDisposition;
using input::ConfiguredInputContext;
using input::InputChord;
using input::InputCommand;
using input::PhysicalKey;

[[nodiscard]] auto known_members(const api::JsonValue& object,
                                 const std::initializer_list<std::string_view> allowed) noexcept
    -> bool {
  if (object.kind != api::JsonKind::object) {
    return false;
  }
  return std::ranges::all_of(object.object, [&allowed](const api::JsonMember& member) {
    return std::ranges::find(allowed, member.key) != allowed.end();
  });
}

[[nodiscard]] auto append_chord(std::string& output, const InputChord chord) -> bool {
  try {
    output += R"({"kind":)";
    if (!api::append_json_string(output, chord.kind == input::ChordKind::byte ? "byte" : "key",
                                 configuration_document_bytes_max)) {
      return false;
    }
    output += R"(,"code":)";
    output += std::to_string(chord.code);
    output += R"(,"modifiers":)";
    output += std::to_string(chord.modifiers);
    output += "}";
    return output.size() <= configuration_document_bytes_max;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto decode_chord(const api::JsonValue& value) noexcept -> std::optional<InputChord> {
  if (!known_members(value, {"kind", "code", "modifiers"}) || value.object.size() != 3U) {
    return std::nullopt;
  }
  const auto kind = api::json_string(value, "kind");
  const auto code = api::json_unsigned(value, "code");
  const auto modifiers = api::json_unsigned(value, "modifiers");
  if (!kind.has_value() || !code.has_value() || !modifiers.has_value() ||
      *code > std::numeric_limits<std::uint16_t>::max() || *modifiers > input::key_modifiers_all) {
    return std::nullopt;
  }
  if (*kind == "byte" && *code <= 0xFFU) {
    return InputChord::byte(static_cast<std::uint8_t>(*code),
                            static_cast<std::uint16_t>(*modifiers));
  }
  if (*kind == "key" && *code < static_cast<std::uint64_t>(PhysicalKey::count)) {
    return InputChord::key(static_cast<PhysicalKey>(*code), static_cast<std::uint16_t>(*modifiers));
  }
  return std::nullopt;
}

[[nodiscard]] constexpr auto disposition_name(const CommandContextDisposition disposition) noexcept
    -> std::string_view {
  return disposition == CommandContextDisposition::base ? "base" : "retain";
}

[[nodiscard]] auto parse_disposition(const std::string_view value) noexcept
    -> std::optional<CommandContextDisposition> {
  if (value == "retain") {
    return CommandContextDisposition::retain;
  }
  if (value == "base") {
    return CommandContextDisposition::base;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr auto preset_name(const input::InputMapPreset preset) noexcept
    -> std::string_view {
  return preset == input::InputMapPreset::none ? "none" : "default";
}

[[nodiscard]] auto parse_preset(const std::string_view value) noexcept
    -> std::optional<input::InputMapPreset> {
  if (value == "default") {
    return input::InputMapPreset::defaults;
  }
  if (value == "none") {
    return input::InputMapPreset::none;
  }
  return std::nullopt;
}

// Fixed-capacity configuration indexes are validated before access.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
[[nodiscard]] auto runtime_options_valid(const Configuration& configuration) noexcept -> bool {
  if (configuration.terminal.scrollback_lines.has_value() &&
      *configuration.terminal.scrollback_lines > limits::terminal_scrollback_lines_hard_max) {
    return false;
  }
  if ((!configuration.launch.default_cwd.empty() &&
       configuration.launch.default_cwd.front() != '/') ||
      configuration.launch.default_cwd.size() > limits::working_directory_bytes_max ||
      configuration.launch.default_cwd.contains('\0') ||
      configuration.launch.default_program.size() > default_program_arguments_max ||
      (!configuration.history.file.empty() && configuration.history.file.front() != '/') ||
      configuration.history.file.size() > configuration_path_bytes_max ||
      configuration.history.file.contains('\0')) {
    return false;
  }
  if (configuration.extensions.size() > extensions_max) {
    return false;
  }
  for (std::size_t index = 0; index < configuration.extensions.size(); ++index) {
    const auto& extension = configuration.extensions.at(index);
    if (extension.name.empty() || extension.name.size() > 64 || extension.name.contains('\0') ||
        extension.argv.empty() || extension.argv.size() > default_program_arguments_max ||
        extension.argv.front().empty()) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (configuration.extensions.at(previous).name == extension.name) {
        return false;
      }
    }
    std::size_t bytes = 0;
    for (const auto& argument : extension.argv) {
      if (argument.contains('\0') || argument.size() + 1 > default_program_bytes_max - bytes) {
        return false;
      }
      bytes += argument.size() + 1;
    }
  }
  std::size_t program_bytes = 0;
  for (std::size_t index = 0; index < configuration.launch.default_program.size(); ++index) {
    const auto& argument = configuration.launch.default_program.at(index);
    if ((index == 0U && argument.empty()) || argument.contains('\0') ||
        argument.size() + 1U > default_program_bytes_max - program_bytes) {
      return false;
    }
    program_bytes += argument.size() + 1U;
  }
  return true;
}

} // namespace

namespace {

constexpr std::array command_names{
    "detach",
    "split_left_right",
    "split_top_bottom",
    "resize_left",
    "resize_right",
    "resize_up",
    "resize_down",
    "focus_left",
    "focus_right",
    "focus_up",
    "focus_down",
    "focus_next",
    "focus_previous",
    "close_pane",
    "toggle_zoom",
    "enter_copy_mode",
    "enter_copy_search_forward",
    "enter_copy_search_backward",
    "copy_selection",
    "create_tab",
    "next_tab",
    "previous_tab",
    "begin_rename_session",
    "begin_rename_tab",
    "begin_command_line",
    "show_messages",
    "message_view_leave",
    "message_view_previous",
    "message_view_next",
    "message_view_page_previous",
    "message_view_page_next",
    "message_view_history_start",
    "message_view_history_end",
    "move_tab_left",
    "move_tab_right",
    "swap_pane_left",
    "swap_pane_right",
    "swap_pane_up",
    "swap_pane_down",
    "close_tab",
    "select_tab_0",
    "select_tab_1",
    "select_tab_2",
    "select_tab_3",
    "select_tab_4",
    "select_tab_5",
    "select_tab_6",
    "select_tab_7",
    "select_tab_8",
    "select_tab_9",
    "copy_cancel_or_leave",
    "copy_leave",
    "copy_cancel_selection",
    "copy_move_left",
    "copy_move_down",
    "copy_move_up",
    "copy_move_right",
    "copy_word_left",
    "copy_word_right",
    "copy_word_end",
    "copy_line_start",
    "copy_line_first_nonblank",
    "copy_line_end",
    "copy_history_top",
    "copy_history_bottom",
    "copy_viewport_top",
    "copy_viewport_middle",
    "copy_viewport_bottom",
    "copy_half_page_up",
    "copy_half_page_down",
    "copy_page_up",
    "copy_page_down",
    "copy_visual_character",
    "copy_visual_line",
    "copy_visual_block",
    "copy_swap_endpoint",
    "copy_repeat_search",
    "copy_reverse_search",
    "copy_cancel_search",
    "copy_commit_search",
    "copy_query_backspace",
    "rename_cancel",
    "rename_commit",
    "rename_backspace",
    "rename_delete",
    "rename_cursor_left",
    "rename_cursor_right",
    "rename_cursor_home",
    "rename_cursor_end",
    "rename_clear",
    "rename_delete_word",
    "command_line_cancel",
    "command_line_commit",
    "command_line_complete",
    "command_line_history_previous",
    "command_line_history_next",
    "command_line_backspace",
    "command_line_delete",
    "command_line_cursor_left",
    "command_line_cursor_right",
    "command_line_cursor_home",
    "command_line_cursor_end",
    "command_line_clear",
    "command_line_delete_word",
};
static_assert(command_names.size() == static_cast<std::size_t>(InputCommand::count));

constexpr std::array context_names{"normal",       "prefix",      "resize",         "copy",
                                   "copy_go",      "copy_search", "copy_searching", "rename",
                                   "command_line", "messages"};
static_assert(context_names.size() == static_cast<std::size_t>(ConfiguredInputContext::count));

} // namespace

auto parse_context(const std::string_view value) noexcept -> std::optional<ConfiguredInputContext> {
  const auto* const found = std::ranges::find(context_names, value);
  return found == context_names.end()
             ? std::nullopt
             : std::optional{static_cast<ConfiguredInputContext>(
                   static_cast<std::size_t>(std::distance(context_names.begin(), found)))};
}

auto parse_command(const std::string_view value) noexcept -> std::optional<InputCommand> {
  const auto* const found = std::ranges::find(command_names, value);
  return found == command_names.end()
             ? std::nullopt
             : std::optional{static_cast<InputCommand>(
                   static_cast<std::size_t>(std::distance(command_names.begin(), found)))};
}

auto command_name(const InputCommand command) noexcept -> std::string_view {
  const auto index = static_cast<std::size_t>(command);
  return index < command_names.size() ? std::span(command_names).subspan(index, 1).front()
                                      : std::string_view{};
}

auto context_name(const ConfiguredInputContext context) noexcept -> std::string_view {
  const auto index = static_cast<std::size_t>(context);
  return index < context_names.size() ? std::span(context_names).subspan(index, 1).front()
                                      : std::string_view{};
}

auto error_name(const Error error) noexcept -> std::string_view {
  switch (error) {
  case Error::invalid_document:
    return "invalid_document";
  case Error::invalid_schema:
    return "invalid_schema";
  case Error::invalid_field:
    return "invalid_field";
  case Error::invalid_context:
    return "invalid_context";
  case Error::invalid_key:
    return "invalid_key";
  case Error::invalid_command:
    return "invalid_command";
  case Error::capacity:
    return "capacity";
  case Error::input_map:
    return "input_map";
  }
  return "invalid_document";
}

namespace {

[[nodiscard]] constexpr auto lifetime_name(const input::ContextLifetime lifetime) noexcept
    -> std::string_view {
  return lifetime == input::ContextLifetime::one_shot ? "one_shot" : "persistent";
}

[[nodiscard]] auto parse_lifetime(const std::string_view value) noexcept
    -> std::optional<input::ContextLifetime> {
  if (value == "persistent") {
    return input::ContextLifetime::persistent;
  }
  if (value == "one_shot") {
    return input::ContextLifetime::one_shot;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr auto unbound_name(const input::UnboundBehavior behavior) noexcept
    -> std::string_view {
  switch (behavior) {
  case input::UnboundBehavior::forward:
    return "forward";
  case input::UnboundBehavior::replay_deferred:
    return "replay";
  case input::UnboundBehavior::consume:
    return "consume";
  case input::UnboundBehavior::retry_base:
    return "retry";
  }
  return {};
}

[[nodiscard]] auto parse_unbound(const std::string_view value) noexcept
    -> std::optional<input::UnboundBehavior> {
  if (value == "forward") {
    return input::UnboundBehavior::forward;
  }
  if (value == "replay") {
    return input::UnboundBehavior::replay_deferred;
  }
  if (value == "consume") {
    return input::UnboundBehavior::consume;
  }
  if (value == "retry") {
    return input::UnboundBehavior::retry_base;
  }
  return std::nullopt;
}

[[nodiscard]] auto append_action_kind(std::string& output, const std::string_view kind) -> bool {
  output += R"({"kind":)";
  return api::append_json_string(output, kind, configuration_document_bytes_max);
}

[[nodiscard]] auto append_command_action(std::string& output,
                                         const input::ConfiguredBindingAction& action) -> bool {
  if (!append_action_kind(output, "command")) {
    return false;
  }
  output += R"(,"command":)";
  if (!api::append_json_string(output, command_name(action.command),
                               configuration_document_bytes_max)) {
    return false;
  }
  output += R"(,"disposition":)";
  return api::append_json_string(output, disposition_name(action.disposition),
                                 configuration_document_bytes_max);
}

[[nodiscard]] auto append_push_action(std::string& output,
                                      const input::ConfiguredBindingAction& action) -> bool {
  if (!append_action_kind(output, "push")) {
    return false;
  }
  output += R"(,"context":)";
  if (!api::append_json_string(output, context_name(action.target),
                               configuration_document_bytes_max)) {
    return false;
  }
  output += R"(,"defer":)";
  output += action.defer_chord ? "true" : "false";
  return true;
}

[[nodiscard]] auto append_send_action(std::string& output,
                                      const input::ConfiguredBindingAction& action) -> bool {
  if (!append_action_kind(output, "send")) {
    return false;
  }
  output += R"(,"key":)";
  return append_chord(output, InputChord::key(action.encoded_key, action.encoded_modifiers));
}

[[nodiscard]] auto append_binding_action(std::string& output,
                                         const input::ConfiguredBindingAction& action) -> bool {
  bool appended = false;
  switch (action.kind) {
  case input::ConfiguredBindingKind::command:
    appended = append_command_action(output, action);
    break;
  case input::ConfiguredBindingKind::hosted_command:
    appended = append_action_kind(output, "hosted");
    output += R"(,"index":)" + std::to_string(action.hosted_command) + R"(,"disposition":)";
    appended = appended && api::append_json_string(output, disposition_name(action.disposition),
                                                   configuration_document_bytes_max);
    break;
  case input::ConfiguredBindingKind::push_context:
    appended = append_push_action(output, action);
    break;
  case input::ConfiguredBindingKind::pop_context:
    appended = append_action_kind(output, "pop");
    break;
  case input::ConfiguredBindingKind::replay_deferred:
    appended = append_action_kind(output, "replay");
    break;
  case input::ConfiguredBindingKind::send_key:
    appended = append_send_action(output, action);
    break;
  }
  output += '}';
  return appended && output.size() <= configuration_document_bytes_max;
}

[[nodiscard]] auto decode_command_action(const api::JsonValue& value) noexcept
    -> std::optional<input::ConfiguredBindingAction> {
  if (!known_members(value, {"kind", "command", "disposition"}) || value.object.size() != 3U) {
    return std::nullopt;
  }
  const auto command_value = api::json_string(value, "command");
  const auto disposition_value = api::json_string(value, "disposition");
  const auto command = command_value.has_value() ? parse_command(*command_value) : std::nullopt;
  const auto disposition = disposition_value.has_value()
                               ? parse_disposition(*disposition_value)
                               : std::optional<CommandContextDisposition>{};
  if (!command.has_value() || !disposition.has_value()) {
    return std::nullopt;
  }
  return input::ConfiguredBindingAction{.kind = input::ConfiguredBindingKind::command,
                                        .command = *command,
                                        .disposition = *disposition};
}

[[nodiscard]] auto decode_push_action(const api::JsonValue& value) noexcept
    -> std::optional<input::ConfiguredBindingAction> {
  if (!known_members(value, {"kind", "context", "defer"}) || value.object.size() != 3U) {
    return std::nullopt;
  }
  const auto context_value = api::json_string(value, "context");
  const auto context = context_value.has_value() ? parse_context(*context_value) : std::nullopt;
  const auto defer = api::json_boolean(value, "defer");
  if (!context.has_value() || !defer.has_value()) {
    return std::nullopt;
  }
  return input::ConfiguredBindingAction{.kind = input::ConfiguredBindingKind::push_context,
                                        .target = *context,
                                        .defer_chord = *defer};
}

[[nodiscard]] auto decode_send_action(const api::JsonValue& value) noexcept
    -> std::optional<input::ConfiguredBindingAction> {
  if (!known_members(value, {"kind", "key"}) || value.object.size() != 2U) {
    return std::nullopt;
  }
  const auto* const encoded_value = api::json_member(value, "key");
  const auto encoded =
      encoded_value == nullptr ? std::optional<InputChord>{} : decode_chord(*encoded_value);
  if (!encoded.has_value() || encoded->kind != input::ChordKind::key ||
      encoded->code >= static_cast<std::uint16_t>(input::PhysicalKey::count)) {
    return std::nullopt;
  }
  return input::ConfiguredBindingAction{.kind = input::ConfiguredBindingKind::send_key,
                                        .encoded_key =
                                            static_cast<input::PhysicalKey>(encoded->code),
                                        .encoded_modifiers = encoded->modifiers};
}

[[nodiscard]] auto decode_binding_action(const api::JsonValue& value) noexcept
    -> std::optional<input::ConfiguredBindingAction> {
  const auto kind = api::json_string(value, "kind");
  if (!kind.has_value()) {
    return std::nullopt;
  }
  if (*kind == "command") {
    return decode_command_action(value);
  }
  if (*kind == "hosted") {
    const auto index = api::json_unsigned(value, "index");
    const auto disposition = parse_disposition(api::json_string(value, "disposition").value_or(""));
    if (!known_members(value, {"kind", "index", "disposition"}) || !index.has_value() ||
        *index >= limits::hosted_commands_hard_max || !disposition.has_value()) {
      return std::nullopt;
    }
    return input::ConfiguredBindingAction{.kind = input::ConfiguredBindingKind::hosted_command,
                                          .disposition = *disposition,
                                          .hosted_command = static_cast<std::uint8_t>(*index)};
  }
  if (*kind == "push") {
    return decode_push_action(value);
  }
  if (*kind == "pop" || *kind == "replay") {
    if (!known_members(value, {"kind"}) || value.object.size() != 1U) {
      return std::nullopt;
    }
    return input::ConfiguredBindingAction{
        .kind = *kind == "pop" ? input::ConfiguredBindingKind::pop_context
                               : input::ConfiguredBindingKind::replay_deferred};
  }
  if (*kind == "send") {
    return decode_send_action(value);
  }
  return std::nullopt;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto encode(const Configuration& configuration) -> std::optional<std::string> {
  if (!runtime_options_valid(configuration) ||
      configuration.input.binding_count > configuration.input.bindings.size()) {
    return std::nullopt;
  }
  try {
    std::string output = R"({"schema":)";
    if (!api::append_json_string(output, configuration_schema, configuration_document_bytes_max)) {
      return std::nullopt;
    }
    output += R"(,"preset":)";
    if (!api::append_json_string(output, preset_name(configuration.input.preset),
                                 configuration_document_bytes_max)) {
      return std::nullopt;
    }
    output += R"(,"prefix":)";
    if (configuration.input.prefix.has_value()) {
      if (!append_chord(output, *configuration.input.prefix)) {
        return std::nullopt;
      }
    } else {
      output += "null";
    }
    output += R"(,"contexts":[)";
    for (std::size_t index = 0; index < configuration.input.contexts.size(); ++index) {
      if (index > 0U) {
        output += ',';
      }
      const auto context = static_cast<ConfiguredInputContext>(index);
      const auto& configured_context = configuration.input.contexts.at(index);
      output += R"({"context":)";
      if (!api::append_json_string(output, context_name(context),
                                   configuration_document_bytes_max)) {
        return std::nullopt;
      }
      output += R"(,"label":)";
      if (!api::append_json_string(
              output,
              std::string_view(configured_context.label.data(), configured_context.label_size),
              configuration_document_bytes_max)) {
        return std::nullopt;
      }
      output += R"(,"lifetime":)";
      if (!api::append_json_string(output, lifetime_name(configured_context.lifetime),
                                   configuration_document_bytes_max)) {
        return std::nullopt;
      }
      output += R"(,"unbound":)";
      if (!api::append_json_string(output, unbound_name(configured_context.unbound),
                                   configuration_document_bytes_max)) {
        return std::nullopt;
      }
      output += R"(,"preempts":)";
      output += configured_context.preempts_interaction ? "true}" : "false}";
    }
    output += R"(],"bindings":[)";
    const auto bindings =
        std::span(configuration.input.bindings).first(configuration.input.binding_count);
    for (std::size_t index = 0; index < bindings.size(); ++index) {
      const auto& binding = bindings.subspan(index, 1).front();
      if (index > 0U) {
        output += ',';
      }
      output += R"({"context":)";
      if (!api::append_json_string(output, context_name(binding.context),
                                   configuration_document_bytes_max)) {
        return std::nullopt;
      }
      output += R"(,"chord":)";
      if (!append_chord(output, binding.chord)) {
        return std::nullopt;
      }
      output += R"(,"action":)";
      if (!append_binding_action(output, binding.action)) {
        return std::nullopt;
      }
      output += '}';
    }
    output += R"(],"scrollback_lines":)";
    output += configuration.terminal.scrollback_lines.has_value()
                  ? std::to_string(*configuration.terminal.scrollback_lines)
                  : "null";
    output += R"(,"clipboard_read":)";
    output += configuration.terminal.clipboard_read ? "true" : "false";
    output += R"(,"clipboard_write":)";
    output += configuration.terminal.clipboard_write ? "true" : "false";
    output += R"(,"status_line":)";
    output += configuration.ui.status_line ? "true" : "false";
    output += R"(,"outer_title":)";
    output += configuration.ui.outer.title ? "true" : "false";
    output += R"(,"outer_bell":)";
    output += configuration.ui.outer.bell ? "true" : "false";
    output += R"(,"outer_notifications":)";
    output += configuration.ui.outer.notifications ? "true" : "false";
    output += R"(,"outer_progress":)";
    output += configuration.ui.outer.progress ? "true" : "false";
    output += R"(,"outer_cwd":)";
    output += configuration.ui.outer.cwd ? "true" : "false";
    output += R"(,"outer_hyperlinks":)";
    output += configuration.ui.outer.hyperlinks ? "true" : "false";
    output += R"(,"default_cwd":)";
    if (!api::append_json_string(output, configuration.launch.default_cwd,
                                 configuration_document_bytes_max)) {
      return std::nullopt;
    }
    output += R"(,"history_file":)";
    if (!api::append_json_string(output, configuration.history.file,
                                 configuration_document_bytes_max)) {
      return std::nullopt;
    }
    output += R"(,"default_program":[)";
    for (std::size_t index = 0; index < configuration.launch.default_program.size(); ++index) {
      if (index > 0U) {
        output += ',';
      }
      if (!api::append_json_string(output, configuration.launch.default_program.at(index),
                                   configuration_document_bytes_max)) {
        return std::nullopt;
      }
    }
    output += R"(],"extensions":[)";
    bool separator = false;
    for (const auto& extension : configuration.extensions) {
      if (separator) {
        output += ',';
      }
      separator = true;
      output += R"({"name":)";
      if (!api::append_json_string(output, extension.name, configuration_document_bytes_max)) {
        return std::nullopt;
      }
      output += R"(,"argv":[)";
      for (std::size_t index = 0; index < extension.argv.size(); ++index) {
        if (index != 0) {
          output += ',';
        }
        if (!api::append_json_string(output, extension.argv.at(index),
                                     configuration_document_bytes_max)) {
          return std::nullopt;
        }
      }
      output += "]}";
    }
    output += "]}";
    return output.size() <= configuration_document_bytes_max ? std::optional{std::move(output)}
                                                             : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
auto decode(const api::JsonValue& document) noexcept -> DecodeResult {
  if (!known_members(document,
                     {"schema", "preset", "prefix", "contexts", "bindings", "scrollback_lines",
                      "status_line", "default_cwd", "history_file", "default_program", "extensions",
                      "clipboard_read", "clipboard_write", "outer_title", "outer_bell",
                      "outer_notifications", "outer_progress", "outer_cwd", "outer_hyperlinks"}) ||
      (document.object.size() < 10U || document.object.size() > 19U)) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::invalid_document, .field = {}}};
  }
  if (api::json_string(document, "schema") != std::optional{configuration_schema}) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::invalid_schema, .field = "schema"}};
  }
  const auto preset_value = api::json_string(document, "preset");
  const auto preset = preset_value.has_value() ? parse_preset(*preset_value) : std::nullopt;
  if (!preset.has_value()) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::invalid_field, .field = "preset"}};
  }
  const auto* const prefix_value = api::json_member(document, "prefix");
  std::optional<InputChord> prefix;
  if (prefix_value == nullptr) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::invalid_key, .field = "prefix"}};
  }
  if (prefix_value->kind != api::JsonKind::null) {
    prefix = decode_chord(*prefix_value);
    if (!prefix.has_value()) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_key, .field = "prefix"}};
    }
  }

  Configuration result;
  result.input.reset(input::InputMapPreset::none);
  result.input.preset = *preset;
  result.input.prefix = prefix;
  const auto* const contexts_value = api::json_member(document, "contexts");
  if (contexts_value == nullptr || contexts_value->kind != api::JsonKind::array ||
      contexts_value->array.size() != result.input.contexts.size()) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::invalid_field, .field = "contexts"}};
  }
  std::array<bool, static_cast<std::size_t>(ConfiguredInputContext::count)> seen_contexts{};
  for (const auto& context_entry : contexts_value->array) {
    if (!known_members(context_entry, {"context", "label", "lifetime", "unbound", "preempts"}) ||
        context_entry.object.size() != 5U) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_field, .field = "contexts"}};
    }
    const auto context_value = api::json_string(context_entry, "context");
    const auto context = context_value.has_value() ? parse_context(*context_value) : std::nullopt;
    const auto label = api::json_string(context_entry, "label");
    const auto lifetime_value = api::json_string(context_entry, "lifetime");
    const auto lifetime =
        lifetime_value.has_value() ? parse_lifetime(*lifetime_value) : std::nullopt;
    const auto unbound_value = api::json_string(context_entry, "unbound");
    const auto unbound = unbound_value.has_value() ? parse_unbound(*unbound_value) : std::nullopt;
    const auto preempts = api::json_boolean(context_entry, "preempts");
    if (!context.has_value() || !label.has_value() || !lifetime.has_value() ||
        !unbound.has_value() || !preempts.has_value()) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_field, .field = "contexts"}};
    }
    auto& seen = seen_contexts.at(static_cast<std::size_t>(*context));
    if (seen || !result.input.set_context(*context, {.label = *label,
                                                     .lifetime = *lifetime,
                                                     .unbound = *unbound,
                                                     .preempts_interaction = *preempts})) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_field, .field = "contexts"}};
    }
    seen = true;
  }

  const auto* const bindings_value = api::json_member(document, "bindings");
  if (bindings_value == nullptr || bindings_value->kind != api::JsonKind::array ||
      bindings_value->array.size() > result.input.bindings.size()) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::capacity, .field = "bindings"}};
  }
  for (const auto& binding : bindings_value->array) {
    if (!known_members(binding, {"context", "chord", "action"}) || binding.object.size() != 3U) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_field, .field = "bindings"}};
    }
    const auto context_value = api::json_string(binding, "context");
    const auto context = context_value.has_value() ? parse_context(*context_value) : std::nullopt;
    const auto* const chord_value = api::json_member(binding, "chord");
    const auto chord =
        chord_value == nullptr ? std::optional<InputChord>{} : decode_chord(*chord_value);
    const auto* const action_value = api::json_member(binding, "action");
    const auto action = action_value == nullptr ? std::optional<input::ConfiguredBindingAction>{}
                                                : decode_binding_action(*action_value);
    if (!context.has_value() || !chord.has_value() || !action.has_value()) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_field, .field = "bindings"}};
    }
    if (!result.input.set_action(*context, *chord, *action)) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::capacity, .field = "bindings"}};
    }
  }

  const auto* const scrollback = api::json_member(document, "scrollback_lines");
  if (scrollback == nullptr) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::invalid_field, .field = "scrollback_lines"}};
  }
  if (scrollback->kind != api::JsonKind::null) {
    const auto lines = api::json_unsigned(document, "scrollback_lines");
    if (!lines.has_value() || *lines > limits::terminal_scrollback_lines_hard_max) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_field, .field = "scrollback_lines"}};
    }
    result.terminal.scrollback_lines = static_cast<std::size_t>(*lines);
  }
  for (const auto name :
       {std::string_view{"clipboard_read"}, std::string_view{"clipboard_write"},
        std::string_view{"outer_title"}, std::string_view{"outer_bell"},
        std::string_view{"outer_notifications"}, std::string_view{"outer_progress"},
        std::string_view{"outer_cwd"}, std::string_view{"outer_hyperlinks"}}) {
    if (api::json_member(document, name) == nullptr) {
      continue;
    }
    const auto allowed = api::json_boolean(document, name);
    if (!allowed.has_value()) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_field, .field = name}};
    }
    if (name == "clipboard_read") {
      result.terminal.clipboard_read = *allowed;
    } else if (name == "clipboard_write") {
      result.terminal.clipboard_write = *allowed;
    } else if (name == "outer_title") {
      result.ui.outer.title = *allowed;
    } else if (name == "outer_bell") {
      result.ui.outer.bell = *allowed;
    } else if (name == "outer_notifications") {
      result.ui.outer.notifications = *allowed;
    } else if (name == "outer_progress") {
      result.ui.outer.progress = *allowed;
    } else if (name == "outer_cwd") {
      result.ui.outer.cwd = *allowed;
    } else {
      result.ui.outer.hyperlinks = *allowed;
    }
  }
  const auto status_line = api::json_boolean(document, "status_line");
  const auto default_cwd = api::json_string(document, "default_cwd");
  const auto history_file = api::json_string(document, "history_file");
  const auto* const default_program = api::json_member(document, "default_program");
  if (!status_line.has_value() || !default_cwd.has_value() || !history_file.has_value() ||
      default_program == nullptr || default_program->kind != api::JsonKind::array ||
      default_program->array.size() > default_program_arguments_max) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::invalid_field, .field = "runtime"}};
  }
  try {
    result.ui.status_line = *status_line;
    result.launch.default_cwd = *default_cwd;
    result.history.file = *history_file;
    result.launch.default_program.reserve(default_program->array.size());
    for (const auto& argument : default_program->array) {
      if (argument.kind != api::JsonKind::string) {
        return {.configuration = std::nullopt,
                .failure = {.error = Error::invalid_field, .field = "default_program"}};
      }
      result.launch.default_program.push_back(argument.string);
    }
  } catch (...) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::capacity, .field = "runtime"}};
  }
  if (const auto* extensions = api::json_member(document, "extensions"); extensions != nullptr) {
    if (extensions->kind != api::JsonKind::array || extensions->array.size() > extensions_max) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::invalid_field, .field = "extensions"}};
    }
    try {
      for (const auto& value : extensions->array) {
        const auto name = api::json_string(value, "name");
        const auto* argv = api::json_member(value, "argv");
        if (!known_members(value, {"name", "argv"}) || !name.has_value() || argv == nullptr ||
            argv->kind != api::JsonKind::array) {
          return {.configuration = std::nullopt,
                  .failure = {.error = Error::invalid_field, .field = "extensions"}};
        }
        ExtensionConfiguration extension{.name = std::string(*name), .argv = {}};
        for (const auto& argument : argv->array) {
          if (argument.kind != api::JsonKind::string) {
            return {.configuration = std::nullopt,
                    .failure = {.error = Error::invalid_field, .field = "extensions"}};
          }
          extension.argv.push_back(argument.string);
        }
        result.extensions.push_back(std::move(extension));
      }
    } catch (...) {
      return {.configuration = std::nullopt,
              .failure = {.error = Error::capacity, .field = "extensions"}};
    }
  }
  if (!runtime_options_valid(result)) {
    return {.configuration = std::nullopt,
            .failure = {.error = Error::invalid_field, .field = "runtime"}};
  }
  return {.configuration = std::move(result), .failure = {}};
}

auto decode(const std::string_view document) -> DecodeResult {
  if (document.size() > configuration_document_bytes_max) {
    return {.configuration = std::nullopt, .failure = {.error = Error::capacity, .field = {}}};
  }
  const auto parsed = api::parse_json(document);
  return parsed.value.has_value()
             ? decode(*parsed.value)
             : DecodeResult{.configuration = std::nullopt,
                            .failure = {.error = Error::invalid_document, .field = {}}};
}

auto compile(const Configuration& configuration) noexcept -> std::expected<Generation, Error> {
  if (!runtime_options_valid(configuration)) {
    return std::unexpected(Error::invalid_field);
  }
  auto compiled = input::compile_input_map(configuration.input);
  if (!compiled.has_value()) {
    return std::unexpected(Error::input_map);
  }
  try {
    std::vector<std::byte> default_program;
    std::size_t program_bytes = 0;
    for (const auto& argument : configuration.launch.default_program) {
      program_bytes += argument.size() + 1U;
    }
    default_program.reserve(program_bytes);
    for (const auto& argument : configuration.launch.default_program) {
      const auto bytes = std::as_bytes(std::span(argument.data(), argument.size()));
      default_program.insert(default_program.end(), bytes.begin(), bytes.end());
      default_program.push_back(std::byte{0});
    }
    auto default_cwd = configuration.launch.default_cwd;
    auto history_file = configuration.history.file;
    return Generation(std::move(*compiled), configuration.terminal.scrollback_lines,
                      configuration.ui.status_line, std::move(default_cwd),
                      std::move(default_program), std::move(history_file), configuration.extensions,
                      configuration.terminal.clipboard_read, configuration.terminal.clipboard_write,
                      configuration.ui.outer);
  } catch (...) {
    return std::unexpected(Error::capacity);
  }
}

} // namespace lemma::config
