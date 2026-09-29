#ifndef FRAME_CLIENT_HOST_TERMINAL_THEME_HPP
#define FRAME_CLIENT_HOST_TERMINAL_THEME_HPP

#include "protocol/attachment.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace frame::client {

inline constexpr std::size_t host_theme_query_bytes_max = 512;
inline constexpr std::size_t host_theme_palette_colors_queried =
    protocol::host_theme_palette_colors;

// Filters bounded OSC 4/10/11/17/19 replies from the physical input stream while retaining every
// byte that is not a valid theme reply for normal client input processing.
class HostTerminalThemeParser final {
public:
  void push(std::span<const std::byte> bytes) noexcept;
  void finish() noexcept;

  [[nodiscard]] auto theme() const noexcept -> std::optional<protocol::HostTerminalTheme>;
  [[nodiscard]] auto pending_input() const noexcept -> std::span<const std::byte>;
  void consume_pending_input() noexcept { pending_size_ = 0; }
  [[nodiscard]] auto complete() const noexcept -> bool;
  [[nodiscard]] auto overflowed() const noexcept -> bool { return overflowed_; }

private:
  enum class State : std::uint8_t {
    normal,
    escape,
    osc,
    osc_escape,
  };

  void append_pending(std::byte byte) noexcept;
  void append_candidate(std::byte byte) noexcept;
  void flush_candidate() noexcept;
  void finish_candidate() noexcept;

  protocol::HostTerminalTheme theme_;
  std::array<std::byte, protocol::input_bytes_max + 64U> pending_{};
  std::array<std::byte, 64> candidate_{};
  std::size_t pending_size_{0};
  std::size_t candidate_size_{0};
  std::size_t palette_count_{0};
  State state_{State::normal};
  bool overflowed_{false};
};

[[nodiscard]] auto encode_host_terminal_theme_query(std::span<char> output) noexcept -> std::size_t;

} // namespace frame::client

#endif // FRAME_CLIENT_HOST_TERMINAL_THEME_HPP
