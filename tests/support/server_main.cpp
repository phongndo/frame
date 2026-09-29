#include "daemon/server.hpp"

#include <csignal>
#include <span>
#include <string_view>

namespace {

volatile sig_atomic_t stop_requested = 0;

void request_stop([[maybe_unused]] const int signal_number) noexcept { stop_requested = 1; }

[[nodiscard]] auto should_stop() noexcept -> bool { return stop_requested != 0; }

} // namespace

int main(const int argc, char** argv) {
  const std::span arguments(argv, static_cast<std::size_t>(argc));
  if (arguments.size() != 2) {
    return 2;
  }
  const auto endpoint = frame::daemon::RuntimeEndpoint::create(std::string_view(arguments.back()));
  if (!endpoint.has_value()) {
    return 2;
  }
  struct sigaction action{};
  action.sa_handler = &request_stop;
  if (sigemptyset(&action.sa_mask) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0) {
    return 2;
  }
  return frame::daemon::serve(*endpoint, {.stop_requested = &should_stop});
}
