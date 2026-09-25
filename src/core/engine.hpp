#ifndef LEMMA_CORE_ENGINE_HPP
#define LEMMA_CORE_ENGINE_HPP

#include "extension/commands.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <poll.h>

namespace lemma::input {
class CompiledInputMap;
}
namespace lemma::config {
class Generation;
}

namespace lemma::core {

using EndpointRelease = void (*)(void* context) noexcept;

struct ChildExit final {
  int process{-1};
  int status{0};
};

using StopRequested = bool (*)() noexcept;
using ReapChild = std::optional<ChildExit> (*)(void* context) noexcept;

struct ChildReaper final {
  int wake_descriptor{-1};
  ReapChild reap{nullptr};
  void* context{nullptr};

  [[nodiscard]] constexpr auto valid() const noexcept -> bool {
    return wake_descriptor >= 0 && reap != nullptr;
  }
};

using ReactorClock = std::chrono::steady_clock;

struct ReactorIoResult final {
  std::ptrdiff_t bytes{0};
  int error{0};
};

using ReactorPoll = int (*)(void* context, std::span<pollfd> descriptors,
                            int timeout_milliseconds) noexcept;
using ReactorNow = ReactorClock::time_point (*)(void* context) noexcept;
using ReactorSend = ReactorIoResult (*)(void* context, int descriptor,
                                        std::span<const std::byte> bytes, int flags) noexcept;

enum class ConfigurationReloadError : std::uint8_t { invalid_configuration, restart_required };

struct ConfigurationCandidate final {
  const config::Generation* generation{nullptr};
  std::span<const extension::CommandDescriptor> commands;
  int descriptor{-1};
  extension::StopHost stop{nullptr};
  void* context{nullptr};
  ConfigurationReloadError error{ConfigurationReloadError::invalid_configuration};
  std::string_view diagnostic;
};

// Daemon-owned cold path. advance never waits: it consumes one bounded input quantum. A failed
// candidate has a null generation. commit/discard must not block on child cleanup.
class ConfigurationReloader {
public:
  ConfigurationReloader() = default;
  ConfigurationReloader(const ConfigurationReloader&) = delete;
  auto operator=(const ConfigurationReloader&) -> ConfigurationReloader& = delete;
  ConfigurationReloader(ConfigurationReloader&&) = delete;
  auto operator=(ConfigurationReloader&&) -> ConfigurationReloader& = delete;
  virtual ~ConfigurationReloader() = default;
  [[nodiscard]] virtual auto start() noexcept -> bool = 0;
  [[nodiscard]] virtual auto descriptor() const noexcept -> int = 0;
  [[nodiscard]] virtual auto deadline() const noexcept -> ReactorClock::time_point = 0;
  [[nodiscard]] virtual auto advance(ReactorClock::time_point now) noexcept
      -> std::optional<ConfigurationCandidate> = 0;
  virtual void commit() noexcept = 0;
  virtual void discard() noexcept = 0;
};

// Production uses native level-triggered readiness (poll fallback), send(2), and steady_clock.
// Deterministic tests may instead provide one scripted readiness/I/O/clock authority. The
// callbacks are turn-local and must not retain borrowed spans.
struct ReactorEnvironment final {
  void* context{nullptr};
  ReactorPoll poll{nullptr};
  ReactorNow now{nullptr};
  ReactorSend send{nullptr};
  // One immutable configuration generation. A reload replaces these views at a reactor turn;
  // the daemon retains both generations until native borrowers have switched.
  const input::CompiledInputMap* input_map{nullptr};
  std::optional<std::size_t> scrollback_lines;
  std::span<const std::byte> default_program;
  std::string_view default_cwd;
  std::string_view command_history_file;
  bool status_line{true};
  bool outer_title{true};
  bool outer_notifications{true};
  bool outer_progress{true};
  bool outer_cwd{true};
  bool clipboard_read{false};
  bool clipboard_write{false};
  int extension_descriptor{-1};
  std::span<const extension::CommandDescriptor> extension_commands;
  extension::StopHost stop_extension{nullptr};
  void* extension_context{nullptr};
  ConfigurationReloader* configuration_reloader{nullptr};

  [[nodiscard]] constexpr auto valid() const noexcept -> bool {
    return poll != nullptr && now != nullptr && send != nullptr;
  }
};

[[nodiscard]] auto production_reactor_environment() noexcept -> ReactorEnvironment;

// Runs the same production reactor with injected readiness, outbound-I/O, and monotonic-time
// sources. This is a narrow deterministic-test seam: descriptor ownership, command execution,
// inbound I/O, and all reactor state remain production code.
[[nodiscard]] auto run_server_with_environment(int listener, EndpointRelease release_endpoint,
                                               void* release_context, StopRequested stop_requested,
                                               ChildReaper child_reaper,
                                               ReactorEnvironment environment) noexcept -> int;

// Runs the authoritative bounded reactor for every session. The engine invokes release_endpoint
// exactly once after it stops using the borrowed listener; the daemon retains ownership of the
// listener and its filesystem lifecycle. Child process exits must have a pollable wake source so a
// signal observed immediately before poll cannot strand an exited child.
[[nodiscard]] auto run_server(int listener, EndpointRelease release_endpoint, void* release_context,
                              StopRequested stop_requested, ChildReaper child_reaper) noexcept
    -> int;

} // namespace lemma::core

#endif // LEMMA_CORE_ENGINE_HPP
