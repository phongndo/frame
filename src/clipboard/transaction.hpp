#ifndef FRAME_CLIPBOARD_TRANSACTION_HPP
#define FRAME_CLIPBOARD_TRANSACTION_HPP

#include "frame/terminal/terminal.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frame::clipboard {
// One outer-terminal transaction, owned by an attachment. No clipboard cache or background work.
// The reactor owns pane/request identity and cancels this state when that ownership changes.
class Transaction final {
public:
  using Clock = std::chrono::steady_clock;
  [[nodiscard]] auto begin(const vt::ClipboardRequest& request, Clock::time_point now) noexcept
      -> std::expected<std::string, vt::ClipboardStatus>;
  void consume(std::string_view record) noexcept;
  // Only complete OSC records may be interleaved with ordinary presentation frames.
  [[nodiscard]] static auto frame_prefix(std::span<const std::byte> data,
                                         std::size_t capacity) noexcept -> std::size_t;
  [[nodiscard]] auto abort_write(std::span<std::byte> output) const noexcept -> std::size_t;
  // Publication completes an OSC 52 write: that protocol has no write acknowledgement.
  void published() noexcept;
  [[nodiscard]] auto protocol() const noexcept -> vt::ClipboardProtocol { return protocol_; }
  [[nodiscard]] auto uncorrelated_read_outstanding() const noexcept -> bool {
    return protocol_ == vt::ClipboardProtocol::osc52 && read_ && published_ && !read_complete_;
  }
  void fail(vt::ClipboardStatus status) noexcept {
    status_ = status;
    done_ = true;
  }
  [[nodiscard]] auto active() const noexcept -> bool { return !id_.empty(); }
  [[nodiscard]] auto done() const noexcept -> bool { return done_; }
  [[nodiscard]] auto expired(Clock::time_point now) const noexcept -> bool {
    return now >= deadline_;
  }
  [[nodiscard]] auto deadline() const noexcept -> Clock::time_point { return deadline_; }
  [[nodiscard]] auto status() const noexcept -> vt::ClipboardStatus { return status_; }
  [[nodiscard]] auto request_id() const noexcept -> std::uint64_t { return request_id_; }
  [[nodiscard]] auto contents() noexcept -> std::span<const vt::ClipboardContent>;
  void reset() noexcept { *this = Transaction{}; }

private:
  struct Content {
    std::string mime;
    std::string data;
  };
  void parse(std::string_view record);
  void parse_osc52(std::string_view part);
  [[nodiscard]] auto content(std::string_view mime) -> Content*;
  std::string id_;
  std::string listing_;
  std::vector<std::string> requested_;
  std::vector<Content> contents_;
  std::array<vt::ClipboardContent, 32> views_{};
  Clock::time_point deadline_;
  std::uint64_t request_id_{0};
  std::size_t decoded_bytes_{0};
  std::size_t osc52_prefix_size_{0};
  std::array<char, 4> osc52_quartet_{};
  std::size_t osc52_quartet_size_{0};
  vt::ClipboardProtocol protocol_{vt::ClipboardProtocol::kitty};
  vt::ClipboardStatus status_{vt::ClipboardStatus::io_error};
  bool read_{false};
  bool primary_{false};
  bool published_{false};
  bool read_complete_{false};
  bool osc52_escape_{false};
  bool osc52_padded_{false};
  bool list_{false};
  bool started_{false};
  bool done_{false};
};
} // namespace frame::clipboard
#endif
