#ifndef FRAME_EXTENSION_LUA_HOST_HPP
#define FRAME_EXTENSION_LUA_HOST_HPP

#include "config/config.hpp"
#include "extension/commands.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frame::extension {

enum class ConfigurationStatus : std::uint8_t {
  absent,
  loaded,
  invalid,
};

class HostProcess final {
public:
  HostProcess() noexcept = default;
  HostProcess(const HostProcess&) = delete;
  auto operator=(const HostProcess&) -> HostProcess& = delete;
  HostProcess(HostProcess&& other) noexcept;
  auto operator=(HostProcess&& other) noexcept -> HostProcess&;
  ~HostProcess();

  [[nodiscard]] auto active() const noexcept -> bool { return process_ > 0; }
  [[nodiscard]] auto descriptor() const noexcept -> int { return descriptor_; }
  // Nonblocking reactor failure path. Reaping remains with the daemon or destructor.
  void terminate() noexcept;
  // Must precede the daemon's general waitpid(-1): revoke descendants while the exited host's
  // unreaped PID still protects its process-group identity, then release that PID.
  void reap_exited() noexcept;

  // Process creation is internal to the extension runtime; this value constructor only transfers
  // already-created descriptor and process ownership.
  HostProcess(int descriptor, int process) noexcept : descriptor_(descriptor), process_(process) {}

private:
  void reset() noexcept;

  int descriptor_{-1};
  int process_{-1};
};

struct ConfigurationLoad final {
  HostProcess host;
  std::unique_ptr<const config::Generation> generation;
  std::vector<CommandDescriptor> commands;
  std::string path;
  std::string diagnostic;
  ConfigurationStatus status{ConfigurationStatus::absent};
};

// An omitted path discovers $XDG_CONFIG_HOME/frame/init.lua (or ~/.config/frame/init.lua).
// A discovered missing file is not an error; an explicit missing path is.
[[nodiscard]] auto load_configuration(std::optional<std::string_view> path = std::nullopt) noexcept
    -> ConfigurationLoad;

[[nodiscard]] auto load_builtin_configuration() noexcept -> ConfigurationLoad;

// The private executable starts with only its configuration channel at descriptor 3. Lua and
// filesystem access run outside the reactor; admission reads at most one 16 KiB quantum per turn.
[[nodiscard]] auto run_configuration_host(std::string_view requested, bool required) noexcept
    -> int;

class ConfigurationLoader final {
public:
  [[nodiscard]] auto start() noexcept -> bool;
  [[nodiscard]] auto descriptor() const noexcept -> int { return load_.host.descriptor(); }
  [[nodiscard]] auto deadline() const noexcept -> std::chrono::steady_clock::time_point {
    return deadline_;
  }
  [[nodiscard]] auto advance(std::chrono::steady_clock::time_point now) noexcept -> bool;
  [[nodiscard]] auto result() const noexcept -> const ConfigurationLoad& { return load_; }
  [[nodiscard]] auto host() noexcept -> HostProcess& { return load_.host; }
  [[nodiscard]] auto take() noexcept -> ConfigurationLoad { return std::move(load_); }

private:
  ConfigurationLoad load_;
  std::vector<std::byte> input_;
  std::chrono::steady_clock::time_point deadline_;
  std::size_t used_{0};
  std::size_t target_{12};
  bool finished_{false};
};

} // namespace frame::extension

#endif // FRAME_EXTENSION_LUA_HOST_HPP
