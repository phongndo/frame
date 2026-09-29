#ifndef FRAME_EXTENSION_COMMANDS_HPP
#define FRAME_EXTENSION_COMMANDS_HPP

#include "api/json.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frame::extension {

inline constexpr std::size_t commands_max = limits::hosted_commands_hard_max;
inline constexpr std::size_t invocations_max = 8;
inline constexpr std::size_t command_name_bytes_max = 64;
inline constexpr std::size_t command_description_bytes_max = 256;
inline constexpr std::uint32_t command_timeout_default_ms = 30'000;
inline constexpr std::uint32_t command_timeout_max_ms = 600'000;

struct CommandDescriptor final {
  std::string name;
  std::string description;
  std::uint32_t timeout_ms{command_timeout_default_ms};
};

// Qualified names cannot shadow the native command roots. Names are resolved only on invocation.
[[nodiscard]] auto valid_command_name(std::string_view name) noexcept -> bool;
[[nodiscard]] auto encode_registration(std::string_view configuration,
                                       std::span<const CommandDescriptor> commands)
    -> std::optional<std::string>;
[[nodiscard]] auto decode_commands(const api::JsonValue& value)
    -> std::optional<std::vector<CommandDescriptor>>;

struct CommandMessage final {
  std::string kind;
  std::uint64_t invocation{0};
  api::JsonValue payload;
};

// Borrowed descriptor. One bounded read, write, and decode per service call; partial writes retain
// their exact suffix. This native channel is also used by the isolated Lua host.
class CommandChannel final {
public:
  explicit CommandChannel(int descriptor = -1) noexcept;
  [[nodiscard]] auto descriptor() const noexcept -> int { return descriptor_; }
  [[nodiscard]] auto events() const noexcept -> short;
  [[nodiscard]] auto buffered() const noexcept -> bool;
  void read_ready() noexcept;
  void write_ready() noexcept;
  void disconnect() noexcept;
  [[nodiscard]] auto receive() noexcept -> std::optional<CommandMessage>;
  [[nodiscard]] auto send(std::string_view kind, std::uint64_t invocation,
                          std::string_view json_payload) noexcept -> bool;

private:
  int descriptor_{-1};
  std::string input_;
  // Cached framing position: incomplete records scan only newly received bytes.
  std::size_t newline_{std::string::npos};
  std::string output_;
  std::size_t written_{0};
};

struct InvocationContext final {
  SessionId session;
  TabId tab;
  PaneId pane;
  ConnectionId connection;
};

enum class InvocationPhase : std::uint8_t { running, waiting_proc, cancelling };

struct Invocation final {
  std::uint64_t id{0};
  InvocationContext context;
  std::chrono::steady_clock::time_point deadline;
  InvocationPhase phase{InvocationPhase::running};
};

using StopHost = void (*)(void*) noexcept;

// Native reactor-owned invocation authority. The descriptor, declarations and stop callback are
// borrowed from the daemon's host owner and outlive this value. There is no Lua dependency.
class CommandRuntime final {
public:
  CommandRuntime(int descriptor, std::span<const CommandDescriptor> commands, StopHost stop,
                 void* context, int listener = -1) noexcept;
  [[nodiscard]] auto channel() noexcept -> CommandChannel& { return channel_; }
  [[nodiscard]] auto commands() const noexcept -> std::span<const CommandDescriptor>;
  [[nodiscard]] auto invocations() const noexcept -> std::span<const Invocation> { return slots_; }
  [[nodiscard]] auto find(std::uint64_t id) noexcept -> Invocation*;
  [[nodiscard]] auto start(std::string_view command, std::span<const std::string> arguments,
                           InvocationContext context,
                           std::chrono::steady_clock::time_point now) noexcept -> bool;
  void cancel(std::uint64_t id) noexcept;
  void finish(std::uint64_t id) noexcept;
  void fail() noexcept;
  [[nodiscard]] auto result(std::uint64_t id, std::string_view json) noexcept -> bool;
  [[nodiscard]] auto poll_timeout(int current,
                                  std::chrono::steady_clock::time_point now) const noexcept -> int;

private:
  CommandChannel channel_;
  std::span<const CommandDescriptor> commands_;
  std::array<Invocation, invocations_max> slots_{};
  std::uint64_t next_id_{0};
  StopHost stop_;
  void* stop_context_;
  int listener_;
};

} // namespace frame::extension

#endif // FRAME_EXTENSION_COMMANDS_HPP
