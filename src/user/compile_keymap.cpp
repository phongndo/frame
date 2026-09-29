#include "config/config.hpp"
#include "input/input_router.hpp"

#include <cstdio>
#include <exception>
#include <fstream>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace {
[[nodiscard]] auto quote(const std::string_view text) -> std::string {
  std::string result{"\""};
  for (const auto byte : text) {
    if (byte == '\\' || byte == '"') {
      result += '\\';
    }
    result += byte;
  }
  return result + '"';
}
[[nodiscard]] auto chord(const char* text) -> std::string {
  const auto value = frame::config::parse_key(text);
  if (!value.has_value()) {
    throw std::runtime_error("invalid default key");
  }
  return "InputChord{.code=" + std::to_string(value->code) +
         ",.modifiers=" + std::to_string(value->modifiers) +
         ",.kind=ChordKind::" + (value->kind == frame::input::ChordKind::byte ? "byte}" : "key}");
}
[[nodiscard]] auto output(lua_State* state) -> std::string& {
  return *static_cast<std::string*>(lua_touserdata(state, lua_upvalueindex(1)));
}
[[nodiscard]] auto binding(lua_State* state) -> int {
  const auto context = std::string{"ConfiguredInputContext::"} + luaL_checkstring(state, 1);
  const auto key = chord(luaL_checkstring(state, 2));
  const auto action = std::string{luaL_checkstring(state, 3)};
  std::string call;
  if (action == "push") {
    call = "push(" + context + ',' + key +
           ",ConfiguredInputContext::" + luaL_checkstring(state, 4) + ')';
  } else if (action == "pop") {
    call = "pop(" + context + ',' + key + ')';
  } else if (action == "send") {
    const auto sent = chord(luaL_checkstring(state, 4));
    call = "send(" + context + ',' + key + ",static_cast<PhysicalKey>((" + sent + ").code),(" +
           sent + ").modifiers)";
  } else {
    call = "set(" + context + ',' + key + ",InputCommand::" + action +
           ",CommandContextDisposition::" + luaL_optstring(state, 4, "retain") + ')';
  }
  output(state) += "  FRAME_ASSERT(" + call + ");\n";
  return 0;
}
[[nodiscard]] auto context(lua_State* state) -> int {
  output(state) +=
      "  FRAME_ASSERT(set_context(ConfiguredInputContext::" +
      std::string(luaL_checkstring(state, 1)) + ",{.label=" + quote(luaL_checkstring(state, 2)) +
      ",.lifetime=ContextLifetime::" + luaL_checkstring(state, 3) +
      ",.unbound=UnboundBehavior::" + luaL_checkstring(state, 4) +
      ",.preempts_interaction=" + (lua_toboolean(state, 5) != 0 ? "true" : "false") + "}));\n";
  return 0;
}
[[nodiscard]] auto prefix(lua_State* state) -> int {
  output(state) += "  FRAME_ASSERT(set_prefix(" + chord(luaL_checkstring(state, 1)) + "));\n";
  return 0;
}
void install(lua_State* state, const char* name, lua_CFunction function, std::string& target) {
  lua_pushlightuserdata(state, &target);
  lua_pushcclosure(state, function, 1);
  lua_setglobal(state, name);
}
} // namespace

int main(int argc, char** argv) {
  try {
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    if (argc != 3) {
      return 2;
    }
    lua_State* state = luaL_newstate();
    if (state == nullptr) {
      return 1;
    }
    luaL_openlibs(state);
    std::string contexts;
    std::string bindings;
    install(state, "context", context, contexts);
    install(state, "bind", binding, bindings);
    install(state, "prefix", prefix, bindings);
    if (luaL_dofile(state, arguments.subspan(1, 1).front()) != LUA_OK) {
      std::println(stderr, "{}", lua_tostring(state, -1));
      lua_close(state);
      return 1;
    }
    lua_close(state);
    std::ofstream file(arguments.back());
    file << "// Generated from src/user/keymap.lua. Do not edit.\n"
         << contexts << "  if (selected == InputMapPreset::none) { return; }\n"
         << bindings;
    return file.good() ? 0 : 1;
  } catch (const std::exception&) {
    return 1;
  }
}
