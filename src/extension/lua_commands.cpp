#include "extension/lua_commands.hpp"

#include "api/json.hpp"
#include "extension/commands.hpp"
#include "extension/external_command.hpp"
#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <poll.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace frame::extension {
namespace {

[[nodiscard]] auto from_lua(lua_State* state, int index, std::size_t depth, std::size_t& nodes,
                            std::size_t& bytes) -> std::optional<api::JsonValue>;

// The branches validate the closed registration grammar before retaining the callback.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto registration_error(lua_State* const state, LuaCommands& commands) -> const
    char* {
  if (commands.published || commands.descriptors.size() == commands_max) {
    return "command registration is startup-only and bounded to 64 commands";
  }
  if (lua_type(state, 1) != LUA_TSTRING || lua_type(state, 2) != LUA_TTABLE) {
    return "expected frame.command.register(name, { description, timeout_ms, handler })";
  }
  std::size_t size = 0;
  const auto* const text = lua_tolstring(state, 1, &size);
  const std::string_view name(text, size);
  if (!valid_command_name(name) || std::ranges::any_of(commands.descriptors, [&](const auto& item) {
        return item.name == name;
      })) {
    return "command name must be unique and qualified, for example project.open";
  }
  lua_pushnil(state);
  while (lua_next(state, 2) != 0) {
    if (lua_type(state, -2) != LUA_TSTRING) {
      return "invalid command option";
    }
    const auto* const key = lua_tolstring(state, -2, &size);
    const std::string_view option(key, size);
    if (option != "description" && option != "timeout_ms" && option != "handler" &&
        option != "argv") {
      return "unknown command option";
    }
    lua_pop(state, 1);
  }
  lua_getfield(state, 2, "description");
  if (lua_type(state, -1) != LUA_TSTRING) {
    return "command description must be a string";
  }
  const auto* const description_text = lua_tolstring(state, -1, &size);
  const std::string_view description(description_text, size);
  if (size > command_description_bytes_max ||
      !std::ranges::all_of(
          description, [](const unsigned char value) { return value >= 32U && value < 127U; })) {
    return "command description must be at most 256 printable ASCII bytes";
  }
  lua_getfield(state, 2, "timeout_ms");
  const auto timeout =
      lua_isnil(state, -1) != 0 ? command_timeout_default_ms : lua_tointeger(state, -1);
  if ((lua_isnil(state, -1) == 0 && lua_isinteger(state, -1) == 0) || timeout <= 0 ||
      std::cmp_greater(timeout, command_timeout_max_ms)) {
    return "command timeout_ms must be between 1 and 600000";
  }
  lua_getfield(state, 2, "handler");
  const auto handler_type = lua_type(state, -1);
  lua_getfield(state, 2, "argv");
  const auto argv_type = lua_type(state, -1);
  const bool lua_handler = handler_type == LUA_TFUNCTION && argv_type == LUA_TNIL;
  const bool external_program = handler_type == LUA_TNIL && argv_type == LUA_TTABLE;
  if (!lua_handler && !external_program) {
    return "command requires exactly one handler function or argv array";
  }
  std::vector<std::string> program;
  if (argv_type == LUA_TTABLE) {
    std::size_t nodes = 0;
    std::size_t bytes = 0;
    const auto argv = from_lua(state, -1, 0, nodes, bytes);
    if (!argv.has_value() || argv->kind != api::JsonKind::array || argv->array.empty() ||
        argv->array.size() > limits::command_arguments_hard_max ||
        bytes + argv->array.size() > limits::command_bytes_hard_max) {
      return "command argv must be a nonempty bounded string array";
    }
    for (const auto& argument : argv->array) {
      if (argument.kind != api::JsonKind::string || argument.string.contains('\0')) {
        return "command argv must contain strings without NUL bytes";
      }
      program.push_back(argument.string);
    }
    if (program.front().empty()) {
      return "command argv executable must not be empty";
    }
  }
  lua_pop(state, 1);
  // No Lua error is raised while a C++ owning value is live in this helper.
  commands.descriptors.push_back({.name = std::string(name),
                                  .description = std::string(description),
                                  .timeout_ms = static_cast<std::uint32_t>(timeout)});
  const auto index = commands.descriptors.size() - 1U;
  commands.programs.at(index) = std::move(program);
  commands.callbacks.at(index) = luaL_ref(state, LUA_REGISTRYINDEX);
  return nullptr;
}

[[nodiscard]] auto raise_error(lua_State* const state, const char* const message) -> int {
  lua_pushstring(state, message);
  return lua_error(state);
}

[[nodiscard]] auto register_command(lua_State* const state) -> int {
  auto& commands = *static_cast<LuaCommands*>(lua_touserdata(state, lua_upvalueindex(1)));
  const char* error = nullptr;
  try {
    error = registration_error(state, commands);
  } catch (...) {
    error = "command registration allocation failed";
  }
  return error == nullptr ? 0 : raise_error(state, error);
}

// Conversion does not invoke metamethods. Depth, node, and encoded-byte limits also bound cycles.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto from_lua(lua_State* const state, const int index, const std::size_t depth,
                            std::size_t& nodes, std::size_t& bytes)
    -> std::optional<api::JsonValue> {
  if (depth > api::json_depth_max || ++nodes > api::json_nodes_max) {
    return std::nullopt;
  }
  api::JsonValue value;
  switch (lua_type(state, index)) {
  case LUA_TNIL:
    return value;
  case LUA_TBOOLEAN:
    value.kind = api::JsonKind::boolean;
    value.boolean = lua_toboolean(state, index) != 0;
    return value;
  case LUA_TNUMBER:
    if (lua_isinteger(state, index) == 0) {
      return std::nullopt;
    }
    value.kind = api::JsonKind::number;
    value.number = lua_tointeger(state, index);
    return value;
  case LUA_TSTRING: {
    std::size_t size = 0;
    const auto* const text = lua_tolstring(state, index, &size);
    if (size > api::json_bytes_max - bytes) {
      return std::nullopt;
    }
    bytes += size;
    value.kind = api::JsonKind::string;
    value.string.assign(text, size);
    return value;
  }
  case LUA_TTABLE:
    break;
  default:
    return std::nullopt;
  }
  const auto absolute = lua_absindex(state, index);
  const auto count = lua_rawlen(state, absolute);
  if (count > api::json_nodes_max || lua_checkstack(state, 4) == 0) {
    return std::nullopt;
  }
  value.kind = count > 0 ? api::JsonKind::array : api::JsonKind::object;
  std::size_t members = 0;
  lua_pushnil(state);
  while (lua_next(state, absolute) != 0) {
    auto child = from_lua(state, -1, depth + 1U, nodes, bytes);
    if (!child.has_value()) {
      lua_pop(state, 2);
      return std::nullopt;
    }
    if (value.kind == api::JsonKind::array) {
      const auto key = lua_tointeger(state, -2);
      if (lua_isinteger(state, -2) == 0 || key <= 0 || static_cast<std::size_t>(key) > count) {
        lua_pop(state, 2);
        return std::nullopt;
      }
      if (value.array.empty()) {
        value.array.resize(count);
      }
      value.array.at(static_cast<std::size_t>(key) - 1U) = std::move(*child);
    } else {
      if (lua_type(state, -2) != LUA_TSTRING) {
        lua_pop(state, 2);
        return std::nullopt;
      }
      std::size_t size = 0;
      const auto* const key = lua_tolstring(state, -2, &size);
      if (size > api::json_bytes_max - bytes) {
        lua_pop(state, 2);
        return std::nullopt;
      }
      bytes += size;
      value.object.push_back({.key = std::string(key, size), .value = std::move(*child)});
    }
    ++members;
    lua_pop(state, 1);
  }
  return value.kind == api::JsonKind::array && members != count ? std::nullopt
                                                                : std::optional{std::move(value)};
}

void to_lua(lua_State* const state, const api::JsonValue& value) {
  if (lua_checkstack(state, 4) == 0) {
    static_cast<void>(raise_error(state, "extension JSON stack exhausted"));
    return;
  }
  switch (value.kind) {
  case api::JsonKind::null:
    lua_pushnil(state);
    break;
  case api::JsonKind::boolean:
    lua_pushboolean(state, value.boolean ? 1 : 0);
    break;
  case api::JsonKind::number:
    lua_pushinteger(state, value.number);
    break;
  case api::JsonKind::string:
    lua_pushlstring(state, value.string.data(), value.string.size());
    break;
  case api::JsonKind::array:
    lua_createtable(state, static_cast<int>(value.array.size()), 0);
    for (std::size_t index = 0; index < value.array.size(); ++index) {
      to_lua(state, value.array.at(index));
      lua_rawseti(state, -2, static_cast<lua_Integer>(index) + 1);
    }
    break;
  case api::JsonKind::object:
    lua_createtable(state, 0, static_cast<int>(value.object.size()));
    for (const auto& member : value.object) {
      lua_pushlstring(state, member.key.data(), member.key.size());
      to_lua(state, member.value);
      lua_rawset(state, -3);
    }
    break;
  }
}

struct Callback final {
  lua_State* thread{nullptr};
  std::uint64_t invocation{0};
  int reference{LUA_NOREF};
  std::optional<ExternalCommand> external;
};

void release_callback(lua_State* const state, Callback& callback) noexcept {
  // Do not run untrusted __close handlers while cancelling a coroutine. Releasing the registry
  // reference lets GC reclaim it; the daemon has already cancelled its outstanding Proc.
  luaL_unref(state, LUA_REGISTRYINDEX, callback.reference);
  callback = {};
}

void instruction_limit(lua_State* const state, [[maybe_unused]] lua_Debug* const debug) {
  static_cast<void>(raise_error(state, "extension callback instruction budget exceeded"));
}

// Resume, yield conversion, and completion are the three bounded coroutine outcomes.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto resume_callback(lua_State* const state, Callback& callback, const int arguments,
                                   CommandChannel& channel) -> bool {
  lua_sethook(callback.thread, &instruction_limit, LUA_MASKCOUNT, 1'000'000);
  int results = 0;
  const auto status = lua_resume(callback.thread, state, arguments, &results);
  if (status == LUA_YIELD && results == 1) {
    std::size_t nodes = 0;
    std::size_t bytes = 0;
    auto document = from_lua(callback.thread, -1, 0, nodes, bytes);
    std::string payload;
    if (document.has_value() && api::append_json_value(payload, *document)) {
      lua_settop(callback.thread, 0);
      return channel.send("proc", callback.invocation, payload);
    }
  }
  std::string payload = status == LUA_OK ? R"({"ok":true})" : R"({"ok":false,"error":)";
  if (status != LUA_OK) {
    std::size_t size = 0;
    const auto* const text = lua_type(callback.thread, -1) == LUA_TSTRING
                                 ? lua_tolstring(callback.thread, -1, &size)
                                 : nullptr;
    std::string diagnostic = text == nullptr ? "invalid extension yield"
                                             : std::string(text, std::min(size, std::size_t{180}));
    for (char& character : diagnostic) {
      if (static_cast<unsigned char>(character) < 32U ||
          static_cast<unsigned char>(character) >= 127U) {
        character = '?';
      }
    }
    if (!api::append_json_string(payload, diagnostic)) {
      return false;
    }
    payload += '}';
  }
  const auto sent = channel.send("complete", callback.invocation, payload);
  release_callback(state, callback);
  return sent;
}

[[nodiscard]] auto start_external_callback(lua_State* const state, Callback& callback,
                                           std::vector<std::string> arguments,
                                           const CommandMessage& message, CommandChannel& channel)
    -> bool {
  const auto* const args = api::json_member(message.payload, "args");
  if (args == nullptr || args->kind != api::JsonKind::array) {
    return false;
  }
  for (const auto& argument : args->array) {
    if (argument.kind != api::JsonKind::string) {
      return false;
    }
    arguments.push_back(argument.string);
  }
  std::string context;
  if (!api::append_json_value(context, message.payload)) {
    return false;
  }
  callback.external.emplace();
  callback.invocation = message.invocation;
  if (!callback.external->start(arguments, context)) {
    release_callback(state, callback);
    return channel.send("complete", message.invocation,
                        R"({"ok":false,"error":"external command launch failed"})");
  }
  return true;
}

[[nodiscard]] auto handle_message(lua_State* const state, LuaCommands& commands,
                                  std::array<Callback, invocations_max>& callbacks,
                                  const CommandMessage& message, CommandChannel& channel) -> bool {
  auto* slot = std::ranges::find_if(
      callbacks, [&](const auto& callback) { return callback.invocation == message.invocation; });
  if (message.kind == "cancel") {
    if (slot == callbacks.end()) {
      return channel.send("complete", message.invocation, R"({"ok":true})");
    }
    if (slot->external.has_value()) {
      slot->external->cancel();
      return true; // Retain invocation capacity until the child has actually exited.
    }
    release_callback(state, *slot);
    return channel.send("complete", message.invocation, R"({"ok":true})");
  }
  if (message.kind == "result") {
    if (slot == callbacks.end()) {
      return true;
    }
    if (slot->thread == nullptr) {
      return false;
    }
    to_lua(slot->thread, message.payload);
    return resume_callback(state, *slot, 1, channel);
  }
  if (message.kind != "invoke" || slot != callbacks.end()) {
    return false;
  }
  const auto name = api::json_string(message.payload, "command");
  const auto command = std::ranges::find_if(commands.descriptors, [&](const auto& descriptor) {
    return name == std::optional<std::string_view>{descriptor.name};
  });
  slot = std::ranges::find_if(callbacks,
                              [](const auto& callback) { return callback.invocation == 0; });
  if (command == commands.descriptors.end() || slot == callbacks.end()) {
    return false;
  }
  const auto index = static_cast<std::size_t>(command - commands.descriptors.begin());
  if (!commands.programs.at(index).empty()) {
    return start_external_callback(state, *slot, commands.programs.at(index), message, channel);
  }
  slot->thread = lua_newthread(state);
  slot->reference = luaL_ref(state, LUA_REGISTRYINDEX);
  slot->invocation = message.invocation;
  lua_rawgeti(slot->thread, LUA_REGISTRYINDEX, commands.invocation_wrapper);
  lua_rawgeti(
      slot->thread, LUA_REGISTRYINDEX,
      commands.callbacks.at(static_cast<std::size_t>(command - commands.descriptors.begin())));
  to_lua(slot->thread, message.payload);
  return resume_callback(state, *slot, 2, channel);
}
} // namespace

void install_commands(lua_State* const state, LuaCommands& commands) {
  lua_createtable(state, 0, 1);
  lua_pushlightuserdata(state, &commands);
  lua_pushcclosure(state, &register_command, 1);
  lua_setfield(state, -2, "register");
  lua_setfield(state, -2, "command");
  constexpr auto wrapper = R"lua(
local yield, type, error = coroutine.yield, type, error
return function(handler, payload)
  local ctx = { session = payload.session, tab = payload.tab, pane = payload.pane,
                connection = payload.connection, endpoint = payload.endpoint }
  function ctx:proc(document)
    if type(document) ~= "table" then error("ctx:proc requires a Proc table") end
    if document.schema == nil then document.schema = "frame.proc/v1" end
    return yield(document)
  end
  handler(ctx, payload.args)
end
)lua";
  if (luaL_loadstring(state, wrapper) != LUA_OK || lua_pcall(state, 0, 1, 0) != LUA_OK) {
    lua_error(state);
    return;
  }
  commands.invocation_wrapper = luaL_ref(state, LUA_REGISTRYINDEX);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto run_commands(lua_State* const state, LuaCommands& commands, const int descriptor) noexcept
    -> int {
  commands.published = true;
  CommandChannel channel(descriptor);
  std::array<Callback, invocations_max> callbacks{};
  try {
    while (channel.descriptor() >= 0) {
      std::array<pollfd, invocations_max + 1U> descriptors{};
      descriptors.front() = {.fd = channel.descriptor(), .events = channel.events(), .revents = 0};
      int timeout = -1;
      for (std::size_t index = 0; index < callbacks.size(); ++index) {
        const auto& external = callbacks.at(index).external;
        descriptors.at(index + 1U) = {.fd = external.has_value() ? external->descriptor() : -1,
                                      .events = POLLIN,
                                      .revents = 0};
        if (external.has_value() && external->reaping()) {
          timeout = 20;
        }
      }
      const auto polled =
          ::poll(descriptors.data(), descriptors.size(), channel.buffered() ? 0 : timeout);
      const auto ready = descriptors.front();
      if (polled < 0 && errno == EINTR) {
        continue;
      }
      if (polled < 0 || (ready.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0) {
        return 1;
      }
      if ((ready.revents & POLLIN) != 0) {
        channel.read_ready();
      }
      if (auto message = channel.receive();
          message.has_value() && !handle_message(state, commands, callbacks, *message, channel)) {
        return 1;
      }
      for (std::size_t index = 0; index < callbacks.size(); ++index) {
        auto& callback = callbacks.at(index);
        if (!callback.external.has_value()) {
          continue;
        }
        if (const auto result = callback.external->service(descriptors.at(index + 1U).revents);
            result.has_value()) {
          if (!channel.send("complete", callback.invocation, *result)) {
            return 1;
          }
          release_callback(state, callback);
        }
      }
      if ((ready.revents & POLLOUT) != 0) {
        channel.write_ready();
      }
    }
  } catch (...) {
    return 1;
  }
  return 0;
}

} // namespace frame::extension
