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
  if (impl.effects.bells < std::numeric_limits<std::uint64_t>::max()) {
    ++impl.effects.bells;
  }
}

void Terminal::Impl::title_changed([[maybe_unused]] GhosttyTerminal terminal_handle,
                                   void* userdata) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  if (impl.effects.title_changes < std::numeric_limits<std::uint64_t>::max()) {
    ++impl.effects.title_changes;
  }
}

void Terminal::Impl::pwd_changed([[maybe_unused]] GhosttyTerminal terminal_handle,
                                 void* userdata) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  if (impl.effects.pwd_changes < std::numeric_limits<std::uint64_t>::max()) {
    ++impl.effects.pwd_changes;
  }
}

void Terminal::Impl::desktop_notification(
    [[maybe_unused]] GhosttyTerminal terminal_handle, void* userdata,
    [[maybe_unused]] const GhosttyTerminalDesktopNotification* notification) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  if (impl.effects.desktop_notifications < std::numeric_limits<std::uint64_t>::max()) {
    ++impl.effects.desktop_notifications;
  }
}

void Terminal::Impl::progress_report(
    [[maybe_unused]] GhosttyTerminal terminal_handle, void* userdata,
    [[maybe_unused]] const GhosttyTerminalProgressReport* report) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  if (impl.effects.progress_reports < std::numeric_limits<std::uint64_t>::max()) {
    ++impl.effects.progress_reports;
  }
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
