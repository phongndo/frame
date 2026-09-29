#include "client/host_input_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace {

struct Transcript final {
  std::uint64_t hash{14'695'981'039'346'656'037ULL};
  std::size_t events{0};
  std::optional<frame::client::HostInputKind> streaming_kind;

  void mix(const std::uint64_t value) noexcept {
    hash ^= value;
    hash *= 1'099'511'628'211ULL;
  }

  void bytes(const std::span<const std::byte> value) noexcept {
    for (const auto byte : value) {
      mix(std::to_integer<std::uint8_t>(byte));
    }
  }

  void event(const frame::client::HostInputEvent& value,
             const std::span<const std::byte> output) noexcept {
    using frame::client::HostInputKind;
    const bool streaming = value.kind == HostInputKind::ordinary ||
                           value.kind == HostInputKind::paste ||
                           value.kind == HostInputKind::terminal_reply_stream;
    if (!streaming || streaming_kind != value.kind) {
      mix(0x100U + static_cast<std::uint8_t>(value.kind));
      ++events;
    }
    streaming_kind = streaming ? std::optional{value.kind} : std::nullopt;
    if (value.offset <= output.size() && value.size <= output.size() - value.offset) {
      bytes(output.subspan(value.offset, value.size));
    } else {
      mix(std::numeric_limits<std::uint64_t>::max());
    }
    switch (value.kind) {
    case HostInputKind::ordinary:
    case HostInputKind::paste:
    case HostInputKind::terminal_reply:
    case HostInputKind::terminal_reply_stream:
    case HostInputKind::theme_reply:
    case HostInputKind::clipboard_supported:
    case HostInputKind::clipboard_unsupported:
      break;
    case HostInputKind::window_size:
      mix(value.window_size.columns);
      mix(value.window_size.rows);
      mix(value.window_size.cell_width_px);
      mix(value.window_size.cell_height_px);
      break;
    case HostInputKind::key:
      mix(static_cast<std::uint8_t>(value.key.action));
      mix(static_cast<std::uint8_t>(value.key.key));
      mix(value.key.modifiers);
      mix(value.key.consumed_modifiers);
      mix(value.key.unshifted_codepoint);
      mix(static_cast<std::uint64_t>(value.key.composing));
      break;
    case HostInputKind::focus:
      mix(static_cast<std::uint8_t>(value.focus));
      break;
    case HostInputKind::mouse:
      mix(static_cast<std::uint8_t>(value.mouse.action));
      mix(static_cast<std::uint8_t>(value.mouse.button));
      mix(value.mouse.modifiers);
      mix(value.mouse.column);
      mix(value.mouse.row);
      mix(value.mouse.geometry.columns);
      mix(value.mouse.geometry.rows);
      mix(static_cast<std::uint64_t>(value.mouse.any_button_pressed));
      break;
    }
  }

  [[nodiscard]] constexpr auto operator==(const Transcript&) const noexcept -> bool = default;
};

[[nodiscard]] auto parse_transcript(const std::span<const std::byte> input,
                                    frame::protocol::Dimensions geometry,
                                    const std::size_t chunk_max) -> std::optional<Transcript> {
  frame::client::HostInputParser parser;
  if (!parser.prepare().has_value()) {
    return std::nullopt;
  }
  // Runtime-sized bounded fuzz output cannot use std::array.
  // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  auto output =
      std::make_unique_for_overwrite<std::byte[]>(frame::client::host_input_output_bytes_max);
  // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  Transcript transcript;
  const auto output_span = std::span(output.get(), frame::client::host_input_output_bytes_max);
  std::size_t offset = 0;
  while (offset < input.size()) {
    const auto copied = std::min(chunk_max, input.size() - offset);
    const auto parsed = parser.parse(input.subspan(offset, copied), output_span, geometry);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    const auto bytes = output_span.first(parsed->bytes);
    for (const auto& event : std::span(parsed->events).first(parsed->event_count)) {
      transcript.event(event, bytes);
      if (event.kind == frame::client::HostInputKind::window_size) {
        geometry = {.columns = event.window_size.columns, .rows = event.window_size.rows};
      }
    }
    offset += copied;
  }
  const auto flushed = parser.flush_pending(output_span);
  if (!flushed.has_value()) {
    return std::nullopt;
  }
  const auto bytes = output_span.first(flushed->bytes);
  for (const auto& event : std::span(flushed->events).first(flushed->event_count)) {
    transcript.event(event, bytes);
  }
  return transcript;
}

} // namespace

// libFuzzer owns this ABI name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
  if (data == nullptr || size < 3 || size > frame::protocol::input_message_bytes_max) {
    return 0;
  }
  const auto source = std::span(data, size);
  const auto input = std::as_bytes(source).subspan(3);
  const auto chunk_max = 1U + (static_cast<std::size_t>(source.front()) % 128U);
  const frame::protocol::Dimensions geometry{
      .columns = static_cast<std::uint16_t>(1U + (source.subspan(1, 1).front() % 250U)),
      .rows = static_cast<std::uint16_t>(1U + (source.subspan(2, 1).front() % 100U)),
  };
  const auto fragmented = parse_transcript(input, geometry, chunk_max);
  const auto whole = parse_transcript(input, geometry, std::max<std::size_t>(input.size(), 1U));
  if (fragmented.has_value() != whole.has_value() ||
      (fragmented.has_value() && *fragmented != *whole)) {
    __builtin_trap();
  }
  return 0;
}
