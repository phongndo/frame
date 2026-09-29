#include "extension/services.hpp"

#include "config/config.hpp"
#include "extension/lua_host.hpp"
#include "platform/io.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace frame::extension {
auto bundled_ui_path() -> std::string {
  std::array<char, 4096> path{};
  const auto size = platform::executable_path(path);
  if (size == 0) {
    throw std::runtime_error("cannot resolve executable path for bundled UI");
  }
  const std::string executable(path.data(), size);
  return executable.substr(0, executable.find_last_of('/') + 1U) + "frame-ui";
}

Services::Services(const std::span<const config::ExtensionConfiguration> configuration,
                   std::string endpoint)
    : endpoint_(std::move(endpoint)) {
  services_.reserve(configuration.size());
  for (const auto& value : configuration) {
    services_.push_back({.configuration = value, .process = {}, .started = {}, .restarts = 0});
    start(services_.back());
  }
}

void Services::start(Service& service) noexcept {
  try {
    std::vector<char*> arguments;
    arguments.reserve(service.configuration.argv.size() + 1U);
    for (auto& argument : service.configuration.argv) {
      arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);
    const auto child = ::fork();
    if (child == 0) {
      if (::setpgid(0, 0) != 0) {
        ::_exit(127);
      }
      // The helper owns no daemon descriptor; exec closes all CLOEXEC descriptors.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      const auto null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
      if (null < 0 || ::dup2(null, STDIN_FILENO) < 0 || ::dup2(null, STDOUT_FILENO) < 0 ||
          ::setenv("FRAME_EXTENSION_ENDPOINT", endpoint_.c_str(), 1) != 0) {
        ::_exit(127);
      }
      if (null > STDERR_FILENO) {
        static_cast<void>(::close(null));
      }
      static_cast<void>(::signal(SIGCHLD, SIG_DFL));
      static_cast<void>(::signal(SIGTERM, SIG_DFL));
      ::execvp(arguments.front(), arguments.data());
      ::_exit(127);
    }
    if (child > 0) {
      static_cast<void>(::setpgid(child, child));
      service.process = HostProcess(-1, static_cast<int>(child));
      service.started = std::chrono::steady_clock::now();
    }
  } catch (...) {
    service.restarts = 3; // Resource failure leaves this optional program stopped.
  }
}

void Services::reap_exited() noexcept {
  for (auto& service : services_) {
    if (!service.process.active()) {
      continue;
    }
    service.process.reap_exited();
    if (service.process.active()) {
      continue;
    }
    // Revoke the whole old group before reusing its identity. Cap a crash loop at three
    // restarts; a minute of successful operation replenishes the budget without a timer.
    if (std::chrono::steady_clock::now() - service.started >= std::chrono::minutes(1)) {
      service.restarts = 0;
    }
    if (service.restarts < 3U) {
      ++service.restarts;
      start(service);
    }
  }
}
} // namespace frame::extension
