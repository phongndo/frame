#ifndef FRAME_EXTENSION_SERVICES_HPP
#define FRAME_EXTENSION_SERVICES_HPP

#include "config/config.hpp"
#include "extension/lua_host.hpp"

#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace frame::extension {

[[nodiscard]] auto bundled_ui_path() -> std::string;

// Owns bounded daemon-lifetime programs. Only startup and child-exit handling execute here;
// ordinary input, PTY output and rendering never enter this module.
class Services final {
public:
  Services(std::span<const config::ExtensionConfiguration> configuration, std::string endpoint);
  void reap_exited() noexcept;

private:
  struct Service final {
    config::ExtensionConfiguration configuration;
    HostProcess process;
    std::chrono::steady_clock::time_point started;
    unsigned restarts{0};
  };
  void start(Service& service) noexcept;
  std::vector<Service> services_;
  std::string endpoint_;
};

} // namespace frame::extension
#endif
