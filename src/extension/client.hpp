#ifndef FRAME_EXTENSION_CLIENT_HPP
#define FRAME_EXTENSION_CLIENT_HPP

#include "api/json.hpp"
#include "extension/protocol.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace frame::extension {

struct ClientRecord final {
  RecordKind kind{RecordKind::error};
  std::uint32_t sequence{0};
  api::JsonValue document;
};

// User-process client for the public extension interface. Blocking requests have a deadline and
// retain interleaved Events; the daemon never links or calls this module.
class Client final {
public:
  Client(std::string_view endpoint, std::string_view hello);
  Client(const Client&) = delete;
  auto operator=(const Client&) -> Client& = delete;
  Client(Client&&) noexcept = default;
  auto operator=(Client&&) noexcept -> Client& = default;
  ~Client() = default;

  [[nodiscard]] auto descriptor() const noexcept -> int { return peer_.descriptor(); }
  [[nodiscard]] auto ready() const noexcept -> bool;
  [[nodiscard]] auto next(int timeout_ms = -1) -> std::optional<ClientRecord>;
  [[nodiscard]] auto proc(std::string_view commands) -> api::JsonValue;
  // Submit without waiting for the result; the caller drains next() and keeps at most one Proc
  // outstanding on this connection. This lets a user UI fetch data without blocking its input.
  [[nodiscard]] auto submit_proc(std::string_view commands) -> std::uint32_t;
  void update(std::string_view content);

private:
  [[nodiscard]] auto receive(std::chrono::steady_clock::time_point deadline)
      -> std::optional<ClientRecord>;
  [[nodiscard]] auto request(RecordKind kind, RecordKind expected, std::string_view document)
      -> api::JsonValue;
  auto send(RecordKind kind, std::string_view document) -> std::uint32_t;
  void flush(std::chrono::steady_clock::time_point deadline);

  FramedPeer peer_;
  std::deque<ClientRecord> events_;
  std::uint32_t sequence_{0};
};

[[nodiscard]] auto json_quote(std::string_view value) -> std::string;
[[nodiscard]] auto json_encode(const api::JsonValue& value) -> std::string;

} // namespace frame::extension

#endif // FRAME_EXTENSION_CLIENT_HPP
