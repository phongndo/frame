#include "extension/client.hpp"

#include "api/json.hpp"
#include "extension/protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace frame::extension {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto request_timeout = std::chrono::seconds(3);
constexpr std::size_t event_limit = 64;

[[nodiscard]] auto connect_endpoint(const std::string_view endpoint) -> int {
  sockaddr_un address{};
  if (endpoint.empty() || endpoint.size() >= sizeof(address.sun_path) || endpoint.contains('\0')) {
    throw std::runtime_error("invalid extension endpoint");
  }
  address.sun_family = AF_UNIX;
  std::memcpy(std::span(address.sun_path).data(), endpoint.data(), endpoint.size());
  const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (descriptor < 0) {
    throw std::runtime_error("extension socket failed");
  }
  // POSIX descriptor flags and socket addresses have erased C interfaces.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const bool cloexec = ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* const generic = reinterpret_cast<const sockaddr*>(&address);
  if (!cloexec || ::connect(descriptor, generic, sizeof(address)) != 0) {
    static_cast<void>(::close(descriptor));
    throw std::runtime_error("extension connection failed");
  }
  return descriptor;
}

[[nodiscard]] auto wait_ready(const int descriptor, const short events,
                              const Clock::time_point deadline) -> bool {
  while (true) {
    const auto now = Clock::now();
    const int timeout =
        deadline == Clock::time_point::max()
            ? -1
            : static_cast<int>(std::clamp<std::int64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count(), 0,
                  std::numeric_limits<int>::max()));
    pollfd ready{.fd = descriptor, .events = events, .revents = 0};
    const auto result = ::poll(&ready, 1, timeout);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 || (ready.revents & (POLLERR | POLLNVAL)) != 0) {
      throw std::runtime_error("extension connection failed");
    }
    return result > 0;
  }
}
} // namespace

Client::Client(const std::string_view endpoint, const std::string_view hello)
    : peer_(connect_endpoint(endpoint)) {
  static_cast<void>(request(RecordKind::hello, RecordKind::welcome, hello));
}

auto Client::ready() const noexcept -> bool { return !events_.empty() || peer_.buffered_record(); }

auto Client::send(const RecordKind kind, const std::string_view document) -> std::uint32_t {
  if (sequence_ == std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("extension sequence exhausted");
  }
  ++sequence_;
  if (!peer_.send_json(kind, sequence_, document)) {
    throw std::runtime_error("extension output capacity exceeded");
  }
  return sequence_;
}

void Client::flush(const Clock::time_point deadline) {
  while (peer_.connected() && peer_.output_bytes() != 0) {
    static_cast<void>(peer_.write_ready());
    if (peer_.output_bytes() != 0 && !wait_ready(peer_.descriptor(), POLLOUT, deadline)) {
      throw std::runtime_error("extension write timed out");
    }
  }
  if (!peer_.connected()) {
    throw std::runtime_error("extension disconnected");
  }
}

auto Client::receive(const Clock::time_point deadline) -> std::optional<ClientRecord> {
  while (peer_.connected()) {
    if (const auto record = peer_.receive(); record.has_value()) {
      // JSON byte and character storage share a representation.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      const std::string_view text(reinterpret_cast<const char*>(record->payload.data()),
                                  record->payload.size());
      auto parsed = api::parse_json(text);
      if (!parsed.value.has_value()) {
        throw std::runtime_error("invalid extension response");
      }
      ClientRecord result{
          .kind = record->kind, .sequence = record->sequence, .document = std::move(*parsed.value)};
      peer_.consume();
      return result;
    }
    if (!wait_ready(peer_.descriptor(), POLLIN, deadline)) {
      return std::nullopt;
    }
    static_cast<void>(peer_.read_ready());
  }
  throw std::runtime_error("extension disconnected");
}

auto Client::request(const RecordKind kind, const RecordKind expected,
                     const std::string_view document) -> api::JsonValue {
  const auto sequence = send(kind, document);
  const auto deadline = Clock::now() + request_timeout;
  flush(deadline);
  while (true) {
    if (Clock::now() >= deadline) {
      throw std::runtime_error("extension request timed out");
    }
    auto record = receive(deadline);
    if (!record.has_value()) {
      throw std::runtime_error("extension request timed out");
    }
    if (record->kind == expected && record->sequence == sequence) {
      return std::move(record->document);
    }
    if (record->kind != RecordKind::event || events_.size() == event_limit) {
      throw std::runtime_error("extension request rejected or event capacity exceeded");
    }
    events_.push_back(std::move(*record));
  }
}

auto Client::next(const int timeout_ms) -> std::optional<ClientRecord> {
  if (!events_.empty()) {
    auto result = std::move(events_.front());
    events_.pop_front();
    return result;
  }
  return receive(timeout_ms < 0 ? Clock::time_point::max()
                                : Clock::now() + std::chrono::milliseconds(timeout_ms));
}

auto Client::proc(const std::string_view commands) -> api::JsonValue {
  return request(RecordKind::proc, RecordKind::proc_result,
                 std::string{R"({"schema":"frame.proc/v1","commands":)"} + std::string(commands) +
                     '}');
}

void Client::update(const std::string_view content) {
  static_cast<void>(send(RecordKind::surface_update, content));
  flush(Clock::now() + request_timeout);
}

auto Client::submit_proc(const std::string_view commands) -> std::uint32_t {
  const auto sequence =
      send(RecordKind::proc,
           std::string{R"({"schema":"frame.proc/v1","commands":)"} + std::string(commands) + '}');
  flush(Clock::now() + request_timeout);
  return sequence;
}

auto json_quote(const std::string_view value) -> std::string {
  std::string output;
  if (!api::append_json_string(output, value)) {
    throw std::runtime_error("extension string capacity exceeded");
  }
  return output;
}

auto json_encode(const api::JsonValue& value) -> std::string {
  std::string output;
  if (!api::append_json_value(output, value)) {
    throw std::runtime_error("extension document capacity exceeded");
  }
  return output;
}

} // namespace frame::extension
