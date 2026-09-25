#ifndef LEMMA_CLIENT_HOST_INPUT_PARSER_HPP
#define LEMMA_CLIENT_HOST_INPUT_PARSER_HPP

#include "lemma/limits.hpp"
#include "platform/terminal_mode.hpp"
#include "protocol/attachment.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace lemma::client {

enum class HostInputKind : std::uint8_t {
  ordinary,
  paste,
  key,
  focus,
  mouse,
  terminal_reply,
  terminal_reply_stream,
  theme_reply,
  window_size,
  clipboard_supported,
  clipboard_unsupported,
};

struct HostInputEvent final {
  HostInputKind kind{HostInputKind::ordinary};
  protocol::FocusInput focus{protocol::FocusInput::lost};
  protocol::MouseInput mouse{};
  std::size_t offset{0};
  std::size_t size{0};
  protocol::KeyInput key{};
  platform::WindowSize window_size{};
};

inline constexpr std::size_t host_input_events_max = (protocol::input_bytes_max / 2U) + 1U;

// Typed sequences consume at least three physical bytes; alternating one-byte ordinary runs give
// the strictest event count, with one extra slot for a paste completed from an earlier read.
using HostInputEvents = std::array<HostInputEvent, host_input_events_max>;
static_assert(sizeof(HostInputEvents) < std::size_t{128} * 1'024U);

struct HostInputBatch final {
  // Borrowed from the parser's prepared storage until its next parse or flush; only the first
  // event_count entries belong to this batch. Every keystroke returns a batch, so it must not
  // construct or copy the worst-case event array.
  std::span<const HostInputEvent> events;
  std::size_t event_count{0};
  std::size_t bytes{0};
};

static_assert(sizeof(HostInputBatch) <= 4U * sizeof(std::size_t));

enum class HostInputError : std::uint8_t {
  output_exhausted,
  event_limit,
  allocation_failed,
  not_prepared,
  incomplete_terminal_reply,
  incomplete_terminal_report,
  invalid_size_report,
  invalid_terminal_report,
};

inline constexpr std::size_t host_input_output_bytes_max =
    limits::structured_input_payload_bytes_max + (protocol::input_bytes_max * 2U);

// Preserves bracketed paste, focus, and SGR mouse boundaries across arbitrary read fragmentation.
// Unknown input remains ordinary bytes. Recognized mouse, size, and clipboard reports never become
// keys: stale mouse coordinates are discarded; incomplete reports fail closed at their deadline.
class HostInputParser final {
public:
  [[nodiscard]] auto prepare() noexcept -> std::expected<void, HostInputError>;
  [[nodiscard]] auto parse(std::span<const std::byte> input, std::span<std::byte> output,
                           protocol::Dimensions geometry) noexcept
      -> std::expected<HostInputBatch, HostInputError>;
  [[nodiscard]] auto flush_pending(std::span<std::byte> output) noexcept
      -> std::expected<HostInputBatch, HostInputError>;
  [[nodiscard]] auto has_pending_sequence() const noexcept -> bool { return pending_size_ > 0; }
  [[nodiscard]] auto paste_active() const noexcept -> bool { return paste_active_; }
  // Identity of the current recognized transport report, stable until it completes or fails.
  [[nodiscard]] auto pending_report() const noexcept -> std::optional<std::uint64_t>;

private:
  enum class Report : std::uint8_t {
    none,
    clipboard,
    legacy_clipboard,
    clipboard_support,
    theme,
    mouse,
    size
  };

  // Decimal Kitty associated-text codepoints can be much larger than their decoded UTF-8.
  static constexpr std::size_t sequence_bytes_max = protocol::terminal_reply_bytes_max;

  std::array<std::byte, sequence_bytes_max> pending_{};
  // One bounded opaque paste is retained until its end marker arrives, independent of reads.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::unique_ptr<std::byte[]> paste_storage_;
  std::unique_ptr<HostInputEvents> events_;
  std::uint64_t report_generation_{0};
  std::size_t pending_size_{0};
  std::size_t legacy_report_bytes_{0};
  std::size_t paste_size_{0};
  bool paste_active_{false};
  Report report_{Report::none};
  bool any_button_pressed_{false};
};

} // namespace lemma::client

#endif // LEMMA_CLIENT_HOST_INPUT_PARSER_HPP
