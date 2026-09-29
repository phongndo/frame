#include "client/host_terminal_theme.hpp"

#include "protocol/attachment.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace frame::client {
namespace {

[[nodiscard]] auto hex_component(const std::string_view text) noexcept
    -> std::optional<std::uint8_t> {
  if (text.empty() || text.size() > 4) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  const auto* end = text.end();
  const auto parsed = std::from_chars(text.begin(), end, value, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  const auto bits = static_cast<unsigned>(text.size() * 4U);
  const auto maximum = (std::uint32_t{1} << bits) - 1U;
  return static_cast<std::uint8_t>(((value * 255U) + (maximum / 2U)) / maximum);
}

[[nodiscard]] auto rgb_color(const std::string_view text) noexcept
    -> std::optional<protocol::RgbColor> {
  if (text.starts_with("rgb:")) {
    auto components = text;
    components.remove_prefix(4);
    const auto first_separator = components.find('/');
    if (first_separator == std::string_view::npos) {
      return std::nullopt;
    }
    auto green_blue = components;
    green_blue.remove_prefix(first_separator + 1U);
    const auto second_separator = green_blue.find('/');
    if (second_separator == std::string_view::npos ||
        green_blue.find('/', second_separator + 1U) != std::string_view::npos) {
      return std::nullopt;
    }
    auto red_text = components;
    red_text.remove_suffix(red_text.size() - first_separator);
    auto green_text = green_blue;
    green_text.remove_suffix(green_text.size() - second_separator);
    const auto red = hex_component(red_text);
    const auto green = hex_component(green_text);
    green_blue.remove_prefix(second_separator + 1U);
    const auto blue = hex_component(green_blue);
    if (!red.has_value() || !green.has_value() || !blue.has_value()) {
      return std::nullopt;
    }
    return protocol::RgbColor{.red = *red, .green = *green, .blue = *blue};
  }

  if (!text.starts_with('#')) {
    return std::nullopt;
  }
  auto encoded = text;
  encoded.remove_prefix(1);
  if (encoded.empty() || encoded.size() > 12 || encoded.size() % 3U != 0) {
    return std::nullopt;
  }
  const auto digits = encoded.size() / 3U;
  auto remaining = encoded;
  auto component = remaining;
  component.remove_suffix(component.size() - digits);
  const auto red = hex_component(component);
  remaining.remove_prefix(digits);
  component = remaining;
  component.remove_suffix(component.size() - digits);
  const auto green = hex_component(component);
  remaining.remove_prefix(digits);
  const auto blue = hex_component(remaining);
  if (!red.has_value() || !green.has_value() || !blue.has_value()) {
    return std::nullopt;
  }
  return protocol::RgbColor{.red = *red, .green = *green, .blue = *blue};
}

[[nodiscard]] auto candidate_body(const std::span<const std::byte> candidate) noexcept
    -> std::optional<std::string_view> {
  if (candidate.size() < 5 || candidate.front() != std::byte{0x1B} ||
      candidate.subspan(1, 1).front() != std::byte{']'}) {
    return std::nullopt;
  }
  std::size_t suffix = 0;
  if (candidate.back() == std::byte{0x07}) {
    suffix = 1;
  } else if (candidate.subspan(candidate.size() - 2U, 1).front() == std::byte{0x1B} &&
             candidate.back() == std::byte{'\\'}) {
    suffix = 2;
  } else {
    return std::nullopt;
  }
  const auto body = candidate.subspan(2, candidate.size() - 2U - suffix);
  // The bytes are borrowed as text only for strict ASCII response parsing.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
}

[[nodiscard]] auto parse_theme_response(const std::span<const std::byte> candidate,
                                        protocol::HostTerminalTheme& theme,
                                        std::size_t& palette_count) noexcept -> bool {
  const auto body = candidate_body(candidate);
  if (!body.has_value()) {
    return false;
  }
  if (body->starts_with("10;") || body->starts_with("11;") || body->starts_with("17;") ||
      body->starts_with("19;")) {
    auto color_text = *body;
    color_text.remove_prefix(3);
    const auto color = rgb_color(color_text);
    if (!color.has_value()) {
      return false;
    }
    if (body->starts_with("10;")) {
      theme.foreground = *color;
    } else if (body->starts_with("11;")) {
      theme.background = *color;
    } else if (body->starts_with("17;")) {
      theme.selection_background = *color;
    } else {
      theme.selection_foreground = *color;
    }
    return true;
  }
  if (!body->starts_with("4;")) {
    return false;
  }
  auto fields = *body;
  fields.remove_prefix(2);
  const auto separator = fields.find(';');
  if (separator == std::string_view::npos) {
    return false;
  }
  std::uint16_t index = 0;
  auto index_text = fields;
  index_text.remove_suffix(index_text.size() - separator);
  fields.remove_prefix(separator + 1U);
  const auto* index_end = index_text.end();
  const auto parsed = std::from_chars(index_text.begin(), index_end, index, 10);
  const auto color = rgb_color(fields);
  if (parsed.ec != std::errc{} || parsed.ptr != index_end ||
      index >= host_theme_palette_colors_queried || !color.has_value()) {
    return false;
  }
  if (!theme.has_palette_color(index)) {
    ++palette_count;
  }
  theme.set_palette_color(index, *color);
  return true;
}

} // namespace

void HostTerminalThemeParser::append_pending(const std::byte byte) noexcept {
  if (pending_size_ >= pending_.size()) {
    overflowed_ = true;
    return;
  }
  std::span(pending_).subspan(pending_size_, 1).front() = byte;
  ++pending_size_;
}

void HostTerminalThemeParser::append_candidate(const std::byte byte) noexcept {
  if (candidate_size_ >= candidate_.size()) {
    flush_candidate();
    append_pending(byte);
    return;
  }
  std::span(candidate_).subspan(candidate_size_, 1).front() = byte;
  ++candidate_size_;
}

void HostTerminalThemeParser::flush_candidate() noexcept {
  for (const auto byte : std::span(candidate_).first(candidate_size_)) {
    append_pending(byte);
  }
  candidate_size_ = 0;
  state_ = State::normal;
}

void HostTerminalThemeParser::finish_candidate() noexcept {
  if (!parse_theme_response(std::span(candidate_).first(candidate_size_), theme_, palette_count_)) {
    flush_candidate();
    return;
  }
  candidate_size_ = 0;
  state_ = State::normal;
}

// The byte-wise OSC state machine is clearer as one exhaustive switch.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void HostTerminalThemeParser::push(const std::span<const std::byte> bytes) noexcept {
  for (const auto byte : bytes) {
    switch (state_) {
    case State::normal:
      if (byte == std::byte{0x1B}) {
        candidate_size_ = 0;
        append_candidate(byte);
        state_ = State::escape;
      } else {
        append_pending(byte);
      }
      break;
    case State::escape:
      append_candidate(byte);
      if (byte == std::byte{']'}) {
        state_ = State::osc;
      } else {
        flush_candidate();
      }
      break;
    case State::osc:
      append_candidate(byte);
      if (byte == std::byte{0x07}) {
        finish_candidate();
      } else if (byte == std::byte{0x1B}) {
        state_ = State::osc_escape;
      }
      break;
    case State::osc_escape:
      append_candidate(byte);
      if (byte == std::byte{'\\'}) {
        finish_candidate();
      } else {
        state_ = State::osc;
      }
      break;
    }
  }
}

void HostTerminalThemeParser::finish() noexcept {
  if (candidate_size_ != 0) {
    flush_candidate();
  }
}

[[nodiscard]] auto HostTerminalThemeParser::theme() const noexcept
    -> std::optional<protocol::HostTerminalTheme> {
  return theme_.empty() ? std::nullopt : std::optional{theme_};
}

[[nodiscard]] auto HostTerminalThemeParser::pending_input() const noexcept
    -> std::span<const std::byte> {
  return std::span(pending_).first(pending_size_);
}

[[nodiscard]] auto HostTerminalThemeParser::complete() const noexcept -> bool {
  return theme_.foreground.has_value() && theme_.background.has_value() &&
         palette_count_ == host_theme_palette_colors_queried;
}

[[nodiscard]] auto encode_host_terminal_theme_query(const std::span<char> output) noexcept
    -> std::size_t {
  std::size_t used = 0;
  const auto append = [&](const std::string_view text) {
    if (text.size() > output.size() - used) {
      return false;
    }
    std::ranges::copy(text, output.subspan(used).begin());
    used += text.size();
    return true;
  };
  if (!append("\x1B]17;?\x1B\\\x1B]19;?\x1B\\\x1B]10;?\x1B\\\x1B]11;?\x1B\\")) {
    return 0;
  }
  for (std::size_t index = 0; index < host_theme_palette_colors_queried; ++index) {
    std::array<char, 3> digits{};
    const auto encoded = std::to_chars(digits.begin(), digits.end(), index);
    if (encoded.ec != std::errc{} || !append("\x1B]4;") ||
        !append(std::string_view(digits.data(),
                                 static_cast<std::size_t>(encoded.ptr - digits.data()))) ||
        !append(";?\x1B\\")) {
      return 0;
    }
  }
  return used;
}

} // namespace frame::client
