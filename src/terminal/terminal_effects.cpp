#include "lemma/limits.hpp"
#include "terminal/terminal_impl.hpp"

#include "lemma/assert.hpp"
#include "lemma/terminal/terminal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace lemma::vt {
namespace {

// Copies pending bytes in order and retains capacity once drained, so repeated replies reuse it.
auto drain_responses(std::vector<std::byte>& responses, std::size_t& offset,
                     const std::span<std::byte> output) noexcept -> std::size_t {
  LEMMA_ASSERT(offset <= responses.size());
  const auto available = std::span(responses).subspan(offset);
  const auto count = std::min(output.size(), available.size());
  std::ranges::copy(available.first(count), output.begin());
  offset += count;
  if (offset == responses.size()) {
    responses.clear();
    offset = 0;
  }
  return count;
}

void saturating_increment(std::uint64_t& value) noexcept {
  if (value < std::numeric_limits<std::uint64_t>::max()) {
    ++value;
  }
}

[[nodiscard]] auto ghostty_text(const GhosttyString value) noexcept -> std::string_view {
  if (value.ptr == nullptr) {
    return {};
  }
  // Ghostty exposes UTF-8 as uint8_t while string_view uses char.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const char*>(value.ptr), value.len};
}

// Length of the valid, non-control UTF-8 sequence at the start of text, or zero. The branches
// are the closed UTF-8 lead-byte classes.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto printable_sequence(const std::string_view text) noexcept -> std::size_t {
  const auto byte = [&text](const std::size_t index) {
    return static_cast<unsigned char>(std::span(text).subspan(index, 1).front());
  };
  const auto leading = byte(0);
  if (leading < 0x80U) {
    return leading >= 0x20U && leading != 0x7fU ? 1U : 0U;
  }
  std::size_t length = 0;
  std::uint32_t codepoint = 0;
  std::uint32_t minimum = 0;
  if (leading >= 0xc2U && leading <= 0xdfU) {
    length = 2;
    codepoint = leading & 0x1fU;
    minimum = 0x80U;
  } else if (leading >= 0xe0U && leading <= 0xefU) {
    length = 3;
    codepoint = leading & 0x0fU;
    minimum = 0x800U;
  } else if (leading >= 0xf0U && leading <= 0xf4U) {
    length = 4;
    codepoint = leading & 0x07U;
    minimum = 0x10000U;
  } else {
    return 0;
  }
  if (length > text.size()) {
    return 0;
  }
  for (std::size_t index = 1; index < length; ++index) {
    if ((byte(index) & 0xc0U) != 0x80U) {
      return 0;
    }
    codepoint = (codepoint << 6U) | (byte(index) & 0x3fU);
  }
  const bool c1_control = codepoint >= 0x80U && codepoint <= 0x9fU;
  const bool surrogate = codepoint >= 0xd800U && codepoint <= 0xdfffU;
  return codepoint < minimum || codepoint > 0x10ffffU || surrogate || c1_control ? 0U : length;
}

// Copies sanitized UTF-8 into output without splitting a sequence; returns bytes written.
[[nodiscard]] auto copy_signal_text(std::string_view text, const std::span<char> output,
                                    bool& truncated) noexcept -> std::size_t {
  std::size_t used = 0;
  while (!text.empty()) {
    const auto length = printable_sequence(text);
    const auto size = length == 0 ? std::size_t{1} : length;
    if (size > output.size() - used) {
      truncated = true;
      break;
    }
    if (length == 0) {
      output.subspan(used, 1).front() = '?';
    } else {
      std::ranges::copy(text.substr(0, length), output.subspan(used).begin());
    }
    used += size;
    text.remove_prefix(size);
  }
  return used;
}

[[nodiscard]] constexpr auto progress_state(const GhosttyTerminalProgressState state) noexcept
    -> ProgressState {
  switch (state) {
  case GHOSTTY_TERMINAL_PROGRESS_STATE_SET:
    return ProgressState::normal;
  case GHOSTTY_TERMINAL_PROGRESS_STATE_ERROR:
    return ProgressState::error;
  case GHOSTTY_TERMINAL_PROGRESS_STATE_INDETERMINATE:
    return ProgressState::indeterminate;
  case GHOSTTY_TERMINAL_PROGRESS_STATE_PAUSE:
    return ProgressState::paused;
  case GHOSTTY_TERMINAL_PROGRESS_STATE_REMOVE:
  case GHOSTTY_TERMINAL_PROGRESS_STATE_MAX_VALUE:
    break;
  }
  return ProgressState::none;
}

} // namespace

void Terminal::Impl::write_pty([[maybe_unused]] GhosttyTerminal terminal_handle, void* userdata,
                               const std::uint8_t* data, const std::size_t length) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  const auto bytes = std::as_bytes(std::span(data, length));
  if (impl.capturing_clipboard_reply || !impl.clipboard_responses.empty()) {
    try {
      if (bytes.size() <= limits::clipboard_response_bytes_max - impl.clipboard_responses.size()) {
        impl.clipboard_responses.insert(impl.clipboard_responses.end(), bytes.begin(), bytes.end());
        return;
      }
    } catch (...) {
      // Callback boundary: allocation failure means reply integrity is lost, never success.
      impl.pty_response_integrity_failed = true;
    }
  } else {
    constexpr auto bytes_max = limits::terminal_pty_response_bytes_max;
    auto& responses = impl.pty_responses;
    // Reclaim a partially read prefix before growing, so the bound applies to pending bytes.
    responses.erase(responses.begin(),
                    responses.begin() + static_cast<std::ptrdiff_t>(impl.pty_response_offset));
    impl.pty_response_offset = 0;
    try {
      if (bytes.size() <= bytes_max - responses.size()) {
        const auto required = responses.size() + bytes.size();
        if (required > responses.capacity()) {
          // Geometric growth, capped so retained capacity never exceeds the pending-byte bound.
          responses.reserve(std::min(bytes_max, std::max(required, responses.capacity() * 2U)));
        }
        responses.insert(responses.end(), bytes.begin(), bytes.end());
        return;
      }
    } catch (...) {
      // Callback boundary: allocation failure loses the reply, exactly like exceeding the bound.
      impl.pty_response_integrity_failed = true;
    }
  }
  impl.effects.pty_response_overflowed = true;
  impl.pty_response_integrity_failed = true;
}

void Terminal::Impl::bell([[maybe_unused]] GhosttyTerminal terminal_handle,
                          void* userdata) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  saturating_increment(impl.effects.bells);
  saturating_increment(impl.signals.bells);
}

void Terminal::Impl::title_changed([[maybe_unused]] GhosttyTerminal terminal_handle,
                                   void* userdata) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  saturating_increment(impl.effects.title_changes);
  saturating_increment(impl.signals.title_changes);
}

void Terminal::Impl::pwd_changed([[maybe_unused]] GhosttyTerminal terminal_handle,
                                 void* userdata) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  saturating_increment(impl.effects.pwd_changes);
  saturating_increment(impl.signals.cwd_changes);
}

void Terminal::Impl::desktop_notification(
    [[maybe_unused]] GhosttyTerminal terminal_handle, void* userdata,
    const GhosttyTerminalDesktopNotification* notification) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  saturating_increment(impl.effects.desktop_notifications);
  auto& signals = impl.signals;
  saturating_increment(signals.notifications);
  bool truncated = false;
  std::size_t title = 0;
  std::size_t body = 0;
  if (notification != nullptr &&
      notification->size >= offsetof(GhosttyTerminalDesktopNotification, body) +
                                sizeof(GhosttyTerminalDesktopNotification::body)) {
    const auto storage = std::span(signals.notification_text);
    title =
        copy_signal_text(ghostty_text(notification->title),
                         storage.first(TerminalSignals::notification_title_bytes_max), truncated);
    body = copy_signal_text(ghostty_text(notification->body), storage.subspan(title), truncated);
  }
  signals.notification_title_bytes = static_cast<std::uint16_t>(title);
  signals.notification_body_bytes = static_cast<std::uint16_t>(body);
  signals.notification_truncated = truncated;
}

void Terminal::Impl::progress_report([[maybe_unused]] GhosttyTerminal terminal_handle,
                                     void* userdata,
                                     const GhosttyTerminalProgressReport* report) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  saturating_increment(impl.effects.progress_reports);
  if (report == nullptr || report->size < offsetof(GhosttyTerminalProgressReport, progress) +
                                              sizeof(GhosttyTerminalProgressReport::progress)) {
    return;
  }
  auto& signals = impl.signals;
  signals.progress = progress_state(report->state);
  signals.progress_percent =
      signals.progress != ProgressState::none && report->progress >= 0 && report->progress <= 100
          ? std::optional{static_cast<std::uint8_t>(report->progress)}
          : std::nullopt;
}

void Terminal::Impl::semantic_prompt([[maybe_unused]] GhosttyTerminal terminal_handle,
                                     void* userdata,
                                     const GhosttyTerminalSemanticPrompt* prompt) noexcept {
  if (prompt == nullptr || prompt->size < offsetof(GhosttyTerminalSemanticPrompt, exit_code) +
                                              sizeof(GhosttyTerminalSemanticPrompt::exit_code)) {
    return;
  }
  if (prompt->action < GHOSTTY_TERMINAL_SEMANTIC_PROMPT_FRESH_LINE ||
      prompt->action > GHOSTTY_TERMINAL_SEMANTIC_PROMPT_COMMAND_END) {
    return;
  }
  auto& impl = *static_cast<Impl*>(userdata);
  auto& signals = impl.signals;
  switch (prompt->action) {
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_NEW_PROMPT:
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_NEW_COMMAND:
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_PROMPT_START:
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_INPUT_START:
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_INPUT_START_EOL:
    // Prompt redraws repeat these markers; only a state change is an observable signal.
    if (signals.command == CommandState::prompt) {
      return;
    }
    signals.command = CommandState::prompt;
    break;
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_OUTPUT_START:
    if (signals.command == CommandState::running) {
      return;
    }
    signals.command = CommandState::running;
    break;
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_COMMAND_END:
    signals.command = CommandState::finished;
    signals.exit_code = prompt->has_exit_code ? std::optional{prompt->exit_code} : std::nullopt;
    saturating_increment(signals.commands);
    break;
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_FRESH_LINE:
  case GHOSTTY_TERMINAL_SEMANTIC_PROMPT_MAX_VALUE:
    return;
  }
  saturating_increment(impl.effects.command_transitions);
}

void Terminal::Impl::unknown_sequence([[maybe_unused]] GhosttyTerminal terminal_handle,
                                      void* userdata,
                                      const GhosttyTerminalUnknownSequence* sequence) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  if (impl.effects.unknown_sequences_dropped < std::numeric_limits<std::uint64_t>::max()) {
    ++impl.effects.unknown_sequences_dropped;
  }
  if (sequence != nullptr && sequence->tag == GHOSTTY_TERMINAL_UNKNOWN_SEQUENCE_APC) {
    impl.effects.unknown_sequence_truncated =
        impl.effects.unknown_sequence_truncated || sequence->value.apc.truncated;
  }
}

auto Terminal::Impl::enquiry([[maybe_unused]] GhosttyTerminal terminal_handle,
                             [[maybe_unused]] void* userdata) noexcept -> GhosttyString {
  static constexpr std::array<std::uint8_t, 5> identity{'l', 'e', 'm', 'm', 'a'};
  return {.ptr = identity.data(), .len = identity.size()};
}

auto Terminal::Impl::color_scheme([[maybe_unused]] GhosttyTerminal terminal_handle, void* userdata,
                                  GhosttyColorScheme* const output) noexcept -> bool {
  if (output == nullptr) {
    return false;
  }
  const auto background = static_cast<Impl*>(userdata)->session_theme.background;
  const auto luminance =
      (299U * background.red) + (587U * background.green) + (114U * background.blue);
  *output = luminance >= 128'000U ? GHOSTTY_COLOR_SCHEME_LIGHT : GHOSTTY_COLOR_SCHEME_DARK;
  return true;
}

auto Terminal::Impl::device_attributes([[maybe_unused]] GhosttyTerminal terminal_handle,
                                       [[maybe_unused]] void* userdata,
                                       GhosttyDeviceAttributes* const output) noexcept -> bool {
  if (output == nullptr) {
    return false;
  }
  *output = {};
  output->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_LEVEL_2;
  output->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
  output->primary.num_features = 1;
  output->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
  return true;
}

auto Terminal::Impl::size_report([[maybe_unused]] GhosttyTerminal terminal_handle, void* userdata,
                                 GhosttySizeReportSize* const output) noexcept -> bool {
  if (output == nullptr) {
    return false;
  }
  const auto size = static_cast<Impl*>(userdata)->options.size;
  *output = {
      .rows = size.rows,
      .columns = size.columns,
      .cell_width = size.cell_width_px,
      .cell_height = size.cell_height_px,
  };
  return true;
}

auto Terminal::Impl::xtversion([[maybe_unused]] GhosttyTerminal terminal_handle,
                               [[maybe_unused]] void* userdata) noexcept -> GhosttyString {
  static constexpr std::array<std::uint8_t, 5> identity{'l', 'e', 'm', 'm', 'a'};
  return {.ptr = identity.data(), .len = identity.size()};
}

auto Terminal::title() const noexcept -> std::expected<std::string_view, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);

  GhosttyString title{};
  const auto result = ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  // Ghostty exposes UTF-8 as uint8_t while string_view uses char.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return std::string_view(reinterpret_cast<const char*>(title.ptr), title.len);
}

auto Terminal::pwd() const noexcept -> std::expected<std::string_view, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);

  GhosttyString pwd{};
  const auto result = ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_PWD, &pwd);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  // Ghostty exposes UTF-8 as uint8_t while string_view uses char.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return std::string_view(reinterpret_cast<const char*>(pwd.ptr), pwd.len);
}

auto Terminal::signals() const noexcept -> const TerminalSignals& {
  LEMMA_ASSERT(impl_ != nullptr);
  return impl_->signals;
}

auto Terminal::take_effects() noexcept -> EffectBatch {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);

  const auto effects = impl_->effects;
  impl_->effects = {};
  return effects;
}

auto Terminal::pending_pty_response_bytes() const noexcept -> std::size_t {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  return impl_->pty_responses.size() - impl_->pty_response_offset +
         impl_->clipboard_responses.size() - impl_->clipboard_response_offset;
}

auto Terminal::pty_response_overflowed() const noexcept -> bool {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  return impl_->pty_response_integrity_failed;
}

auto Terminal::read_pty_responses(const std::span<std::byte> output) noexcept -> std::size_t {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  const auto used = drain_responses(impl_->pty_responses, impl_->pty_response_offset, output);
  return used + drain_responses(impl_->clipboard_responses, impl_->clipboard_response_offset,
                                output.subspan(used));
}

} // namespace lemma::vt
