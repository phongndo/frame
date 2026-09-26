#ifndef LEMMA_CORE_OUTER_ATTENTION_HPP
#define LEMMA_CORE_OUTER_ATTENTION_HPP

#include "lemma/id.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace lemma::core {

// Child-controlled text is sanitized before entering the outer-terminal stream. C0, DEL, UTF-8
// encoded C1 controls, and malformed UTF-8 bytes are dropped; the bounded result is truncated at a
// code point boundary, so no escape, BEL, or string terminator can end an OSC early. Separator
// replacement keeps a field from splitting a multi-field OSC.
template <std::size_t Capacity> class OuterText final {
public:
  constexpr explicit OuterText(const char separator = '\0') noexcept : separator_(separator) {}

  constexpr void append(const std::string_view text) noexcept {
    std::size_t index = 0;
    while (!truncated_ && index < text.size()) {
      const auto length = sequence_length(text.substr(index));
      if (length == 0) {
        ++index;
        continue;
      }
      const auto sequence = text.substr(index, length);
      index += length;
      if (control(sequence)) {
        continue;
      }
      if (length > Capacity - size_) {
        truncated_ = true;
        return;
      }
      if (separator_ != '\0' && sequence.front() == separator_) {
        std::span(bytes_).subspan(size_, 1).front() = ' ';
      } else {
        std::ranges::copy(sequence, std::span(bytes_).subspan(size_).begin());
      }
      size_ += length;
    }
  }

  [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
    return {bytes_.data(), size_};
  }

private:
  // Returns the length of one well-formed UTF-8 sequence at the start of text, or zero.
  [[nodiscard]] static constexpr auto sequence_length(const std::string_view text) noexcept
      -> std::size_t {
    const auto lead = static_cast<std::uint8_t>(text.front());
    std::size_t length = 0;
    if (lead < 0x80U) {
      return 1;
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
      length = 2;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      length = 3;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      length = 4;
    }
    if (length == 0 || length > text.size()) {
      return 0;
    }
    const auto continuation = text.substr(1, length - 1U);
    return std::ranges::all_of(continuation,
                               [](const char byte) noexcept {
                                 return (static_cast<std::uint8_t>(byte) & 0xC0U) == 0x80U;
                               })
               ? length
               : 0;
  }

  [[nodiscard]] static constexpr auto control(const std::string_view sequence) noexcept -> bool {
    const auto lead = static_cast<std::uint8_t>(sequence.front());
    return lead < 0x20U || lead == 0x7FU ||
           (lead == 0xC2U && static_cast<std::uint8_t>(sequence.at(1)) < 0xA0U);
  }

  std::array<char, Capacity> bytes_{};
  std::size_t size_{0};
  char separator_{'\0'};
  bool truncated_{false};
};

// Token bucket bounding attention forwarded to one outer terminal. It refills lazily from the
// reactor clock, so an idle attachment has no timer.
class AttentionRateLimit final {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Duration = Clock::duration;

  constexpr AttentionRateLimit(const std::uint8_t burst, const Duration interval) noexcept
      : interval_(interval), burst_(burst), tokens_(burst) {}

  // Consumes one token when available. After a refusal, next_token() reports when to retry.
  [[nodiscard]] auto take(TimePoint now) noexcept -> bool;
  [[nodiscard]] auto next_token() const noexcept -> TimePoint { return refilled_at_ + interval_; }

private:
  TimePoint refilled_at_;
  Duration interval_;
  std::uint8_t burst_;
  std::uint8_t tokens_;
};

// A burst of four bells, then one per 250 ms, matches repeated interactive bells such as failed
// completion while bounding a BEL flood. Desktop notifications are costlier interruptions: three,
// then one per five seconds.
inline constexpr std::uint8_t outer_bell_burst = 4;
inline constexpr auto outer_bell_interval = std::chrono::milliseconds{250};
inline constexpr std::uint8_t outer_notification_burst = 3;
inline constexpr auto outer_notification_interval = std::chrono::seconds{5};
// Progress changes coalesce to the latest value, presented at most four times per second.
inline constexpr auto outer_progress_interval = std::chrono::milliseconds{250};

// Presentation shadow of the attention forwarded to one client connection. Signal values stay in
// each Pane's terminal; this retains only what was presented and delivery pacing, and moves with
// the connection across Session switches.
struct OuterAttention final {
  AttentionRateLimit bells{outer_bell_burst, outer_bell_interval};
  AttentionRateLimit notifications{outer_notification_burst, outer_notification_interval};
  // Earliest time a rate-limited or remaining attention item should request another frame.
  std::optional<AttentionRateLimit::TimePoint> retry_at;
  // Pane whose OSC 7 directory was last considered, and its terminal's cwd change count then.
  SessionId cwd_session;
  PaneId cwd_pane;
  std::uint64_t cwd_changes{0};
  vt::ProgressState progress{vt::ProgressState::none};
  std::optional<std::uint8_t> progress_percent;
  std::optional<AttentionRateLimit::TimePoint> progress_presented_at;
  // Some Pane's notification count may exceed its forwarded count.
  bool notification_pending{false};
};

using OuterNotificationTitle = OuterText<limits::outer_notification_title_bytes_max>;
using OuterNotificationBody = OuterText<limits::outer_notification_body_bytes_max>;

// Appends text when it fits in output after used; false leaves output unchanged.
[[nodiscard]] auto append_outer_bytes(std::span<std::byte> output, std::size_t& used,
                                      std::string_view text) noexcept -> bool;
[[nodiscard]] auto append_outer_notification(std::span<std::byte> output, std::size_t& used,
                                             std::string_view title, std::string_view body) noexcept
    -> bool;
// State none removes the indicator.
[[nodiscard]] auto append_outer_progress(std::span<std::byte> output, std::size_t& used,
                                         vt::ProgressState state,
                                         std::optional<std::uint8_t> percent) noexcept -> bool;
// Forwards an OSC 7 URI only when it is nonempty, bounded, and free of controls; the value is
// never truncated or rewritten because a changed path would name another directory.
[[nodiscard]] auto outer_cwd_forwardable(std::string_view uri) noexcept -> bool;
[[nodiscard]] auto append_outer_cwd(std::span<std::byte> output, std::size_t& used,
                                    std::string_view uri) noexcept -> bool;

// Decodes an OSC 7 `file://HOST/PATH` report into a local absolute directory. The host must be
// empty, `localhost`, or `local_host`, so a directory reported through SSH never names a daemon
// path. Returns the decoded path within storage, or nullopt.
[[nodiscard]] auto local_directory_from_osc7(std::string_view uri, std::string_view local_host,
                                             std::span<char> storage) noexcept
    -> std::optional<std::string_view>;

} // namespace lemma::core

#endif // LEMMA_CORE_OUTER_ATTENTION_HPP
