#include "core/outer_attention.hpp"

#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace lemma::core {
namespace {

constexpr std::string_view notification_begin = "\x1B]777;notify;";
constexpr std::string_view string_terminator = "\x1B\\";
static_assert(notification_begin.size() + limits::outer_notification_title_bytes_max + 1U +
                  limits::outer_notification_body_bytes_max + string_terminator.size() ==
              limits::outer_notification_frame_bytes_max);
static_assert(std::string_view{"\x1B]9;4;1;100\x1B\\"}.size() ==
              limits::outer_progress_frame_bytes_max);
static_assert(std::string_view{"\x1B]7;"}.size() + limits::outer_cwd_bytes_max +
                  string_terminator.size() ==
              limits::outer_cwd_frame_bytes_max);

[[nodiscard]] constexpr auto hex_value(const char digit) noexcept -> std::optional<std::uint8_t> {
  if (digit >= '0' && digit <= '9') {
    return static_cast<std::uint8_t>(digit - '0');
  }
  if (digit >= 'a' && digit <= 'f') {
    return static_cast<std::uint8_t>(digit - 'a' + 10);
  }
  if (digit >= 'A' && digit <= 'F') {
    return static_cast<std::uint8_t>(digit - 'A' + 10);
  }
  return std::nullopt;
}

[[nodiscard]] constexpr auto ascii_lower(const char character) noexcept -> char {
  return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                              : character;
}

[[nodiscard]] constexpr auto same_host(const std::string_view left,
                                       const std::string_view right) noexcept -> bool {
  return left.size() == right.size() &&
         std::ranges::equal(left, right, [](const char first, const char second) {
           return ascii_lower(first) == ascii_lower(second);
         });
}

[[nodiscard]] constexpr auto progress_code(const vt::ProgressState state) noexcept -> char {
  switch (state) {
  case vt::ProgressState::none:
    return '0';
  case vt::ProgressState::normal:
    return '1';
  case vt::ProgressState::error:
    return '2';
  case vt::ProgressState::indeterminate:
    return '3';
  case vt::ProgressState::paused:
    return '4';
  }
  return '0';
}

// Decodes the percent escape starting at text's front, or returns nullopt when malformed.
[[nodiscard]] auto escaped_byte(const std::string_view text) noexcept -> std::optional<char> {
  if (text.size() < 3U) {
    return std::nullopt;
  }
  const auto high = hex_value(text.at(1));
  const auto low = hex_value(text.at(2));
  if (!high.has_value() || !low.has_value()) {
    return std::nullopt;
  }
  return static_cast<char>((*high << 4U) | *low);
}

// Decodes URI percent escapes into storage. NUL, malformed escapes, and overflow fail.
[[nodiscard]] auto percent_decode(const std::string_view text,
                                  const std::span<char> storage) noexcept
    -> std::optional<std::string_view> {
  std::size_t size = 0;
  for (std::size_t index = 0; index < text.size(); ++index) {
    auto byte = std::optional{text.at(index)};
    if (*byte == '%') {
      byte = escaped_byte(text.substr(index));
      index += 2U;
    }
    if (!byte.has_value() || *byte == '\0' || size >= storage.size()) {
      return std::nullopt;
    }
    storage.subspan(size, 1).front() = *byte;
    ++size;
  }
  return std::string_view(storage.data(), size);
}

} // namespace

auto AttentionRateLimit::take(const TimePoint now) noexcept -> bool {
  // refilled_at_ is meaningful only below a full bucket: it starts when the first token is spent.
  if (tokens_ < burst_ && now >= refilled_at_ + interval_) {
    const auto earned = (now - refilled_at_) / interval_;
    const auto missing = static_cast<std::int64_t>(burst_ - tokens_);
    tokens_ = static_cast<std::uint8_t>(tokens_ + std::min<std::int64_t>(earned, missing));
    refilled_at_ += earned * interval_;
  }
  if (tokens_ == 0) {
    return false;
  }
  if (tokens_ == burst_) {
    refilled_at_ = now;
  }
  --tokens_;
  return true;
}

auto append_outer_bytes(const std::span<std::byte> output, std::size_t& used,
                        const std::string_view text) noexcept -> bool {
  if (used > output.size() || text.size() > output.size() - used) {
    return false;
  }
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    output.subspan(used).begin());
  used += text.size();
  return true;
}

auto append_outer_notification(const std::span<std::byte> output, std::size_t& used,
                               const std::string_view title, const std::string_view body) noexcept
    -> bool {
  const auto size =
      notification_begin.size() + title.size() + 1U + body.size() + string_terminator.size();
  if (used > output.size() || size > output.size() - used) {
    return false;
  }
  return append_outer_bytes(output, used, notification_begin) &&
         append_outer_bytes(output, used, title) && append_outer_bytes(output, used, ";") &&
         append_outer_bytes(output, used, body) &&
         append_outer_bytes(output, used, string_terminator);
}

auto append_outer_progress(const std::span<std::byte> output, std::size_t& used,
                           const vt::ProgressState state,
                           const std::optional<std::uint8_t> percent) noexcept -> bool {
  std::array<char, limits::outer_progress_frame_bytes_max> sequence{};
  std::size_t size = 0;
  const auto put = [&sequence, &size](const std::string_view text) noexcept {
    std::ranges::copy(text, std::span(sequence).subspan(size).begin());
    size += text.size();
  };
  put("\x1B]9;4;");
  const std::array code{progress_code(state)};
  put({code.data(), code.size()});
  // Normal progress always carries a value; error and paused carry one when reported.
  const bool valued = state == vt::ProgressState::normal ||
                      ((state == vt::ProgressState::error || state == vt::ProgressState::paused) &&
                       percent.has_value());
  if (valued) {
    const auto value = std::min<unsigned>(percent.value_or(0), 100U);
    std::array<char, 4> digits{';'};
    std::size_t count = 1;
    if (value >= 100U) {
      std::span(digits).subspan(count++, 1).front() = '1';
    }
    if (value >= 10U) {
      std::span(digits).subspan(count++, 1).front() =
          static_cast<char>('0' + ((value / 10U) % 10U));
    }
    std::span(digits).subspan(count++, 1).front() = static_cast<char>('0' + (value % 10U));
    put({digits.data(), count});
  }
  put(string_terminator);
  return append_outer_bytes(output, used, {sequence.data(), size});
}

auto outer_cwd_forwardable(const std::string_view uri) noexcept -> bool {
  if (uri.empty() || uri.size() > limits::outer_cwd_bytes_max) {
    return false;
  }
  OuterText<limits::outer_cwd_bytes_max> sanitized;
  sanitized.append(uri);
  return sanitized.view() == uri;
}

auto append_outer_cwd(const std::span<std::byte> output, std::size_t& used,
                      const std::string_view uri) noexcept -> bool {
  const auto size = std::string_view{"\x1B]7;"}.size() + uri.size() + string_terminator.size();
  if (used > output.size() || size > output.size() - used) {
    return false;
  }
  return append_outer_bytes(output, used, "\x1B]7;") && append_outer_bytes(output, used, uri) &&
         append_outer_bytes(output, used, string_terminator);
}

auto local_directory_from_osc7(const std::string_view uri, const std::string_view local_host,
                               const std::span<char> storage) noexcept
    -> std::optional<std::string_view> {
  constexpr std::string_view scheme = "file://";
  if (uri.size() < scheme.size() || !same_host(uri.substr(0, scheme.size()), scheme)) {
    return std::nullopt;
  }
  const auto authority_and_path = uri.substr(scheme.size());
  const auto slash = authority_and_path.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  const auto host = authority_and_path.substr(0, slash);
  if (!host.empty() && !same_host(host, "localhost") &&
      (local_host.empty() || !same_host(host, local_host))) {
    return std::nullopt;
  }
  return percent_decode(authority_and_path.substr(slash), storage);
}

} // namespace lemma::core
