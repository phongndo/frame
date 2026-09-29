#ifndef FRAME_EXTENSION_LUA_COMMANDS_HPP
#define FRAME_EXTENSION_LUA_COMMANDS_HPP

#include "extension/commands.hpp"

#include <array>
#include <string>
#include <vector>

struct lua_State;

namespace frame::extension {

struct LuaCommands final {
  std::vector<CommandDescriptor> descriptors;
  std::array<int, commands_max> callbacks{};
  std::array<std::vector<std::string>, commands_max> programs{};
  int invocation_wrapper{0};
  bool published{false};
};

// Installs frame.command into the module table at the top of the Lua stack.
void install_commands(lua_State* state, LuaCommands& commands);
[[nodiscard]] auto run_commands(lua_State* state, LuaCommands& commands, int descriptor) noexcept
    -> int;

} // namespace frame::extension

#endif // FRAME_EXTENSION_LUA_COMMANDS_HPP
