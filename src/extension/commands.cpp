#include "extension/commands.hpp"

#include "api/json.hpp"
#include "config/config.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace frame::extension {
namespace {
constexpr std::size_t io_bytes_per_turn = std::size_t{16} * 1'024U;
constexpr std::size_t queued_bytes_max = 2U * api::json_bytes_max;

[[nodiscard]] auto printable(const std::string_view text) noexcept -> bool {
  return std::ranges::all_of(
      text, [](const unsigned char value) { return value >= 32U && value < 127U; });
}

template <typename Id> [[nodiscard]] auto id_text(const Id id) -> std::string {
  return std::to_string(id.slot()) + ":" + std::to_string(id.generation());
}
} // namespace

auto valid_command_name(const std::string_view name) noexcept -> bool {
  if (name.empty() || name.size() > command_name_bytes_max || !name.contains('.')) {
    return false;
  }
  bool segment_start = true;
  for (const char character : name) {
    if (character >= 'a' && character <= 'z') {
      segment_start = false;
    } else if (!segment_start && character == '.') {
      segment_start = true;
    } else if (segment_start ||
               ((character < '0' || character > '9') && character != '_' && character != '-')) {
      return false;
    }
  }
  return !segment_start;
}

auto encode_registration(const std::string_view configuration,
                         const std::span<const CommandDescriptor> commands)
    -> std::optional<std::string> {
  std::string output = R"({"configuration":)";
  output += configuration;
  output += R"(,"commands":[)";
  for (const auto& command : commands) {
    if (&command != commands.data()) {
      output += ',';
    }
    output += R"({"name":)";
    if (!api::append_json_string(output, command.name, config::configuration_document_bytes_max)) {
      return std::nullopt;
    }
    output += R"(,"description":)";
    if (!api::append_json_string(output, command.description,
                                 config::configuration_document_bytes_max)) {
      return std::nullopt;
    }
    output += R"(,"timeout_ms":)" + std::to_string(command.timeout_ms) + '}';
  }
  output += "]}";
  return output.size() <= config::configuration_document_bytes_max
             ? std::optional{std::move(output)}
             : std::nullopt;
}

auto decode_commands(const api::JsonValue& value) -> std::optional<std::vector<CommandDescriptor>> {
  if (value.kind != api::JsonKind::array || value.array.size() > commands_max) {
    return std::nullopt;
  }
  std::vector<CommandDescriptor> result;
  for (const auto& item : value.array) {
    const auto name = api::json_string(item, "name");
    const auto description = api::json_string(item, "description");
    const auto timeout = api::json_unsigned(item, "timeout_ms");
    if (item.kind != api::JsonKind::object || item.object.size() != 3U || !name.has_value() ||
        !valid_command_name(*name) || !description.has_value() ||
        description->size() > command_description_bytes_max || !printable(*description) ||
        !timeout.has_value() || *timeout == 0 || *timeout > command_timeout_max_ms ||
        std::ranges::any_of(result, [&](const auto& command) { return command.name == *name; })) {
      return std::nullopt;
    }
    result.push_back({.name = std::string(*name),
                      .description = std::string(*description),
                      .timeout_ms = static_cast<std::uint32_t>(*timeout)});
  }
  return result;
}

CommandChannel::CommandChannel(const int descriptor) noexcept : descriptor_(descriptor) {
  if (descriptor_ >= 0) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const auto flags = ::fcntl(descriptor_, F_GETFL, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (flags < 0 || ::fcntl(descriptor_, F_SETFL, flags | O_NONBLOCK) != 0) {
      disconnect();
    }
  }
}

auto CommandChannel::events() const noexcept -> short {
  return static_cast<short>(POLLIN | (written_ < output_.size() ? POLLOUT : 0));
}

auto CommandChannel::buffered() const noexcept -> bool { return newline_ != std::string::npos; }

void CommandChannel::disconnect() noexcept {
  if (descriptor_ >= 0) {
    static_cast<void>(::shutdown(descriptor_, SHUT_RDWR));
    descriptor_ = -1;
  }
  input_.clear();
  newline_ = std::string::npos;
  output_.clear();
  written_ = 0;
}

void CommandChannel::read_ready() noexcept {
  if (descriptor_ < 0 || buffered()) {
    return;
  }
  std::array<char, io_bytes_per_turn> buffer{};
  const auto received = ::recv(descriptor_, buffer.data(), buffer.size(), 0);
  if (received > 0) {
    const auto size = static_cast<std::size_t>(received);
    try {
      if (size > api::json_bytes_max + io_bytes_per_turn - input_.size()) {
        disconnect();
        return;
      }
      const auto begin = input_.size();
      input_.append(buffer.data(), size);
      newline_ = input_.find('\n', begin);
      if (!buffered() && input_.size() >= api::json_bytes_max) {
        disconnect();
      }
    } catch (...) {
      disconnect();
    }
  } else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
    disconnect();
  }
}

void CommandChannel::write_ready() noexcept {
  if (descriptor_ < 0 || written_ == output_.size()) {
    return;
  }
  const auto remaining = std::span(output_).subspan(written_);
  const auto sent = ::send(descriptor_, remaining.data(),
                           std::min(remaining.size(), io_bytes_per_turn), MSG_NOSIGNAL);
  if (sent > 0) {
    written_ += static_cast<std::size_t>(sent);
    if (written_ == output_.size()) {
      output_.clear();
      written_ = 0;
    }
  } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
    disconnect();
  }
}

auto CommandChannel::receive() noexcept -> std::optional<CommandMessage> {
  const auto newline = newline_;
  if (newline == std::string::npos) {
    return std::nullopt;
  }
  try {
    if (newline >= api::json_bytes_max) {
      disconnect();
      return std::nullopt;
    }
    auto parsed = api::parse_json(std::string_view(input_).substr(0, newline));
    input_.erase(0, newline + 1U);
    newline_ = input_.find('\n');
    if (parsed.value.has_value() && parsed.value->kind == api::JsonKind::object &&
        parsed.value->object.size() == 3U) {
      const auto kind = api::json_string(*parsed.value, "kind");
      const auto id = api::json_unsigned(*parsed.value, "invocation");
      auto payload = std::ranges::find_if(
          parsed.value->object, [](const auto& member) { return member.key == "payload"; });
      if (kind.has_value() && id.has_value() && *id > 0 && payload != parsed.value->object.end() &&
          (*kind == "invoke" || *kind == "proc" || *kind == "result" || *kind == "complete" ||
           *kind == "cancel")) {
        return CommandMessage{
            .kind = std::string(*kind), .invocation = *id, .payload = std::move(payload->value)};
      }
    }
  } catch (...) {
    disconnect();
    return std::nullopt;
  }
  disconnect();
  return std::nullopt;
}

auto CommandChannel::send(const std::string_view kind, const std::uint64_t invocation,
                          const std::string_view json_payload) noexcept -> bool {
  if (descriptor_ < 0) {
    return false;
  }
  try {
    auto payload = json_payload;
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r')) {
      payload.remove_suffix(1);
    }
    std::string message = R"({"kind":)";
    if (!api::append_json_string(message, kind)) {
      return false;
    }
    message += R"(,"invocation":)" + std::to_string(invocation) + R"(,"payload":)";
    if (message.size() + payload.size() + 2U > api::json_bytes_max) {
      return false;
    }
    message += payload;
    message += "}\n";
    if (message.size() > queued_bytes_max - (output_.size() - written_)) {
      return false;
    }
    // Compact only on admission, never on each partial write.
    if (written_ > 0) {
      output_.erase(0, written_);
      written_ = 0;
    }
    output_ += message;
    return true;
  } catch (...) {
    return false;
  }
}

CommandRuntime::CommandRuntime(const int descriptor,
                               const std::span<const CommandDescriptor> commands,
                               const StopHost stop, void* const context,
                               const int listener) noexcept
    : channel_(descriptor), commands_(commands), stop_(stop), stop_context_(context),
      listener_(listener) {}

auto CommandRuntime::commands() const noexcept -> std::span<const CommandDescriptor> {
  return channel_.descriptor() >= 0 ? commands_ : std::span<const CommandDescriptor>{};
}

auto CommandRuntime::find(const std::uint64_t id) noexcept -> Invocation* {
  if (id == 0 || channel_.descriptor() < 0) {
    return nullptr;
  }
  auto* const found = std::ranges::find_if(slots_, [id](const auto& slot) {
    return slot.id == id && slot.phase != InvocationPhase::cancelling;
  });
  return found == slots_.end() ? nullptr : &*found;
}

auto CommandRuntime::start(const std::string_view command,
                           const std::span<const std::string> arguments,
                           const InvocationContext context,
                           const std::chrono::steady_clock::time_point now) noexcept -> bool {
  const auto declarations = commands();
  const auto definition =
      std::ranges::find_if(declarations, [&](const auto& item) { return item.name == command; });
  auto* slot = std::ranges::find_if(slots_, [](const auto& item) { return item.id == 0; });
  if (definition == declarations.end() || slot == slots_.end() ||
      next_id_ == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  try {
    std::string payload = R"({"schema":"frame.command-context/v1","command":)";
    if (!api::append_json_string(payload, command)) {
      return false;
    }
    sockaddr_un address{};
    socklen_t address_size = sizeof(address);
    std::string_view endpoint;
    // POSIX sockets expose their family-specific address through sockaddr.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* const socket_address = reinterpret_cast<sockaddr*>(&address);
    if (listener_ >= 0 && ::getsockname(listener_, socket_address, &address_size) == 0 &&
        address.sun_family == AF_UNIX) {
      const auto path = std::span(address.sun_path);
      endpoint = {path.data(),
                  static_cast<std::size_t>(std::ranges::find(path, '\0') - path.begin())};
    }
    payload += R"(,"endpoint":)";
    if (!api::append_json_string(payload, endpoint)) {
      return false;
    }
    payload += R"(,"session":")" + id_text(context.session) + R"(","tab":")" +
               id_text(context.tab) + R"(","pane":")" + id_text(context.pane) +
               R"(","connection":")" + id_text(context.connection) + R"(","args":[)";
    for (const auto& argument : arguments) {
      if (&argument != arguments.data()) {
        payload += ',';
      }
      if (!api::append_json_string(payload, argument)) {
        return false;
      }
    }
    payload += "]}";
    const auto id = ++next_id_;
    if (!channel_.send("invoke", id, payload)) {
      return false;
    }
    *slot = {.id = id,
             .context = context,
             .deadline = now + std::chrono::milliseconds(definition->timeout_ms),
             .phase = InvocationPhase::running};
    return true;
  } catch (...) {
    return false;
  }
}

void CommandRuntime::cancel(const std::uint64_t id) noexcept {
  auto* const invocation = find(id);
  if (invocation == nullptr) {
    return;
  }
  invocation->phase = InvocationPhase::cancelling;
  // Retain the deadline and capacity until acknowledgement. A blocked host must not escape its
  // watchdog or grow an unbounded cancellation backlog just because the controller detached.
  if (!channel_.send("cancel", id, "null")) {
    channel_.disconnect();
  }
}

void CommandRuntime::finish(const std::uint64_t id) noexcept {
  for (auto& invocation : slots_) {
    if (invocation.id == id) {
      invocation = {};
      return;
    }
  }
}

void CommandRuntime::fail() noexcept {
  channel_.disconnect();
  slots_ = {};
  if (stop_ != nullptr) {
    stop_(stop_context_);
    stop_ = nullptr;
  }
}

auto CommandRuntime::result(const std::uint64_t id, const std::string_view json) noexcept -> bool {
  auto* const invocation = find(id);
  if (invocation == nullptr) {
    return false;
  }
  if (!channel_.send("result", id, json)) {
    channel_.disconnect();
    return false;
  }
  invocation->phase = InvocationPhase::running;
  return true;
}

auto CommandRuntime::poll_timeout(int current,
                                  const std::chrono::steady_clock::time_point now) const noexcept
    -> int {
  if (channel_.buffered()) {
    return 0;
  }
  for (const auto& slot : slots_) {
    if (slot.id == 0) {
      continue;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(slot.deadline - now);
    const auto candidate =
        now >= slot.deadline ? 0 : static_cast<int>(std::max(std::int64_t{1}, remaining.count()));
    current = current < 0 ? candidate : std::min(current, candidate);
  }
  return current;
}

} // namespace frame::extension
