#include "extension/lua_host.hpp"

#include "extension/defaults.hpp"
#include "extension/lua_commands.hpp"
#include "extension/services.hpp"

#include "api/json.hpp"
#include "config/config.hpp"
#include "extension/commands.hpp"
#include "input/input_router.hpp"
#include "lemma/limits.hpp"
#include "platform/io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#endif

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace lemma::extension {
namespace {

using platform::close_descriptor;

inline constexpr std::array<std::byte, 4> host_magic{std::byte{'L'}, std::byte{'M'}, std::byte{'C'},
                                                     std::byte{'F'}};
inline constexpr std::size_t host_header_bytes = 12;
inline constexpr std::size_t diagnostic_bytes_max = 4'096;
inline constexpr std::size_t lua_memory_bytes_max = std::size_t{64} * 1'024U * 1'024U;
inline constexpr auto startup_timeout = std::chrono::seconds(2);

enum class HostMessageStatus : std::uint8_t {
  configured = 1,
  failed = 2,
};

struct LuaAllocator final {
  std::size_t used{0};
};

struct LuaConfiguration final {
  config::Configuration configuration;
  LuaCommands commands;
  config::LaunchConfiguration extension_program;
};

[[nodiscard]] auto host_configuration(lua_State* const state) noexcept -> LuaConfiguration& {
  return *static_cast<LuaConfiguration*>(lua_touserdata(state, lua_upvalueindex(1)));
}

[[nodiscard]] auto lua_allocate(void* const context, void* const pointer,
                                const std::size_t old_size, const std::size_t new_size) noexcept
    -> void* {
  auto& allocator = *static_cast<LuaAllocator*>(context);
  const auto accounted_old_size = pointer == nullptr ? 0U : old_size;
  if (new_size == 0U) {
    // Lua's allocator contract requires realloc-compatible storage.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    std::free(pointer);
    allocator.used =
        accounted_old_size <= allocator.used ? allocator.used - accounted_old_size : 0U;
    return nullptr;
  }
  const auto retained =
      accounted_old_size <= allocator.used ? allocator.used - accounted_old_size : 0U;
  if (new_size > lua_memory_bytes_max - retained) {
    return nullptr;
  }
  // Lua's allocator contract requires realloc-compatible storage.
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
  void* const resized = std::realloc(pointer, new_size);
  if (resized != nullptr) {
    allocator.used = retained + new_size;
  }
  return resized;
}

[[nodiscard]] auto raise_lua_error(lua_State* const state, const std::string_view message) noexcept
    -> int {
  lua_pushlstring(state, message.data(), message.size());
  return lua_error(state);
}

[[nodiscard]] auto lua_table_key(lua_State* const state) noexcept
    -> std::optional<std::string_view> {
  if (lua_type(state, -2) != LUA_TSTRING) {
    return std::nullopt;
  }
  std::size_t size = 0;
  const char* const data = lua_tolstring(state, -2, &size);
  return data == nullptr ? std::nullopt : std::optional<std::string_view>{{data, size}};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto read_input_options(lua_State* const state, const int table,
                                      input::InputMapConfiguration& target) -> int {
  const auto absolute = lua_absindex(state, table);
  std::optional<input::InputMapPreset> preset;
  std::optional<input::InputChord> prefix;
  bool prefix_seen = false;
  lua_pushnil(state);
  while (lua_next(state, absolute) != 0) {
    const auto key = lua_table_key(state);
    if (!key.has_value()) {
      return raise_lua_error(state, "lemma.setup.input keys must be strings");
    }
    if (*key == "preset") {
      std::size_t size = 0;
      const char* const value = luaL_checklstring(state, -1, &size);
      if (std::string_view(value, size) == "default") {
        preset = input::InputMapPreset::defaults;
      } else if (std::string_view(value, size) == "none") {
        preset = input::InputMapPreset::none;
      } else {
        return raise_lua_error(state, "input.preset must be 'default' or 'none'");
      }
    } else if (*key == "prefix") {
      prefix_seen = true;
      if (lua_type(state, -1) == LUA_TBOOLEAN && lua_toboolean(state, -1) == 0) {
        prefix = std::nullopt;
      } else {
        std::size_t size = 0;
        const char* const value = luaL_checklstring(state, -1, &size);
        prefix = config::parse_key({value, size});
        if (!prefix.has_value()) {
          return raise_lua_error(state, "invalid input.prefix key");
        }
      }
    } else {
      return raise_lua_error(state, "unknown lemma.setup.input option");
    }
    lua_pop(state, 1);
  }
  if (preset.has_value()) {
    target.reset(*preset);
  }
  if (prefix_seen && !target.set_prefix(prefix)) {
    return raise_lua_error(state, "Lemma keymap capacity reached while setting input.prefix");
  }
  return 0;
}

// Validate the complete terminal policy before publishing any field.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto read_terminal_options(lua_State* const state, const int table,
                                         config::TerminalConfiguration& target) -> int {
  const auto absolute = lua_absindex(state, table);
  lua_pushnil(state);
  while (lua_next(state, absolute) != 0) {
    const auto key = lua_table_key(state);
    if (key == std::optional<std::string_view>{"clipboard_read"} ||
        key == std::optional<std::string_view>{"clipboard_write"}) {
      if (lua_type(state, -1) != LUA_TBOOLEAN) {
        return raise_lua_error(state, "terminal clipboard policy must be boolean");
      }
      if (*key == "clipboard_read") {
        target.clipboard_read = lua_toboolean(state, -1) != 0;
      } else {
        target.clipboard_write = lua_toboolean(state, -1) != 0;
      }
      lua_pop(state, 1);
      continue;
    }
    if (key != std::optional<std::string_view>{"scrollback_lines"}) {
      return raise_lua_error(state, "unknown lemma.setup.terminal option");
    }
    if (lua_type(state, -1) == LUA_TBOOLEAN && lua_toboolean(state, -1) == 0) {
      target.scrollback_lines = std::nullopt;
    } else if (lua_isinteger(state, -1) == 0) {
      return raise_lua_error(state, "terminal.scrollback_lines must be an integer or false");
    } else {
      const auto lines = lua_tointeger(state, -1);
      if (lines < 0 || std::cmp_greater(lines, limits::terminal_scrollback_lines_hard_max)) {
        return raise_lua_error(state, "terminal.scrollback_lines is out of range");
      }
      target.scrollback_lines = static_cast<std::size_t>(lines);
    }
    lua_pop(state, 1);
  }
  return 0;
}

[[nodiscard]] auto read_ui_options(lua_State* const state, const int table,
                                   config::UiConfiguration& target) -> int {
  struct Option final {
    std::string_view name;
    bool config::UiConfiguration::* flag{nullptr};
    bool config::OuterPresentation::* outer{nullptr};
    const char* error{nullptr};
  };
  static constexpr std::array options{
      Option{.name = "status_line",
             .flag = &config::UiConfiguration::status_line,
             .error = "ui.status_line must be a boolean"},
      Option{.name = "outer_title",
             .outer = &config::OuterPresentation::title,
             .error = "ui.outer_title must be a boolean"},
      Option{.name = "outer_notifications",
             .outer = &config::OuterPresentation::notifications,
             .error = "ui.outer_notifications must be a boolean"},
      Option{.name = "outer_progress",
             .outer = &config::OuterPresentation::progress,
             .error = "ui.outer_progress must be a boolean"},
      Option{.name = "outer_cwd",
             .outer = &config::OuterPresentation::cwd,
             .error = "ui.outer_cwd must be a boolean"},
  };
  const auto absolute = lua_absindex(state, table);
  lua_pushnil(state);
  while (lua_next(state, absolute) != 0) {
    const auto key = lua_table_key(state);
    const auto* const option = std::ranges::find_if(
        options, [&key](const Option& candidate) { return key == candidate.name; });
    if (option == options.end()) {
      return raise_lua_error(state, "unknown lemma.setup.ui option");
    }
    if (lua_type(state, -1) != LUA_TBOOLEAN) {
      return raise_lua_error(state, option->error);
    }
    const bool value = lua_toboolean(state, -1) != 0;
    if (option->flag != nullptr) {
      target.*(option->flag) = value;
    } else {
      target.outer.*(option->outer) = value;
    }
    lua_pop(state, 1);
  }
  return 0;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto read_default_program(lua_State* const state, const int table,
                                        config::LaunchConfiguration& target) -> int {
  const auto absolute = lua_absindex(state, table);
  const auto count = lua_rawlen(state, absolute);
  if (count > config::default_program_arguments_max) {
    return raise_lua_error(state, "launch.default_program has too many arguments");
  }
  try {
    target.default_program.clear();
    target.default_program.reserve(count);
  } catch (...) {
    return raise_lua_error(state, "launch.default_program allocation failed");
  }
  std::size_t bytes = 0;
  for (std::size_t index = 1; index <= count; ++index) {
    lua_rawgeti(state, absolute, static_cast<lua_Integer>(index));
    if (lua_type(state, -1) != LUA_TSTRING) {
      return raise_lua_error(state, "launch.default_program must be an array of strings");
    }
    std::size_t size = 0;
    const char* const value = lua_tolstring(state, -1, &size);
    if ((index == 1U && size == 0U) || value == nullptr ||
        size + 1U > config::default_program_bytes_max - bytes) {
      return raise_lua_error(state, "launch.default_program is invalid or too large");
    }
    try {
      target.default_program.emplace_back(value, size);
    } catch (...) {
      return raise_lua_error(state, "launch.default_program allocation failed");
    }
    bytes += size + 1U;
    lua_pop(state, 1);
  }
  std::size_t members = 0;
  lua_pushnil(state);
  while (lua_next(state, absolute) != 0) {
    const auto index = lua_isinteger(state, -2) != 0 ? lua_tointeger(state, -2) : lua_Integer{0};
    if (index <= 0 || static_cast<std::size_t>(index) > count) {
      return raise_lua_error(state, "launch.default_program must be a dense array");
    }
    ++members;
    lua_pop(state, 1);
  }
  return members == count ? 0
                          : raise_lua_error(state, "launch.default_program must be a dense array");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto read_launch_options(lua_State* const state, const int table,
                                       config::LaunchConfiguration& target) -> int {
  const auto absolute = lua_absindex(state, table);
  lua_pushnil(state);
  while (lua_next(state, absolute) != 0) {
    const auto key = lua_table_key(state);
    if (!key.has_value()) {
      return raise_lua_error(state, "lemma.setup.launch keys must be strings");
    }
    if (*key == "default_cwd") {
      std::size_t size = 0;
      const char* const value = luaL_checklstring(state, -1, &size);
      if (size > limits::working_directory_bytes_max || value == nullptr ||
          std::string_view(value, size).contains('\0') || (size > 0U && *value != '/')) {
        return raise_lua_error(state, "launch.default_cwd must be empty or absolute");
      }
      try {
        target.default_cwd.assign(value, size);
      } catch (...) {
        return raise_lua_error(state, "launch.default_cwd allocation failed");
      }
    } else if (*key == "default_program") {
      luaL_checktype(state, -1, LUA_TTABLE);
      if (read_default_program(state, -1, target) != 0) {
        return 0;
      }
    } else {
      return raise_lua_error(state, "unknown lemma.setup.launch option");
    }
    lua_pop(state, 1);
  }
  return 0;
}

[[nodiscard]] auto read_history_options(lua_State* const state, const int table,
                                        config::HistoryConfiguration& target) -> int {
  const auto absolute = lua_absindex(state, table);
  lua_pushnil(state);
  while (lua_next(state, absolute) != 0) {
    const auto key = lua_table_key(state);
    if (key != std::optional<std::string_view>{"file"}) {
      return raise_lua_error(state, "history.file is the only supported history option");
    }
    std::size_t size = 0;
    const char* const value = luaL_checklstring(state, -1, &size);
    if (value == nullptr || size > config::configuration_path_bytes_max ||
        std::string_view(value, size).contains('\0') || (size > 0U && *value != '/')) {
      return raise_lua_error(state, "history.file must be empty or an absolute path");
    }
    try {
      target.file.assign(value, size);
    } catch (...) {
      return raise_lua_error(state, "history.file allocation failed");
    }
    lua_pop(state, 1);
  }
  return 0;
}

[[nodiscard]] auto config_setup(lua_State* const state) -> int {
  luaL_checktype(state, 1, LUA_TTABLE);
  auto& target = host_configuration(state).configuration;
  lua_pushnil(state);
  while (lua_next(state, 1) != 0) {
    const auto key = lua_table_key(state);
    if (!key.has_value()) {
      return raise_lua_error(state, "lemma.setup keys must be strings");
    }
    luaL_checktype(state, -1, LUA_TTABLE);
    if ((*key == "input" && read_input_options(state, -1, target.input) != 0) ||
        (*key == "terminal" && read_terminal_options(state, -1, target.terminal) != 0) ||
        (*key == "ui" && read_ui_options(state, -1, target.ui) != 0) ||
        (*key == "launch" && read_launch_options(state, -1, target.launch) != 0) ||
        (*key == "history" && read_history_options(state, -1, target.history) != 0)) {
      return 0;
    }
    if (*key != "input" && *key != "terminal" && *key != "ui" && *key != "launch" &&
        *key != "history") {
      return raise_lua_error(state, "unknown lemma.setup option");
    }
    lua_pop(state, 1);
  }
  return 0;
}

[[nodiscard]] auto
command_binding_action(lua_State* const state, const int index,
                       const input::CommandContextDisposition disposition) noexcept
    -> std::optional<input::ConfiguredBindingAction> {
  std::size_t size = 0;
  const char* const value = lua_tolstring(state, index, &size);
  const auto command = value == nullptr ? std::optional<input::InputCommand>{}
                                        : config::parse_command({value, size});
  if (command.has_value()) {
    return input::ConfiguredBindingAction{.kind = input::ConfiguredBindingKind::command,
                                          .command = *command,
                                          .disposition = disposition};
  }
  const auto& commands = host_configuration(state).commands.descriptors;
  const auto found = std::ranges::find_if(commands, [&](const auto& descriptor) {
    return value != nullptr && descriptor.name == std::string_view(value, size);
  });
  if (found == commands.end()) {
    return std::nullopt;
  }
  return input::ConfiguredBindingAction{.kind = input::ConfiguredBindingKind::hosted_command,
                                        .disposition = disposition,
                                        .hosted_command =
                                            static_cast<std::uint8_t>(found - commands.begin())};
}

[[nodiscard]] auto table_string_field(lua_State* const state, const int table,
                                      const char* const field) noexcept -> std::string_view {
  lua_getfield(state, table, field);
  std::size_t size = 0;
  const char* const data = lua_tolstring(state, -1, &size);
  const auto value = data == nullptr ? std::string_view{} : std::string_view(data, size);
  lua_pop(state, 1);
  return value;
}

[[nodiscard]] auto push_binding_action(lua_State* const state, const int table) noexcept
    -> std::optional<input::ConfiguredBindingAction> {
  const auto context = config::parse_context(table_string_field(state, table, "context"));
  lua_getfield(state, table, "defer");
  const bool defer = lua_type(state, -1) == LUA_TBOOLEAN && lua_toboolean(state, -1) != 0;
  lua_pop(state, 1);
  if (!context.has_value()) {
    return std::nullopt;
  }
  return input::ConfiguredBindingAction{
      .kind = input::ConfiguredBindingKind::push_context, .target = *context, .defer_chord = defer};
}

[[nodiscard]] constexpr auto send_physical_key(const input::InputChord chord) noexcept
    -> std::optional<input::PhysicalKey> {
  if (chord.kind == input::ChordKind::key &&
      chord.code < static_cast<std::uint16_t>(input::PhysicalKey::count)) {
    return static_cast<input::PhysicalKey>(chord.code);
  }
  if (chord.kind != input::ChordKind::byte) {
    return std::nullopt;
  }
  switch (chord.code) {
  case 0x0DU:
    return input::PhysicalKey::enter;
  case 0x09U:
    return input::PhysicalKey::tab;
  case 0x7FU:
    return input::PhysicalKey::backspace;
  case 0x1BU:
    return input::PhysicalKey::escape;
  case 0x20U:
    return input::PhysicalKey::space;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] auto send_binding_action(lua_State* const state, const int table) noexcept
    -> std::optional<input::ConfiguredBindingAction> {
  const auto key = config::parse_key(table_string_field(state, table, "key"));
  const auto physical = key.has_value() ? send_physical_key(*key) : std::nullopt;
  if (!physical.has_value()) {
    return std::nullopt;
  }
  return input::ConfiguredBindingAction{.kind = input::ConfiguredBindingKind::send_key,
                                        .encoded_key = *physical,
                                        .encoded_modifiers = key->modifiers};
}

[[nodiscard]] auto binding_action(lua_State* const state, const int index,
                                  const input::CommandContextDisposition disposition) noexcept
    -> std::optional<input::ConfiguredBindingAction> {
  if (lua_type(state, index) == LUA_TSTRING) {
    return command_binding_action(state, index, disposition);
  }
  if (lua_type(state, index) != LUA_TTABLE) {
    return std::nullopt;
  }
  const auto table = lua_absindex(state, index);
  const auto kind = table_string_field(state, table, "kind");
  if (kind == "push") {
    return push_binding_action(state, table);
  }
  if (kind == "pop" || kind == "replay") {
    return input::ConfiguredBindingAction{
        .kind = kind == "pop" ? input::ConfiguredBindingKind::pop_context
                              : input::ConfiguredBindingKind::replay_deferred};
  }
  if (kind == "send") {
    return send_binding_action(state, table);
  }
  return std::nullopt;
}

[[nodiscard]] auto keymap_set(lua_State* const state) -> int {
  std::size_t context_size = 0;
  std::size_t key_size = 0;
  const char* const context_data = luaL_checklstring(state, 1, &context_size);
  const char* const key_data = luaL_checklstring(state, 2, &key_size);
  const auto context = config::parse_context({context_data, context_size});
  const auto chord = config::parse_key({key_data, key_size});
  if (!context.has_value()) {
    return raise_lua_error(state, "invalid Lemma keymap context");
  }
  if (!chord.has_value()) {
    return raise_lua_error(state, "invalid Lemma key");
  }
  auto disposition = input::CommandContextDisposition::retain;
  if (!lua_isnoneornil(state, 4)) {
    std::size_t disposition_size = 0;
    const char* const disposition_data = luaL_checklstring(state, 4, &disposition_size);
    const std::string_view value(disposition_data, disposition_size);
    if (value == "base") {
      disposition = input::CommandContextDisposition::base;
    } else if (value != "retain") {
      return raise_lua_error(state, "keymap disposition must be 'retain' or 'base'");
    }
  }
  const auto action = binding_action(state, 3, disposition);
  if (!action.has_value()) {
    return raise_lua_error(state, "invalid Lemma keymap action");
  }
  if (!host_configuration(state).configuration.input.set_action(*context, *chord, *action)) {
    return raise_lua_error(state, "Lemma keymap capacity reached");
  }
  return 0;
}

[[nodiscard]] auto push_action(lua_State* const state, const std::string_view kind) -> int {
  lua_createtable(state, 0, 3);
  lua_pushlstring(state, kind.data(), kind.size());
  lua_setfield(state, -2, "kind");
  return 1;
}

[[nodiscard]] auto context_push(lua_State* const state) -> int {
  std::size_t size = 0;
  const char* const value = luaL_checklstring(state, 1, &size);
  if (!config::parse_context({value, size}).has_value()) {
    return raise_lua_error(state, "invalid Lemma input context");
  }
  static_cast<void>(push_action(state, "push"));
  lua_pushlstring(state, value, size);
  lua_setfield(state, -2, "context");
  bool defer = false;
  if (!lua_isnoneornil(state, 2)) {
    luaL_checktype(state, 2, LUA_TTABLE);
    lua_getfield(state, 2, "defer");
    if (!lua_isnil(state, -1) && lua_type(state, -1) != LUA_TBOOLEAN) {
      return raise_lua_error(state, "context.push defer must be a boolean");
    }
    defer = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
  }
  lua_pushboolean(state, defer ? 1 : 0);
  lua_setfield(state, -2, "defer");
  return 1;
}

[[nodiscard]] auto context_pop(lua_State* const state) -> int { return push_action(state, "pop"); }

[[nodiscard]] auto keymap_replay(lua_State* const state) -> int {
  return push_action(state, "replay");
}

[[nodiscard]] auto keymap_send(lua_State* const state) -> int {
  std::size_t size = 0;
  const char* const value = luaL_checklstring(state, 1, &size);
  const auto key = config::parse_key({value, size});
  if (!key.has_value() || !send_physical_key(*key).has_value()) {
    return raise_lua_error(state, "keymap.send requires a named physical key");
  }
  static_cast<void>(push_action(state, "send"));
  lua_pushlstring(state, value, size);
  lua_setfield(state, -2, "key");
  return 1;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto context_set(lua_State* const state) -> int {
  std::size_t size = 0;
  const char* const value = luaL_checklstring(state, 1, &size);
  const auto context = config::parse_context({value, size});
  luaL_checktype(state, 2, LUA_TTABLE);
  if (!context.has_value()) {
    return raise_lua_error(state, "invalid Lemma input context");
  }
  auto& input = host_configuration(state).configuration.input;
  const auto& current = input.contexts.at(static_cast<std::size_t>(*context));
  std::array<char, input::input_context_label_bytes_max> label = current.label;
  std::size_t label_size = current.label_size;
  auto lifetime = current.lifetime;
  auto unbound = current.unbound;
  bool preempts = current.preempts_interaction;
  lua_pushnil(state);
  while (lua_next(state, 2) != 0) {
    const auto key = lua_table_key(state);
    if (!key.has_value()) {
      return raise_lua_error(state, "lemma.context.set keys must be strings");
    }
    if (*key == "label") {
      std::size_t configured_size = 0;
      const char* const configured = luaL_checklstring(state, -1, &configured_size);
      if (configured_size > label.size() ||
          std::string_view(configured, configured_size).contains('\0')) {
        return raise_lua_error(state, "context label is too large or invalid");
      }
      label = {};
      std::ranges::copy(std::string_view(configured, configured_size), label.begin());
      label_size = configured_size;
    } else if (*key == "lifetime") {
      std::size_t configured_size = 0;
      const char* const configured = luaL_checklstring(state, -1, &configured_size);
      const std::string_view selected(configured, configured_size);
      if (selected == "persistent") {
        lifetime = input::ContextLifetime::persistent;
      } else if (selected == "one_shot") {
        lifetime = input::ContextLifetime::one_shot;
      } else {
        return raise_lua_error(state, "context lifetime must be 'persistent' or 'one_shot'");
      }
    } else if (*key == "unbound") {
      std::size_t configured_size = 0;
      const char* const configured = luaL_checklstring(state, -1, &configured_size);
      const std::string_view selected(configured, configured_size);
      if (selected == "forward") {
        unbound = input::UnboundBehavior::forward;
      } else if (selected == "replay") {
        unbound = input::UnboundBehavior::replay_deferred;
      } else if (selected == "consume") {
        unbound = input::UnboundBehavior::consume;
      } else if (selected == "retry") {
        unbound = input::UnboundBehavior::retry_base;
      } else {
        return raise_lua_error(state, "invalid context unbound behavior");
      }
    } else if (*key == "preempts") {
      if (lua_type(state, -1) != LUA_TBOOLEAN) {
        return raise_lua_error(state, "context preempts must be a boolean");
      }
      preempts = lua_toboolean(state, -1) != 0;
    } else {
      return raise_lua_error(state, "unknown lemma.context.set option");
    }
    lua_pop(state, 1);
  }
  if (!input.set_context(*context, {.label = std::string_view(label.data(), label_size),
                                    .lifetime = lifetime,
                                    .unbound = unbound,
                                    .preempts_interaction = preempts})) {
    return raise_lua_error(state, "invalid Lemma context options");
  }
  return 0;
}

[[nodiscard]] auto keymap_del(lua_State* const state) -> int {
  std::size_t context_size = 0;
  std::size_t key_size = 0;
  const char* const context_data = luaL_checklstring(state, 1, &context_size);
  const char* const key_data = luaL_checklstring(state, 2, &key_size);
  const auto context = config::parse_context({context_data, context_size});
  const auto chord = config::parse_key({key_data, key_size});
  if (!context.has_value()) {
    return raise_lua_error(state, "invalid Lemma keymap context");
  }
  if (!chord.has_value()) {
    return raise_lua_error(state, "invalid Lemma key");
  }
  if (!host_configuration(state).configuration.input.unbind(*context, *chord)) {
    return raise_lua_error(state, "Lemma keymap override capacity reached");
  }
  return 0;
}

void set_host_function(lua_State* const state, LuaConfiguration& configuration, const char* name,
                       lua_CFunction function) {
  lua_pushlightuserdata(state, &configuration);
  lua_pushcclosure(state, function, 1);
  lua_setfield(state, -2, name);
}

[[nodiscard]] auto extension_set(lua_State* const state) -> int {
  std::size_t size = 0;
  const auto* name = luaL_checklstring(state, 1, &size);
  const std::string_view key(name, size);
  if (key.empty() || key.size() > 64 || key.contains('\0')) {
    return raise_lua_error(state, "extension name must be 1..64 bytes");
  }
  auto& extensions = host_configuration(state).configuration.extensions;
  if (lua_type(state, 2) == LUA_TBOOLEAN && lua_toboolean(state, 2) == 0) {
    std::erase_if(extensions, [&](const auto& entry) { return entry.name == key; });
    return 0;
  }
  luaL_checktype(state, 2, LUA_TTABLE);
  auto& program = host_configuration(state).extension_program;
  static_cast<void>(read_default_program(state, 2, program));
  if (program.default_program.empty()) {
    return raise_lua_error(state, "extension argv must not be empty");
  }
  const auto found = std::ranges::find(extensions, key, &config::ExtensionConfiguration::name);
  if (found != extensions.end()) {
    found->argv = std::move(program.default_program);
  } else if (extensions.size() < config::extensions_max) {
    extensions.push_back({.name = std::string(key), .argv = std::move(program.default_program)});
  } else {
    return raise_lua_error(state, "extension capacity reached");
  }
  return 0;
}

void install_lemma_module(lua_State* const state, LuaConfiguration& configuration) {
  lua_createtable(state, 0, 3);
  set_host_function(state, configuration, "setup", &config_setup);
  install_commands(state, configuration.commands);
  const auto ui_path = bundled_ui_path();
  lua_pushlstring(state, ui_path.data(), ui_path.size());
  lua_setfield(state, -2, "bundled_ui");
  lua_createtable(state, 0, 1);
  set_host_function(state, configuration, "set", &extension_set);
  lua_setfield(state, -2, "extension");
  lua_createtable(state, 0, 4);
  set_host_function(state, configuration, "set", &keymap_set);
  set_host_function(state, configuration, "del", &keymap_del);
  set_host_function(state, configuration, "replay", &keymap_replay);
  set_host_function(state, configuration, "send", &keymap_send);
  lua_setfield(state, -2, "keymap");
  lua_createtable(state, 0, 3);
  set_host_function(state, configuration, "set", &context_set);
  set_host_function(state, configuration, "push", &context_push);
  set_host_function(state, configuration, "pop", &context_pop);
  lua_setfield(state, -2, "context");
  lua_getfield(state, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_pushvalue(state, -2);
  lua_setfield(state, -2, "lemma");
  lua_pop(state, 2);
}

[[nodiscard]] auto install_config_search_path(lua_State* const state,
                                              const std::string& path) noexcept -> bool {
  try {
    const auto separator = path.find_last_of('/');
    const auto directory =
        separator == std::string::npos ? std::string{"."} : path.substr(0, separator);
    lua_getglobal(state, "package");
    if (!lua_istable(state, -1)) {
      lua_pop(state, 1);
      return false;
    }
    lua_getfield(state, -1, "path");
    std::size_t existing_size = 0;
    const char* const existing = lua_tolstring(state, -1, &existing_size);
    std::string search = directory + "/?.lua;" + directory + "/?/init.lua;";
    if (existing != nullptr) {
      search.append(existing, existing_size);
    }
    lua_pop(state, 1);
    lua_pushlstring(state, search.data(), search.size());
    lua_setfield(state, -2, "path");
    lua_pop(state, 1);
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto append_diagnostic(const std::string_view prefix,
                                     const std::string_view detail) noexcept -> std::string {
  try {
    std::string result(prefix);
    const auto available =
        diagnostic_bytes_max > result.size() ? diagnostic_bytes_max - result.size() : 0U;
    result.append(detail.substr(0, available));
    return result;
  } catch (...) {
    return {};
  }
}

// Host header indexes address a fixed-size array.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] auto encode_header(const HostMessageStatus status, const std::size_t size) noexcept
    -> std::array<std::byte, host_header_bytes> {
  const auto bounded = static_cast<std::uint32_t>(size);
  return {host_magic.at(0),
          host_magic.at(1),
          host_magic.at(2),
          host_magic.at(3),
          std::byte{1},
          std::byte{0},
          static_cast<std::byte>(status),
          std::byte{0},
          static_cast<std::byte>((bounded >> 24U) & 0xFFU),
          static_cast<std::byte>((bounded >> 16U) & 0xFFU),
          static_cast<std::byte>((bounded >> 8U) & 0xFFU),
          static_cast<std::byte>(bounded & 0xFFU)};
}

[[nodiscard]] auto send_host_message(const int descriptor, const HostMessageStatus status,
                                     const std::string_view payload) noexcept -> bool {
  const auto header = encode_header(status, payload.size());
  return platform::write_all(descriptor, header) &&
         platform::write_all(descriptor, std::as_bytes(std::span(payload.data(), payload.size())));
}

[[nodiscard]] auto run_host(const int descriptor, const std::string& path) noexcept -> int {
  LuaAllocator allocator;
  lua_State* const state = lua_newstate(&lua_allocate, &allocator);
  if (state == nullptr) {
    static_cast<void>(send_host_message(descriptor, HostMessageStatus::failed,
                                        "failed to allocate Lua configuration runtime"));
    return 1;
  }
  luaL_openlibs(state);
  LuaConfiguration configuration;
  install_lemma_module(state, configuration);
  if (!install_config_search_path(state, path)) {
    static_cast<void>(send_host_message(descriptor, HostMessageStatus::failed,
                                        "failed to configure Lua module search path"));
    lua_close(state);
    return 1;
  }
  int status = luaL_loadbufferx(state, bundled_defaults.data(), bundled_defaults.size(),
                                "@lemma/defaults.lua", "t");
  if (status == LUA_OK) {
    status = lua_pcall(state, 0, 0, 0);
  }
  if (status == LUA_OK && !path.empty()) {
    status = luaL_loadfilex(state, path.c_str(), "t");
  } else if (status == LUA_OK) {
    lua_pushcfunction(state, [](lua_State*) -> int { return 0; });
  }
  if (status == LUA_OK) {
    status = lua_pcall(state, 0, 0, 0);
  }
  if (status != LUA_OK) {
    const char* const message = lua_tostring(state, -1);
    const auto diagnostic = append_diagnostic("Lua configuration failed: ",
                                              message == nullptr ? "unknown error" : message);
    static_cast<void>(send_host_message(descriptor, HostMessageStatus::failed, diagnostic));
    lua_close(state);
    return 1;
  }
  if (!configuration.configuration.ui.status_line) {
    std::erase_if(configuration.configuration.extensions,
                  [](const auto& entry) { return entry.name == "statusline"; });
  }
  const auto compiled = config::compile(configuration.configuration);
  if (!compiled.has_value()) {
    static_cast<void>(send_host_message(descriptor, HostMessageStatus::failed,
                                        "Lua configuration produced an invalid input map"));
    lua_close(state);
    return 1;
  }
  const auto encoded = config::encode(configuration.configuration);
  const auto registration = encoded.has_value()
                                ? encode_registration(*encoded, configuration.commands.descriptors)
                                : std::nullopt;
  if (!registration.has_value() ||
      !send_host_message(descriptor, HostMessageStatus::configured, *registration)) {
    lua_close(state);
    return 1;
  }

  const auto result = run_commands(state, configuration.commands, descriptor);
  lua_close(state);
  return result;
}

[[nodiscard]] auto set_close_on_exec(const int descriptor) noexcept -> bool {
  // fcntl is variadic because its final argument depends on the command.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const auto flags = ::fcntl(descriptor, F_GETFD, 0);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

[[nodiscard]] auto read_before(const int descriptor, const std::span<std::byte> output,
                               const std::chrono::steady_clock::time_point deadline) noexcept
    -> bool {
  std::size_t used = 0;
  while (used < output.size()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto timeout = static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
    pollfd ready{.fd = descriptor, .events = POLLIN, .revents = 0};
    const auto polled = ::poll(&ready, 1, timeout);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled <= 0 || (ready.revents & (POLLERR | POLLNVAL)) != 0) {
      return false;
    }
    const auto received = ::read(descriptor, output.subspan(used).data(), output.size() - used);
    if (received > 0) {
      used += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

struct HostFrame final {
  HostMessageStatus status{HostMessageStatus::failed};
  std::string payload;
};

// Header accesses follow an exact-size read and payload allocation is caught locally.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] auto receive_host_frame(const int descriptor) noexcept -> std::optional<HostFrame> {
  const auto deadline = std::chrono::steady_clock::now() + startup_timeout;
  std::array<std::byte, host_header_bytes> header{};
  if (!read_before(descriptor, header, deadline) ||
      !std::ranges::equal(std::span(header).first(host_magic.size()), host_magic) ||
      header.at(4) != std::byte{1} || header.at(5) != std::byte{0} ||
      header.at(7) != std::byte{0}) {
    return std::nullopt;
  }
  const auto status = static_cast<HostMessageStatus>(std::to_integer<std::uint8_t>(header.at(6)));
  const auto size = (std::to_integer<std::uint32_t>(header.at(8)) << 24U) |
                    (std::to_integer<std::uint32_t>(header.at(9)) << 16U) |
                    (std::to_integer<std::uint32_t>(header.at(10)) << 8U) |
                    std::to_integer<std::uint32_t>(header.at(11));
  const auto maximum = status == HostMessageStatus::configured
                           ? config::configuration_document_bytes_max
                           : diagnostic_bytes_max;
  if ((status != HostMessageStatus::configured && status != HostMessageStatus::failed) ||
      size > maximum) {
    return std::nullopt;
  }
  HostFrame result{.status = status, .payload = {}};
  try {
    result.payload.resize(size);
  } catch (...) {
    return std::nullopt;
  }
  if (!result.payload.empty() &&
      !read_before(descriptor,
                   std::as_writable_bytes(std::span(result.payload.data(), result.payload.size())),
                   deadline)) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] auto candidate_configuration_path() -> std::string {
  const char* const configured = std::getenv("LEMMA_CONFIG");
  if (configured != nullptr && *configured != '\0') {
    return {configured};
  }
  const char* const xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg != nullptr && *xdg == '/') {
    return std::string(xdg) + "/lemma/init.lua";
  }
  const char* const home = std::getenv("HOME");
  return home != nullptr && *home == '/' ? std::string(home) + "/.config/lemma/init.lua"
                                         : std::string{};
}

enum class ConfigurationFile : std::uint8_t { absent, readable, invalid };

[[nodiscard]] auto configuration_file(const std::string& path) noexcept -> ConfigurationFile {
  if (path.empty()) {
    return ConfigurationFile::absent;
  }
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
    return errno == ENOENT ? ConfigurationFile::absent : ConfigurationFile::invalid;
  }
  // A dangling symlink is an existing invalid configuration, not an absent optional file.
  if (S_ISLNK(status.st_mode) && ::stat(path.c_str(), &status) != 0) {
    return ConfigurationFile::invalid;
  }
  return S_ISREG(status.st_mode) && ::access(path.c_str(), R_OK) == 0 ? ConfigurationFile::readable
                                                                      : ConfigurationFile::invalid;
}

[[nodiscard]] auto spawn_host(const std::string& path) noexcept
    -> std::pair<HostProcess, std::optional<HostFrame>> {
  std::array<int, 2> sockets{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0 ||
      !set_close_on_exec(sockets.front()) || !set_close_on_exec(sockets.back())) {
    close_descriptor(sockets.front());
    close_descriptor(sockets.back());
    return {};
  }
  const auto child = ::fork();
  if (child < 0) {
    close_descriptor(sockets.front());
    close_descriptor(sockets.back());
    return {};
  }
  if (child == 0) {
    close_descriptor(sockets.front());
    if (::setpgid(0, 0) != 0) {
      close_descriptor(sockets.back());
      ::_exit(1);
    }
    const auto result = run_host(sockets.back(), path);
    close_descriptor(sockets.back());
    ::_exit(result);
  }
  close_descriptor(sockets.back());
  // Either side may win the setpgid race. EACCES means the child already executed the matching
  // transition; an exited child is handled by startup framing and HostProcess cleanup.
  static_cast<void>(::setpgid(child, child));
  HostProcess host(sockets.front(), static_cast<int>(child));
  auto frame = receive_host_frame(sockets.front());
  return {std::move(host), std::move(frame)};
}

enum class ProcessWait : std::uint8_t {
  exited,
  running,
  unavailable,
};

[[nodiscard]] auto peek_process(const int process, siginfo_t& information) noexcept -> int {
  while (true) {
    information = {};
    const auto result =
        ::waitid(P_PID, static_cast<id_t>(process), &information, WEXITED | WNOHANG | WNOWAIT);
    if (result == 0 || errno != EINTR) {
      return result;
    }
  }
}

[[nodiscard]] auto wait_for_process_exit(const int process) noexcept -> ProcessWait {
  for (std::size_t attempt = 0; attempt < 50U; ++attempt) {
    siginfo_t information{};
    const auto waited = peek_process(process, information);
    if (waited == 0 && information.si_pid == process) {
      return ProcessWait::exited;
    }
    if (waited < 0 && errno == ECHILD) {
      return ProcessWait::unavailable;
    }
    if (waited < 0 && errno != EINTR) {
      return ProcessWait::running;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return ProcessWait::running;
}

} // namespace

HostProcess::HostProcess(HostProcess&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      process_(std::exchange(other.process_, -1)) {}

auto HostProcess::operator=(HostProcess&& other) noexcept -> HostProcess& {
  if (this != &other) {
    reset();
    descriptor_ = std::exchange(other.descriptor_, -1);
    process_ = std::exchange(other.process_, -1);
  }
  return *this;
}

HostProcess::~HostProcess() { reset(); }

void HostProcess::terminate() noexcept {
  if (descriptor_ >= 0) {
    static_cast<void>(::shutdown(descriptor_, SHUT_RDWR));
  }
  if (process_ > 0) {
    // Observe without reaping: even an exited host protects the group identity until waitpid.
    siginfo_t information{};
    if (peek_process(process_, information) == 0) {
      static_cast<void>(::kill(-process_, SIGKILL));
    } else if (errno == ECHILD) {
      process_ = -1;
    }
  }
}

void HostProcess::reap_exited() noexcept {
  if (process_ <= 0) {
    return;
  }
  siginfo_t information{};
  if (peek_process(process_, information) != 0) {
    if (errno == ECHILD) {
      process_ = -1;
    }
    return;
  }
  if (information.si_pid != process_) {
    return;
  }
  const auto exited = std::exchange(process_, -1);
  static_cast<void>(::kill(-exited, SIGKILL));
  while (::waitpid(exited, nullptr, WNOHANG) < 0 && errno == EINTR) {
  }
}

void HostProcess::reset() noexcept {
  close_descriptor(descriptor_);
  if (process_ <= 0) {
    return;
  }
  const auto initial_wait = wait_for_process_exit(process_);
  if (initial_wait != ProcessWait::running) {
    if (initial_wait == ProcessWait::exited) {
      reap_exited();
    }
    process_ = -1;
    return;
  }
  static_cast<void>(::kill(-process_, SIGTERM));
  if (wait_for_process_exit(process_) != ProcessWait::running) {
    reap_exited();
    process_ = -1;
    return;
  }
  static_cast<void>(::kill(-process_, SIGKILL));
  while (::waitpid(process_, nullptr, 0) < 0 && errno == EINTR) {
  }
  process_ = -1;
}

namespace {
[[nodiscard]] auto decode_configuration(const std::string_view payload, ConfigurationLoad& result)
    -> bool {
  const auto registration = api::parse_json(payload);
  if (!registration.value.has_value() || registration.value->object.size() != 2U) {
    result.diagnostic = "configuration runtime returned an invalid document";
    return false;
  }
  const auto* const configuration = api::json_member(*registration.value, "configuration");
  const auto* const commands = api::json_member(*registration.value, "commands");
  auto declarations = commands == nullptr ? std::nullopt : decode_commands(*commands);
  auto decoded = configuration == nullptr ? config::DecodeResult{} : config::decode(*configuration);
  if (!decoded.configuration.has_value() || !declarations.has_value()) {
    result.diagnostic = "configuration runtime returned an invalid document";
    return false;
  }
  const auto& input = decoded.configuration->input;
  for (const auto& binding : std::span(input.bindings).first(input.binding_count)) {
    if (binding.action.kind == input::ConfiguredBindingKind::hosted_command &&
        binding.action.hosted_command >= declarations->size()) {
      result.diagnostic = "input binding references an undeclared command";
      return false;
    }
  }
  auto compiled = config::compile(*decoded.configuration);
  if (!compiled.has_value()) {
    result.diagnostic = "configuration input map failed validation";
    return false;
  }
  result.generation = std::make_unique<config::Generation>(std::move(*compiled));
  result.commands = std::move(*declarations);
  result.status = result.path.empty() ? ConfigurationStatus::absent : ConfigurationStatus::loaded;
  return true;
}

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
auto load_configuration_impl(const std::optional<std::string_view> requested_path,
                             const bool builtins_only) noexcept -> ConfigurationLoad {
  ConfigurationLoad result;
  const char* const configured_environment = std::getenv("LEMMA_CONFIG");
  const bool path_required =
      !builtins_only && (requested_path.has_value() ||
                         (configured_environment != nullptr && *configured_environment != '\0'));
  try {
    if (!builtins_only) {
      result.path = requested_path.has_value() ? std::string(*requested_path)
                                               : candidate_configuration_path();
    }
  } catch (...) {
    result.status = ConfigurationStatus::invalid;
    result.diagnostic = "configuration path allocation failed";
    return result;
  }
  if ((path_required && result.path.empty()) ||
      result.path.size() > config::configuration_path_bytes_max || result.path.contains('\0')) {
    result.status = path_required ? ConfigurationStatus::invalid : ConfigurationStatus::absent;
    result.diagnostic = path_required ? "invalid configuration path" : std::string{};
    return result;
  }
  const auto file = configuration_file(result.path);
  if (file == ConfigurationFile::invalid || (file == ConfigurationFile::absent && path_required)) {
    result.status = ConfigurationStatus::invalid;
    result.diagnostic = "configuration file is not a readable regular file";
    return result;
  }
  if (file == ConfigurationFile::absent) {
    result.path.clear();
  }

  auto [host, frame] = spawn_host(result.path);
  if (!frame.has_value()) {
    result.status = ConfigurationStatus::invalid;
    result.diagnostic = "configuration runtime timed out or exited without a valid result";
    return result;
  }
  if (frame->status == HostMessageStatus::failed) {
    result.status = ConfigurationStatus::invalid;
    result.diagnostic = std::move(frame->payload);
    return result;
  }
  try {
    if (decode_configuration(frame->payload, result)) {
      result.host = std::move(host);
      return result;
    }
  } catch (...) {
    result.diagnostic = "configuration publication allocation failed";
  }
  result.status = ConfigurationStatus::invalid;
  return result;
}

} // namespace

auto load_configuration(const std::optional<std::string_view> requested_path) noexcept
    -> ConfigurationLoad {
  return load_configuration_impl(requested_path, false);
}

auto load_builtin_configuration() noexcept -> ConfigurationLoad {
  return load_configuration_impl(std::nullopt, true);
}

auto run_configuration_host(const std::string_view requested, const bool required) noexcept -> int {
  constexpr int channel = 3;
  if (!set_close_on_exec(channel)) {
    return 1;
  }
  try {
    std::string path(requested);
    const auto file = configuration_file(path);
    if (file == ConfigurationFile::invalid || (file == ConfigurationFile::absent && required)) {
      static_cast<void>(send_host_message(channel, HostMessageStatus::failed,
                                          "configuration file is not a readable regular file"));
      return 1;
    }
    if (file == ConfigurationFile::absent) {
      path.clear();
    }
    return run_host(channel, path);
  } catch (...) {
    // Process boundary: allocation/encoding failure must reject the candidate, never publish
    // defaults. If even the bounded error frame cannot be sent, EOF is a failed load.
    static_cast<void>(send_host_message(channel, HostMessageStatus::failed,
                                        "configuration host resource failure"));
    return 1;
  }
}

// POSIX spawn setup keeps descriptor/process cleanup explicit on each fallible operation.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto ConfigurationLoader::start() noexcept -> bool {
  if (load_.host.active()) {
    return false;
  }
  std::array<int, 2> sockets{-1, -1};
  posix_spawn_file_actions_t actions{};
  posix_spawnattr_t attributes{};
  bool actions_ready = false;
  bool attributes_ready = false;
  bool started = false;
  try {
    load_.path = candidate_configuration_path();
    std::array<char, 4096> executable{};
    const auto size = platform::executable_path(executable);
    const std::string_view executable_name(executable.data(), size);
    std::string helper(executable_name.substr(0, executable_name.find_last_of('/') + 1U));
    helper += "lemma-config-host";
    const char* const configured = std::getenv("LEMMA_CONFIG");
    std::string required = configured != nullptr && *configured != '\0' ? "required" : "optional";
    input_.resize(host_header_bytes + config::configuration_document_bytes_max);
    used_ = 0;
    target_ = host_header_bytes;
    finished_ = false;
    if (size != 0 && ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0 &&
        set_close_on_exec(sockets.front()) && set_close_on_exec(sockets.back()) &&
        platform::set_nonblocking(sockets.front()) &&
        ::posix_spawn_file_actions_init(&actions) == 0) {
      actions_ready = true;
      if (::posix_spawnattr_init(&attributes) == 0) {
        attributes_ready = true;
        sigset_t defaults{};
        sigset_t mask{};
        static_cast<void>(sigemptyset(&defaults));
        static_cast<void>(sigemptyset(&mask));
        for (const auto signal : {SIGCHLD, SIGPIPE, SIGINT, SIGTERM, SIGHUP}) {
          static_cast<void>(sigaddset(&defaults, signal));
        }
        std::array arguments{helper.data(), load_.path.data(), required.data(),
                             static_cast<char*>(nullptr)};
        pid_t process = -1;
        constexpr short spawn_flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
                                      POSIX_SPAWN_SETSIGMASK
#ifdef __APPLE__
                                      | POSIX_SPAWN_CLOEXEC_DEFAULT
#endif
            ;
#ifdef __APPLE__
        auto** environment = *_NSGetEnviron();
#else
        auto** environment = ::environ;
#endif
        if (::posix_spawn_file_actions_adddup2(&actions, sockets.back(), 3) == 0 &&
            (sockets.front() == 3 ||
             ::posix_spawn_file_actions_addclose(&actions, sockets.front()) == 0) &&
            (sockets.back() == 3 ||
             ::posix_spawn_file_actions_addclose(&actions, sockets.back()) == 0) &&
#ifndef __APPLE__
            ::posix_spawn_file_actions_addclosefrom_np(&actions, 4) == 0 &&
#endif
            ::posix_spawnattr_setpgroup(&attributes, 0) == 0 &&
            ::posix_spawnattr_setsigdefault(&attributes, &defaults) == 0 &&
            ::posix_spawnattr_setsigmask(&attributes, &mask) == 0 &&
            ::posix_spawnattr_setflags(&attributes, spawn_flags) == 0 &&
            ::posix_spawn(&process, helper.c_str(), &actions, &attributes, arguments.data(),
                          environment) == 0) {
          load_.host = HostProcess(std::exchange(sockets.front(), -1), static_cast<int>(process));
          deadline_ = std::chrono::steady_clock::now() + startup_timeout;
          started = true;
        }
      }
    }
  } catch (...) {
    started = false;
  }
  if (actions_ready) {
    static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
  }
  if (attributes_ready) {
    static_cast<void>(::posix_spawnattr_destroy(&attributes));
  }
  close_descriptor(sockets.front());
  close_descriptor(sockets.back());
  return started;
}

// One bounded read advances header/payload admission; no waiting or partial publication.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto ConfigurationLoader::advance(const std::chrono::steady_clock::time_point now) noexcept
    -> bool {
  if (finished_) {
    return true;
  }
  try {
    bool failed = now >= deadline_;
    if (!failed) {
      const auto count = ::read(descriptor(), std::span(input_).subspan(used_).data(),
                                std::min(target_ - used_, std::size_t{16} * 1024U));
      if (count > 0) {
        used_ += static_cast<std::size_t>(count);
      } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        failed = true;
      }
    }
    if (!failed && used_ >= host_header_bytes) {
      const auto header = std::span(input_).first(host_header_bytes);
      const auto status = static_cast<HostMessageStatus>(
          std::to_integer<std::uint8_t>(header.subspan(6, 1).front()));
      const auto size = (std::to_integer<std::uint32_t>(header.subspan(8, 1).front()) << 24U) |
                        (std::to_integer<std::uint32_t>(header.subspan(9, 1).front()) << 16U) |
                        (std::to_integer<std::uint32_t>(header.subspan(10, 1).front()) << 8U) |
                        std::to_integer<std::uint32_t>(header.subspan(11, 1).front());
      const auto maximum = status == HostMessageStatus::configured
                               ? config::configuration_document_bytes_max
                               : diagnostic_bytes_max;
      failed = !std::ranges::equal(header.first(host_magic.size()), host_magic) ||
               header.subspan(4, 1).front() != std::byte{1} ||
               header.subspan(5, 1).front() != std::byte{0} ||
               header.subspan(7, 1).front() != std::byte{0} || size > maximum ||
               (status != HostMessageStatus::configured && status != HostMessageStatus::failed);
      if (!failed) {
        target_ = host_header_bytes + size;
        if (used_ == target_) {
          // Header validation bounds this borrowed UTF-8 document.
          const auto bytes = std::span(input_).subspan(host_header_bytes, size);
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          const auto* const text = reinterpret_cast<const char*>(bytes.data());
          const std::string_view payload(text, size);
          if (status == HostMessageStatus::configured && decode_configuration(payload, load_)) {
            finished_ = true;
            return true;
          }
          if (status == HostMessageStatus::failed) {
            load_.diagnostic = payload;
          }
          failed = true;
        }
      }
    }
    if (!failed) {
      return false;
    }
    if (load_.diagnostic.empty()) {
      load_.diagnostic = "configuration runtime timed out or exited without a valid result";
    }
  } catch (...) {
    load_.diagnostic.clear();
  }
  load_.status = ConfigurationStatus::invalid;
  load_.host.terminate();
  finished_ = true;
  return true;
}

} // namespace lemma::extension
