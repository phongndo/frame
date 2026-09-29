#include "protocol/attachment.hpp"

#include "frame/assert.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

// Fixed wire offsets are checked against compile-time-sized headers and validated payload bounds.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

namespace frame::protocol {
namespace {

void encode_u16(const std::uint16_t value, const std::span<std::byte, 2> output) noexcept {
  output.front() = static_cast<std::byte>(value >> 8U);
  output.back() = static_cast<std::byte>(value & 0xFFU);
}

void encode_u32(const std::uint32_t value, const std::span<std::byte, 4> output) noexcept {
  output[0] = static_cast<std::byte>(value >> 24U);
  output[1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
  output[2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  output[3] = static_cast<std::byte>(value & 0xFFU);
}

[[nodiscard]] constexpr auto decode_u16(const std::byte high, const std::byte low) noexcept
    -> std::uint16_t {
  const auto high_value = std::to_integer<std::uint16_t>(high);
  const auto low_value = std::to_integer<std::uint16_t>(low);
  return static_cast<std::uint16_t>((high_value << 8U) | low_value);
}

[[nodiscard]] constexpr auto decode_u32(const std::span<const std::byte, 4> bytes) noexcept
    -> std::uint32_t {
  return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
         (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
         std::to_integer<std::uint32_t>(bytes[3]);
}

[[nodiscard]] auto valid_dimensions(const Dimensions dimensions) noexcept -> bool {
  return dimensions.columns > 0 && dimensions.rows > 0 && dimensions.columns <= columns_max &&
         dimensions.rows <= rows_max;
}

[[nodiscard]] auto valid_session_name(const std::string_view session) noexcept -> bool {
  return !session.empty() && session.front() != '-' && session.size() <= session_name_bytes_max &&
         std::ranges::all_of(session, [](const char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '_' || character == '-';
         });
}

constexpr std::uint8_t hello_host_theme_flag = 0x01;
constexpr std::uint8_t host_theme_foreground_flag = 0x01;
constexpr std::uint8_t host_theme_background_flag = 0x02;
constexpr std::uint8_t host_theme_selection_foreground_flag = 0x04;
constexpr std::uint8_t host_theme_selection_background_flag = 0x08;
constexpr std::uint8_t host_theme_flags_mask =
    host_theme_foreground_flag | host_theme_background_flag | host_theme_selection_foreground_flag |
    host_theme_selection_background_flag;

void encode_rgb(const RgbColor color, const std::span<std::byte, 3> output) noexcept {
  output[0] = static_cast<std::byte>(color.red);
  output[1] = static_cast<std::byte>(color.green);
  output[2] = static_cast<std::byte>(color.blue);
}

[[nodiscard]] constexpr auto decode_rgb(const std::span<const std::byte, 3> input) noexcept
    -> RgbColor {
  return {
      .red = std::to_integer<std::uint8_t>(input[0]),
      .green = std::to_integer<std::uint8_t>(input[1]),
      .blue = std::to_integer<std::uint8_t>(input[2]),
  };
}

void encode_host_theme(const HostTerminalTheme& theme,
                       const std::span<std::byte, host_theme_wire_bytes> output) noexcept {
  output.front() = static_cast<std::byte>(
      (theme.foreground.has_value() ? host_theme_foreground_flag : 0U) |
      (theme.background.has_value() ? host_theme_background_flag : 0U) |
      (theme.selection_foreground.has_value() ? host_theme_selection_foreground_flag : 0U) |
      (theme.selection_background.has_value() ? host_theme_selection_background_flag : 0U));
  auto mask = output.subspan<1, host_theme_palette_mask_bytes>();
  for (std::size_t index = 0; index < theme.palette.size(); ++index) {
    if (theme.has_palette_color(index)) {
      mask[index / 8U] |= static_cast<std::byte>(std::uint8_t{1} << (index % 8U));
    }
  }
  encode_rgb(theme.foreground.value_or(RgbColor{}), output.subspan<3, 3>());
  encode_rgb(theme.background.value_or(RgbColor{}), output.subspan<6, 3>());
  auto palette = output.subspan<9, host_theme_palette_colors * 3U>();
  for (std::size_t index = 0; index < theme.palette.size(); ++index) {
    encode_rgb(std::span(theme.palette).subspan(index, 1).front(),
               palette.subspan(index * 3U).first<3>());
  }
  encode_rgb(theme.selection_foreground.value_or(RgbColor{}),
             output.subspan<9 + (host_theme_palette_colors * 3U), 3>());
  encode_rgb(theme.selection_background.value_or(RgbColor{}),
             output.subspan<12 + (host_theme_palette_colors * 3U), 3>());
}

[[nodiscard]] auto
decode_host_theme(const std::span<const std::byte, host_theme_wire_bytes> input) noexcept
    -> std::expected<HostTerminalTheme, DecodeError> {
  const auto flags = std::to_integer<std::uint8_t>(input.front());
  if ((flags & ~host_theme_flags_mask) != 0) {
    return std::unexpected(DecodeError::invalid_flags);
  }
  HostTerminalTheme theme;
  if ((flags & host_theme_foreground_flag) != 0) {
    theme.foreground = decode_rgb(input.subspan<3, 3>());
  }
  if ((flags & host_theme_background_flag) != 0) {
    theme.background = decode_rgb(input.subspan<6, 3>());
  }
  const auto mask = input.subspan<1, host_theme_palette_mask_bytes>();
  const auto palette = input.subspan<9, host_theme_palette_colors * 3U>();
  for (std::size_t index = 0; index < theme.palette.size(); ++index) {
    if ((std::to_integer<std::uint8_t>(mask[index / 8U]) & (std::uint8_t{1} << (index % 8U))) !=
        0) {
      theme.set_palette_color(index, decode_rgb(palette.subspan(index * 3U).first<3>()));
    }
  }
  if ((flags & host_theme_selection_foreground_flag) != 0) {
    theme.selection_foreground =
        decode_rgb(input.subspan<9 + (host_theme_palette_colors * 3U), 3>());
  }
  if ((flags & host_theme_selection_background_flag) != 0) {
    theme.selection_background =
        decode_rgb(input.subspan<12 + (host_theme_palette_colors * 3U), 3>());
  }
  if (theme.empty()) {
    return std::unexpected(DecodeError::invalid_length);
  }
  return theme;
}

[[nodiscard]] auto pane_command(const std::byte encoded) noexcept -> std::optional<PaneCommand> {
  const auto command = static_cast<PaneCommand>(std::to_integer<std::uint8_t>(encoded));
  switch (command) {
  case PaneCommand::none:
    break;
  case PaneCommand::split_left_right:
  case PaneCommand::split_top_bottom:
  case PaneCommand::resize_left:
  case PaneCommand::resize_right:
  case PaneCommand::resize_up:
  case PaneCommand::resize_down:
  case PaneCommand::focus_left:
  case PaneCommand::focus_right:
  case PaneCommand::focus_up:
  case PaneCommand::focus_down:
  case PaneCommand::focus_next:
  case PaneCommand::focus_previous:
  case PaneCommand::close:
  case PaneCommand::zoom:
  case PaneCommand::enter_copy_mode:
  case PaneCommand::enter_copy_search_forward:
  case PaneCommand::enter_copy_search_backward:
  case PaneCommand::create_tab:
  case PaneCommand::next_tab:
  case PaneCommand::previous_tab:
  case PaneCommand::begin_rename_session:
  case PaneCommand::begin_rename_tab:
  case PaneCommand::move_tab_left:
  case PaneCommand::move_tab_right:
  case PaneCommand::swap_pane_left:
  case PaneCommand::swap_pane_right:
  case PaneCommand::swap_pane_up:
  case PaneCommand::swap_pane_down:
  case PaneCommand::kill_tab:
  case PaneCommand::select_tab_0:
  case PaneCommand::select_tab_1:
  case PaneCommand::select_tab_2:
  case PaneCommand::select_tab_3:
  case PaneCommand::select_tab_4:
  case PaneCommand::select_tab_5:
  case PaneCommand::select_tab_6:
  case PaneCommand::select_tab_7:
  case PaneCommand::select_tab_8:
  case PaneCommand::select_tab_9:
    return command;
  }
  return std::nullopt;
}

[[nodiscard]] auto focus_input(const std::byte encoded) noexcept -> std::optional<FocusInput> {
  const auto focus = static_cast<FocusInput>(std::to_integer<std::uint8_t>(encoded));
  switch (focus) {
  case FocusInput::lost:
  case FocusInput::gained:
    return focus;
  }
  return std::nullopt;
}

[[nodiscard]] auto mouse_action(const std::byte encoded) noexcept
    -> std::optional<MouseInputAction> {
  const auto action = static_cast<MouseInputAction>(std::to_integer<std::uint8_t>(encoded));
  switch (action) {
  case MouseInputAction::press:
  case MouseInputAction::release:
  case MouseInputAction::motion:
    return action;
  }
  return std::nullopt;
}

[[nodiscard]] auto mouse_button(const std::byte encoded) noexcept
    -> std::optional<MouseInputButton> {
  const auto button = static_cast<MouseInputButton>(std::to_integer<std::uint8_t>(encoded));
  switch (button) {
  case MouseInputButton::none:
  case MouseInputButton::left:
  case MouseInputButton::right:
  case MouseInputButton::middle:
  case MouseInputButton::four:
  case MouseInputButton::five:
  case MouseInputButton::six:
  case MouseInputButton::seven:
  case MouseInputButton::eight:
  case MouseInputButton::nine:
  case MouseInputButton::ten:
  case MouseInputButton::eleven:
    return button;
  }
  return std::nullopt;
}

[[nodiscard]] auto disconnect_reason(const std::byte encoded) noexcept
    -> std::optional<DisconnectReason> {
  const auto reason = static_cast<DisconnectReason>(std::to_integer<std::uint8_t>(encoded));
  switch (reason) {
  case DisconnectReason::normal:
  case DisconnectReason::protocol_error:
  case DisconnectReason::version_mismatch:
  case DisconnectReason::session_busy:
  case DisconnectReason::session_missing:
  case DisconnectReason::capacity:
  case DisconnectReason::setup_failed:
  case DisconnectReason::frame_timeout:
  case DisconnectReason::daemon_shutdown:
  case DisconnectReason::internal_error:
    return reason;
  }
  return std::nullopt;
}

[[nodiscard]] auto message_kind(const std::byte encoded) noexcept -> std::optional<MessageKind> {
  const auto kind = static_cast<MessageKind>(std::to_integer<std::uint8_t>(encoded));
  switch (kind) {
  case MessageKind::hello:
  case MessageKind::input:
  case MessageKind::resize:
  case MessageKind::pane_command:
  case MessageKind::detach:
  case MessageKind::render_frame:
  case MessageKind::disconnect:
  case MessageKind::host_theme:
  case MessageKind::paste:
  case MessageKind::key:
  case MessageKind::focus:
  case MessageKind::mouse:
  case MessageKind::terminal_reply:
  case MessageKind::cell_size:
  case MessageKind::clipboard_support:
    return kind;
  }
  return std::nullopt;
}

[[nodiscard]] auto valid_diagnostic(const std::span<const std::byte> bytes) noexcept -> bool {
  return std::ranges::all_of(bytes, [](const std::byte byte) {
    const auto value = std::to_integer<std::uint8_t>(byte);
    return value >= 0x20U && value <= 0x7EU;
  });
}

struct Envelope final {
  MessageKind kind{MessageKind::disconnect};
  std::uint8_t flags{0};
  std::size_t payload_bytes{0};
  std::uint32_t sequence{0};
};

[[nodiscard]] auto decode_envelope(const std::span<const std::byte> buffered,
                                   const std::uint32_t expected_sequence) noexcept
    -> std::expected<std::optional<Envelope>, DecodeError> {
  if (buffered.size() < attach_header_bytes) {
    return std::optional<Envelope>{};
  }
  if (!std::ranges::equal(buffered.first(attach_magic.size()), attach_magic)) {
    return std::unexpected(DecodeError::invalid_magic);
  }
  if (std::to_integer<std::uint8_t>(buffered[4]) != current_version.major ||
      std::to_integer<std::uint8_t>(buffered[5]) != current_version.minor) {
    return std::unexpected(DecodeError::version_mismatch);
  }
  const auto kind = message_kind(buffered[6]);
  if (!kind.has_value()) {
    return std::unexpected(DecodeError::invalid_kind);
  }
  const auto payload = decode_u32(std::span(buffered).subspan<8, 4>());
  const auto sequence = decode_u32(std::span(buffered).subspan<12, 4>());
  if (sequence == 0 || sequence != expected_sequence) {
    return std::unexpected(DecodeError::invalid_sequence);
  }
  return Envelope{
      .kind = *kind,
      .flags = std::to_integer<std::uint8_t>(buffered[7]),
      .payload_bytes = payload,
      .sequence = sequence,
  };
}

void copy_header(const std::array<std::byte, attach_header_bytes>& header,
                 const std::span<std::byte> storage, std::size_t& size) noexcept {
  std::ranges::copy(header, storage.begin());
  size = header.size();
}

[[nodiscard]] auto next_sequence(const std::uint32_t sequence) noexcept -> std::uint32_t {
  return sequence == std::numeric_limits<std::uint32_t>::max() ? 0U : sequence + 1U;
}

} // namespace

[[nodiscard]] auto encode_session_header(const ControlCommand command,
                                         const std::string_view session) noexcept
    -> std::array<std::byte, 2> {
  FRAME_ASSERT(!session.empty());
  FRAME_ASSERT(session.size() <= session_name_bytes_max);
  return {wire_byte(command), static_cast<std::byte>(session.size())};
}

[[nodiscard]] auto encode_dimensions(const Dimensions dimensions) noexcept
    -> std::array<std::byte, 4> {
  std::array<std::byte, 4> packet{};
  encode_u16(dimensions.columns, std::span(packet).subspan<0, 2>());
  encode_u16(dimensions.rows, std::span(packet).subspan<2, 2>());
  return packet;
}

[[nodiscard]] auto encode_bounded_size(const std::size_t size) noexcept
    -> std::array<std::byte, 2> {
  FRAME_ASSERT(size <= std::numeric_limits<std::uint16_t>::max());
  std::array<std::byte, 2> encoded{};
  encode_u16(static_cast<std::uint16_t>(size), encoded);
  return encoded;
}

[[nodiscard]] auto decode_bounded_size(const std::span<const std::byte, 2> bytes) noexcept
    -> std::size_t {
  return decode_u16(bytes.front(), bytes.back());
}

[[nodiscard]] auto decode_dimensions(const std::span<const std::byte, 4> bytes) noexcept
    -> Dimensions {
  return {
      .columns = decode_u16(bytes.front(), bytes.subspan<1>().front()),
      .rows = decode_u16(bytes.subspan<2>().front(), bytes.subspan<3>().front()),
  };
}

[[nodiscard]] auto encode_header(const MessageKind kind, const std::uint8_t flags,
                                 const std::uint32_t payload_bytes, const std::uint32_t sequence,
                                 const ProtocolVersion version) noexcept
    -> std::array<std::byte, attach_header_bytes> {
  FRAME_ASSERT(sequence != 0);
  std::array<std::byte, attach_header_bytes> header{};
  std::ranges::copy(attach_magic, header.begin());
  header[4] = static_cast<std::byte>(version.major);
  header[5] = static_cast<std::byte>(version.minor);
  header[6] = static_cast<std::byte>(kind);
  header[7] = static_cast<std::byte>(flags);
  encode_u32(payload_bytes, std::span(header).subspan<8, 4>());
  encode_u32(sequence, std::span(header).subspan<12, 4>());
  return header;
}

[[nodiscard]] auto encode_client_hello(const std::string_view session, const Dimensions dimensions,
                                       const std::uint32_t sequence, const ProtocolVersion version,
                                       const std::optional<HostTerminalTheme>& host_theme) noexcept
    -> SmallMessage {
  FRAME_ASSERT(session.size() <= session_name_bytes_max);
  FRAME_ASSERT(!host_theme.has_value() || !host_theme->empty());
  SmallMessage message;
  const auto theme_bytes = host_theme.has_value() ? host_theme_wire_bytes : 0U;
  const auto payload_bytes = static_cast<std::uint32_t>(6U + theme_bytes + session.size());
  copy_header(encode_header(MessageKind::hello, 0, payload_bytes, sequence, version),
              message.storage_, message.size_);
  auto output = std::span(message.storage_).subspan(message.size_);
  output.front() = static_cast<std::byte>(session.size());
  const auto dimensions_bytes = encode_dimensions(dimensions);
  std::ranges::copy(dimensions_bytes, output.subspan<1, 4>().begin());
  output.subspan<5, 1>().front() =
      host_theme.has_value() ? static_cast<std::byte>(hello_host_theme_flag) : std::byte{0};
  if (host_theme.has_value()) {
    encode_host_theme(*host_theme, output.subspan<6, host_theme_wire_bytes>());
  }
  std::ranges::copy(std::as_bytes(std::span(session.data(), session.size())),
                    output.subspan(6U + theme_bytes).begin());
  message.size_ += payload_bytes;
  return message;
}

[[nodiscard]] auto encode_daemon_hello(const Dimensions dimensions,
                                       const std::uint32_t sequence) noexcept -> SmallMessage {
  FRAME_ASSERT(valid_dimensions(dimensions));
  SmallMessage message;
  copy_header(encode_header(MessageKind::hello, 0, 4, sequence), message.storage_, message.size_);
  const auto encoded = encode_dimensions(dimensions);
  std::ranges::copy(encoded, std::span(message.storage_).subspan(message.size_).begin());
  message.size_ += encoded.size();
  return message;
}

[[nodiscard]] auto encode_input_header(const std::size_t bytes,
                                       const std::uint32_t sequence) noexcept
    -> std::array<std::byte, attach_header_bytes> {
  FRAME_ASSERT(bytes > 0);
  FRAME_ASSERT(bytes <= legacy_input_message_bytes_max);
  return encode_header(MessageKind::input, 0, static_cast<std::uint32_t>(bytes), sequence);
}

[[nodiscard]] auto encode_clipboard_support(const bool supported,
                                            const std::uint32_t sequence) noexcept -> SmallMessage {
  SmallMessage message;
  copy_header(encode_header(MessageKind::clipboard_support, 0, 1, sequence), message.storage_,
              message.size_);
  message.storage_.at(message.size_++) = supported ? std::byte{1} : std::byte{0};
  return message;
}

[[nodiscard]] auto encode_cell_size(const CellSize size, const std::uint32_t sequence) noexcept
    -> SmallMessage {
  FRAME_ASSERT(size.width > 0 && size.width <= 500 && size.height > 0 && size.height <= 200);
  SmallMessage message;
  copy_header(encode_header(MessageKind::cell_size, 0, 4, sequence), message.storage_,
              message.size_);
  const auto encoded = encode_dimensions({.columns = size.width, .rows = size.height});
  std::ranges::copy(encoded, std::span(message.storage_).subspan(message.size_).begin());
  message.size_ += encoded.size();
  return message;
}

[[nodiscard]] auto encode_terminal_reply_header(const std::size_t bytes,
                                                const std::uint32_t sequence) noexcept
    -> std::array<std::byte, attach_header_bytes> {
  FRAME_ASSERT(bytes > 0 && bytes <= terminal_reply_bytes_max);
  return encode_header(MessageKind::terminal_reply, 0, static_cast<std::uint32_t>(bytes), sequence);
}

[[nodiscard]] auto encode_paste_header(const std::size_t bytes,
                                       const std::uint32_t sequence) noexcept
    -> std::array<std::byte, attach_header_bytes> {
  FRAME_ASSERT(bytes > 0);
  FRAME_ASSERT(bytes <= input_message_bytes_max);
  return encode_header(MessageKind::paste, 0, static_cast<std::uint32_t>(bytes), sequence);
}

[[nodiscard]] auto encode_key(const KeyInput& key, const std::span<const std::byte> text,
                              const std::uint32_t sequence) noexcept -> SmallMessage {
  FRAME_ASSERT(text.size() <= key_input_text_bytes_max);
  const auto payload_size = key_input_wire_fixed_bytes + text.size();
  SmallMessage message;
  copy_header(
      encode_header(MessageKind::key, 0, static_cast<std::uint32_t>(payload_size), sequence),
      message.storage_, message.size_);
  auto output = std::span(message.storage_).subspan(message.size_, payload_size);
  output[0] = static_cast<std::byte>(key.action);
  output[1] = static_cast<std::byte>(key.key);
  encode_u16(key.modifiers, output.subspan<2, 2>());
  encode_u16(key.consumed_modifiers, output.subspan<4, 2>());
  encode_u32(key.unshifted_codepoint, output.subspan<6, 4>());
  output[10] = key.composing ? std::byte{1} : std::byte{0};
  std::ranges::copy(text, output.subspan(key_input_wire_fixed_bytes).begin());
  message.size_ += payload_size;
  return message;
}

[[nodiscard]] auto encode_focus(const FocusInput focus, const std::uint32_t sequence) noexcept
    -> SmallMessage {
  SmallMessage message;
  copy_header(encode_header(MessageKind::focus, 0, 1, sequence), message.storage_, message.size_);
  std::span(message.storage_).subspan(message.size_, 1).front() = static_cast<std::byte>(focus);
  ++message.size_;
  return message;
}

[[nodiscard]] auto encode_mouse(const MouseInput& mouse, const std::uint32_t sequence) noexcept
    -> SmallMessage {
  FRAME_ASSERT(valid_dimensions(mouse.geometry));
  FRAME_ASSERT(mouse.column < mouse.geometry.columns);
  FRAME_ASSERT(mouse.row < mouse.geometry.rows);
  SmallMessage message;
  copy_header(encode_header(MessageKind::mouse, 0, mouse_input_wire_bytes, sequence),
              message.storage_, message.size_);
  auto output = std::span(message.storage_).subspan(message.size_, mouse_input_wire_bytes);
  output[0] = static_cast<std::byte>(mouse.action);
  output[1] = static_cast<std::byte>(mouse.button);
  encode_u16(mouse.modifiers, output.subspan<2, 2>());
  encode_u16(mouse.column, output.subspan<4, 2>());
  encode_u16(mouse.row, output.subspan<6, 2>());
  encode_u16(mouse.geometry.columns, output.subspan<8, 2>());
  encode_u16(mouse.geometry.rows, output.subspan<10, 2>());
  output[12] = mouse.any_button_pressed ? std::byte{1} : std::byte{0};
  message.size_ += mouse_input_wire_bytes;
  return message;
}

[[nodiscard]] auto encode_resize(const Dimensions dimensions, const std::uint32_t sequence) noexcept
    -> SmallMessage {
  FRAME_ASSERT(valid_dimensions(dimensions));
  SmallMessage message;
  copy_header(encode_header(MessageKind::resize, 0, 4, sequence), message.storage_, message.size_);
  const auto encoded = encode_dimensions(dimensions);
  std::ranges::copy(encoded, std::span(message.storage_).subspan(message.size_).begin());
  message.size_ += encoded.size();
  return message;
}

[[nodiscard]] auto encode_detach(const std::uint32_t sequence) noexcept -> SmallMessage {
  SmallMessage message;
  copy_header(encode_header(MessageKind::detach, 0, 0, sequence), message.storage_, message.size_);
  return message;
}

[[nodiscard]] auto encode_pane_command(const PaneCommand command,
                                       const std::uint32_t sequence) noexcept -> SmallMessage {
  FRAME_ASSERT(command != PaneCommand::none);
  SmallMessage message;
  copy_header(encode_header(MessageKind::pane_command, 0, 1, sequence), message.storage_,
              message.size_);
  std::span(message.storage_).subspan(message.size_, 1).front() = static_cast<std::byte>(command);
  ++message.size_;
  return message;
}

[[nodiscard]] auto encode_host_theme_update(const HostTerminalTheme& theme,
                                            const std::uint32_t sequence) noexcept -> SmallMessage {
  FRAME_ASSERT(!theme.empty());
  SmallMessage message;
  copy_header(encode_header(MessageKind::host_theme, 0, host_theme_wire_bytes, sequence),
              message.storage_, message.size_);
  encode_host_theme(
      theme, std::span(message.storage_).subspan<attach_header_bytes, host_theme_wire_bytes>());
  message.size_ += host_theme_wire_bytes;
  return message;
}

[[nodiscard]] auto encode_render_frame_header(const std::size_t ansi_bytes,
                                              const std::uint32_t sequence,
                                              const std::uint32_t full_redraw_generation,
                                              const bool full_redraw) noexcept
    -> std::array<std::byte, attach_header_bytes + render_generation_bytes> {
  FRAME_ASSERT(ansi_bytes > 0);
  FRAME_ASSERT(ansi_bytes <= render_ansi_bytes_max);
  FRAME_ASSERT(full_redraw_generation > 0);
  std::array<std::byte, attach_header_bytes + render_generation_bytes> encoded{};
  const auto header =
      encode_header(MessageKind::render_frame, full_redraw ? render_full_redraw_flag : 0,
                    static_cast<std::uint32_t>(render_generation_bytes + ansi_bytes), sequence);
  std::ranges::copy(header, encoded.begin());
  encode_u32(full_redraw_generation,
             std::span(encoded).subspan<attach_header_bytes, render_generation_bytes>());
  return encoded;
}

[[nodiscard]] auto encode_disconnect(const DisconnectReason reason,
                                     const std::string_view diagnostic,
                                     const std::uint32_t sequence) noexcept -> SmallMessage {
  FRAME_ASSERT(disconnect_reason(static_cast<std::byte>(reason)).has_value());
  FRAME_ASSERT(diagnostic.size() <= diagnostic_bytes_max);
  SmallMessage message;
  const auto payload_bytes = static_cast<std::uint32_t>(1U + diagnostic.size());
  copy_header(encode_header(MessageKind::disconnect, 0, payload_bytes, sequence), message.storage_,
              message.size_);
  auto output = std::span(message.storage_).subspan(message.size_);
  output.front() = static_cast<std::byte>(reason);
  std::ranges::copy(std::as_bytes(std::span(diagnostic.data(), diagnostic.size())),
                    output.subspan<1>().begin());
  message.size_ += payload_bytes;
  return message;
}

[[nodiscard]] auto decode_error_diagnostic(const DecodeError error) noexcept -> std::string_view {
  switch (error) {
  case DecodeError::invalid_magic:
    return "invalid attach protocol magic";
  case DecodeError::version_mismatch:
    return "attach protocol version mismatch";
  case DecodeError::invalid_kind:
    return "invalid attach message kind";
  case DecodeError::invalid_flags:
    return "invalid attach message flags";
  case DecodeError::invalid_length:
    return "invalid attach message length";
  case DecodeError::oversized:
    return "attach message exceeds its bounded size";
  case DecodeError::invalid_sequence:
    return "invalid attach message sequence";
  case DecodeError::invalid_enum:
    return "invalid attach message enum";
  case DecodeError::invalid_dimensions:
    return "invalid attach terminal dimensions";
  case DecodeError::invalid_session:
    return "invalid attach session name";
  case DecodeError::invalid_generation:
    return "invalid full-redraw generation";
  case DecodeError::buffer_full:
    return "attach decoder buffer exhausted";
  case DecodeError::allocation_failed:
    return "attach decoder allocation failed";
  }
  return "invalid attach protocol message";
}

auto ClientDecoder::prepare() noexcept -> std::expected<void, DecodeError> {
  prepared_ = true;
  return {};
}

void ClientDecoder::release() noexcept {
  expanded_storage_.reset();
  reset();
  prepared_ = false;
}

[[nodiscard]] auto ClientDecoder::mutable_storage() noexcept -> std::span<std::byte> {
  return expanded_storage_ == nullptr
             ? std::span<std::byte>(inline_storage_)
             : std::span<std::byte>(expanded_storage_.get(), client_decoder_bytes_max);
}

[[nodiscard]] auto ClientDecoder::storage() const noexcept -> std::span<const std::byte> {
  return expanded_storage_ == nullptr
             ? std::span<const std::byte>(inline_storage_)
             : std::span<const std::byte>(expanded_storage_.get(), client_decoder_bytes_max);
}

[[nodiscard]] auto ClientDecoder::writable_bytes() noexcept -> std::span<std::byte> {
  FRAME_ASSERT(prepared_);
  FRAME_ASSERT(pending_size_ == 0);
  return mutable_storage().subspan(used_);
}

[[nodiscard]] auto ClientDecoder::commit(const std::size_t bytes) noexcept
    -> std::expected<void, DecodeError> {
  if (!prepared_ || pending_size_ != 0 || bytes > mutable_storage().size() - used_) {
    return std::unexpected(DecodeError::buffer_full);
  }
  used_ += bytes;
  return {};
}

// Header and payload validation is complete before a borrowed message is exposed to the core.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto ClientDecoder::next() noexcept
    -> std::expected<std::optional<ClientMessage>, DecodeError> {
  if (used_ == 0) {
    return std::optional<ClientMessage>{};
  }
  FRAME_ASSERT(prepared_);
  const auto buffered = mutable_storage().first(used_);
  const auto decoded = decode_envelope(buffered, expected_sequence_);
  if (!decoded.has_value()) {
    return std::unexpected(decoded.error());
  }
  if (!decoded->has_value()) {
    return std::optional<ClientMessage>{};
  }
  const auto envelope = **decoded;
  if (envelope.flags != 0) {
    return std::unexpected(DecodeError::invalid_flags);
  }

  if (expect_hello_) {
    if (envelope.kind != MessageKind::hello) {
      return std::unexpected(DecodeError::invalid_kind);
    }
    if (envelope.payload_bytes < 6U || envelope.payload_bytes > client_hello_payload_bytes_max) {
      return std::unexpected(envelope.payload_bytes > client_hello_payload_bytes_max
                                 ? DecodeError::oversized
                                 : DecodeError::invalid_length);
    }
  } else {
    switch (envelope.kind) {
    case MessageKind::input:
      if (envelope.payload_bytes == 0) {
        return std::unexpected(DecodeError::invalid_length);
      }
      if (envelope.payload_bytes > legacy_input_message_bytes_max) {
        return std::unexpected(DecodeError::oversized);
      }
      break;
    case MessageKind::terminal_reply:
      if (envelope.payload_bytes == 0 || envelope.payload_bytes > terminal_reply_bytes_max) {
        return std::unexpected(DecodeError::invalid_length);
      }
      break;
    case MessageKind::paste:
      if (envelope.payload_bytes == 0) {
        return std::unexpected(DecodeError::invalid_length);
      }
      if (envelope.payload_bytes > input_message_bytes_max) {
        return std::unexpected(DecodeError::oversized);
      }
      break;
    case MessageKind::key:
      if (envelope.payload_bytes < key_input_wire_fixed_bytes ||
          envelope.payload_bytes > key_input_wire_fixed_bytes + key_input_text_bytes_max) {
        return std::unexpected(envelope.payload_bytes >
                                       key_input_wire_fixed_bytes + key_input_text_bytes_max
                                   ? DecodeError::oversized
                                   : DecodeError::invalid_length);
      }
      break;
    case MessageKind::focus:
    case MessageKind::clipboard_support:
      if (envelope.payload_bytes != 1) {
        return std::unexpected(DecodeError::invalid_length);
      }
      break;
    case MessageKind::mouse:
      if (envelope.payload_bytes != mouse_input_wire_bytes) {
        return std::unexpected(DecodeError::invalid_length);
      }
      break;
    case MessageKind::resize:
    case MessageKind::cell_size:
      if (envelope.payload_bytes != 4) {
        return std::unexpected(DecodeError::invalid_length);
      }
      break;
    case MessageKind::pane_command:
      if (envelope.payload_bytes != 1) {
        return std::unexpected(DecodeError::invalid_length);
      }
      break;
    case MessageKind::detach:
      if (envelope.payload_bytes != 0) {
        return std::unexpected(DecodeError::invalid_length);
      }
      break;
    case MessageKind::host_theme:
      if (envelope.payload_bytes != host_theme_wire_bytes) {
        return std::unexpected(DecodeError::invalid_length);
      }
      break;
    case MessageKind::hello:
    case MessageKind::render_frame:
    case MessageKind::disconnect:
      return std::unexpected(DecodeError::invalid_kind);
    }
  }

  const auto packet_size = attach_header_bytes + envelope.payload_bytes;
  if (packet_size > client_decoder_bytes_max) {
    return std::unexpected(DecodeError::oversized);
  }
  if (packet_size > mutable_storage().size()) {
    FRAME_ASSERT(expanded_storage_ == nullptr);
    try {
      // Allocated only after a valid live paste envelope exceeds the ordinary input bound.
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
      auto expanded = std::make_unique_for_overwrite<std::byte[]>(client_decoder_bytes_max);
      std::ranges::copy(std::span(inline_storage_).first(used_), expanded.get());
      expanded_storage_ = std::move(expanded);
    } catch (const std::bad_alloc&) {
      return std::unexpected(DecodeError::allocation_failed);
    }
    return next();
  }
  if (buffered.size() < packet_size) {
    return std::optional<ClientMessage>{};
  }
  const auto payload = buffered.subspan(attach_header_bytes, envelope.payload_bytes);
  pending_size_ = packet_size;

  if (envelope.kind == MessageKind::hello) {
    const auto name_size = std::to_integer<std::size_t>(payload.front());
    const auto hello_flags = std::to_integer<std::uint8_t>(payload.subspan<5, 1>().front());
    if (name_size > session_name_bytes_max || (hello_flags & ~hello_host_theme_flag) != 0) {
      return std::unexpected(DecodeError::invalid_length);
    }
    const bool has_host_theme = (hello_flags & hello_host_theme_flag) != 0;
    const auto theme_bytes = has_host_theme ? host_theme_wire_bytes : 0U;
    if (payload.size() != 6U + theme_bytes + name_size) {
      return std::unexpected(DecodeError::invalid_length);
    }
    const auto dimensions = decode_dimensions(std::span(payload).subspan<1, 4>());
    if (!valid_dimensions(dimensions)) {
      return std::unexpected(DecodeError::invalid_dimensions);
    }
    const HostTerminalTheme* host_theme = nullptr;
    if (has_host_theme) {
      const auto decoded_theme =
          decode_host_theme(std::span(payload).subspan<6, host_theme_wire_bytes>());
      if (!decoded_theme.has_value()) {
        return std::unexpected(decoded_theme.error());
      }
      host_theme_ = *decoded_theme;
      host_theme = &host_theme_;
    }
    // Wire bytes are borrowed as a character view only for synchronous validation/dispatch.
    const std::string_view session(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const char*>(payload.subspan(6U + theme_bytes).data()), name_size);
    if (!session.empty() && !valid_session_name(session)) {
      return std::unexpected(DecodeError::invalid_session);
    }
    return ClientMessage{
        .kind = ClientMessageKind::hello,
        .dimensions = dimensions,
        .pane_command = PaneCommand::none,
        .host_theme = host_theme,
        .session = session,
        .input = {},
        .sequence = envelope.sequence,
    };
  }
  if (envelope.kind == MessageKind::host_theme) {
    const auto host_theme = decode_host_theme(std::span(payload).first<host_theme_wire_bytes>());
    if (!host_theme.has_value()) {
      return std::unexpected(host_theme.error());
    }
    host_theme_ = *host_theme;
    return ClientMessage{
        .kind = ClientMessageKind::host_theme,
        .dimensions = {},
        .pane_command = PaneCommand::none,
        .host_theme = &host_theme_,
        .session = {},
        .input = {},
        .sequence = envelope.sequence,
    };
  }
  if (envelope.kind == MessageKind::input || envelope.kind == MessageKind::paste ||
      envelope.kind == MessageKind::terminal_reply) {
    auto kind =
        envelope.kind == MessageKind::input ? ClientMessageKind::input : ClientMessageKind::paste;
    if (envelope.kind == MessageKind::terminal_reply) {
      kind = ClientMessageKind::terminal_reply;
    }
    return ClientMessage{
        .kind = kind,
        .dimensions = {},
        .pane_command = PaneCommand::none,
        .host_theme = nullptr,
        .session = {},
        .input = payload,
        .sequence = envelope.sequence,
    };
  }
  if (envelope.kind == MessageKind::key) {
    const auto action_value = std::to_integer<std::uint8_t>(payload[0]);
    const auto key_value = std::to_integer<std::uint8_t>(payload[1]);
    const auto modifiers = decode_u16(payload[2], payload[3]);
    const auto consumed_modifiers = decode_u16(payload[4], payload[5]);
    constexpr std::uint16_t valid_modifiers =
        key_input_modifier_shift | key_input_modifier_control | key_input_modifier_alt |
        key_input_modifier_super | key_input_modifier_caps_lock | key_input_modifier_num_lock;
    if (action_value > static_cast<std::uint8_t>(KeyInputAction::repeat) ||
        key_value > static_cast<std::uint8_t>(KeyInputKey::f12) ||
        (modifiers & ~valid_modifiers) != 0 || (consumed_modifiers & ~valid_modifiers) != 0 ||
        payload[10] > std::byte{1}) {
      return std::unexpected(DecodeError::invalid_enum);
    }
    return ClientMessage{
        .kind = ClientMessageKind::key,
        .key = {.action = static_cast<KeyInputAction>(action_value),
                .key = static_cast<KeyInputKey>(key_value),
                .modifiers = modifiers,
                .consumed_modifiers = consumed_modifiers,
                .unshifted_codepoint = decode_u32(std::span(payload).subspan<6, 4>()),
                .composing = payload[10] == std::byte{1}},
        .session = {},
        .input = payload.subspan(key_input_wire_fixed_bytes),
        .sequence = envelope.sequence,
    };
  }
  if (envelope.kind == MessageKind::focus) {
    const auto focus = focus_input(payload.front());
    if (!focus.has_value()) {
      return std::unexpected(DecodeError::invalid_enum);
    }
    return ClientMessage{
        .kind = ClientMessageKind::focus,
        .focus = *focus,
        .session = {},
        .input = {},
        .sequence = envelope.sequence,
    };
  }
  if (envelope.kind == MessageKind::mouse) {
    const auto action = mouse_action(payload[0]);
    const auto button = mouse_button(payload[1]);
    const auto modifiers = decode_u16(payload[2], payload[3]);
    const MouseInput mouse{
        .action = action.value_or(MouseInputAction::motion),
        .button = button.value_or(MouseInputButton::none),
        .modifiers = modifiers,
        .column = decode_u16(payload[4], payload[5]),
        .row = decode_u16(payload[6], payload[7]),
        .geometry = {.columns = decode_u16(payload[8], payload[9]),
                     .rows = decode_u16(payload[10], payload[11])},
        .any_button_pressed = payload[12] == std::byte{1},
    };
    constexpr std::uint16_t valid_modifiers = 0x03FFU;
    if (!action.has_value() || !button.has_value() || (modifiers & ~valid_modifiers) != 0 ||
        (payload[12] != std::byte{0} && payload[12] != std::byte{1}) ||
        !valid_dimensions(mouse.geometry) || mouse.column >= mouse.geometry.columns ||
        mouse.row >= mouse.geometry.rows) {
      return std::unexpected(!valid_dimensions(mouse.geometry) ? DecodeError::invalid_dimensions
                                                               : DecodeError::invalid_enum);
    }
    return ClientMessage{
        .kind = ClientMessageKind::mouse,
        .mouse = mouse,
        .session = {},
        .input = {},
        .sequence = envelope.sequence,
    };
  }
  if (envelope.kind == MessageKind::clipboard_support) {
    const auto value = std::to_integer<unsigned>(payload.front());
    if (value > 1) {
      return std::unexpected(DecodeError::invalid_enum);
    }
    return ClientMessage{.kind = ClientMessageKind::clipboard_support,
                         .clipboard_supported = value != 0,
                         .session = {},
                         .input = {},
                         .sequence = envelope.sequence};
  }
  if (envelope.kind == MessageKind::cell_size) {
    const auto dimensions = decode_dimensions(std::span(payload).first<4>());
    if (!valid_dimensions(dimensions)) {
      return std::unexpected(DecodeError::invalid_dimensions);
    }
    return ClientMessage{.kind = ClientMessageKind::cell_size,
                         .cell_size = {.width = dimensions.columns, .height = dimensions.rows},
                         .session = {},
                         .input = {},
                         .sequence = envelope.sequence};
  }
  if (envelope.kind == MessageKind::resize) {
    const auto dimensions = decode_dimensions(std::span(payload).first<4>());
    if (!valid_dimensions(dimensions)) {
      return std::unexpected(DecodeError::invalid_dimensions);
    }
    return ClientMessage{
        .kind = ClientMessageKind::resize,
        .dimensions = dimensions,
        .pane_command = PaneCommand::none,
        .host_theme = nullptr,
        .session = {},
        .input = {},
        .sequence = envelope.sequence,
    };
  }
  if (envelope.kind == MessageKind::pane_command) {
    const auto command = pane_command(payload.front());
    if (!command.has_value()) {
      return std::unexpected(DecodeError::invalid_enum);
    }
    return ClientMessage{
        .kind = ClientMessageKind::pane_command,
        .dimensions = {},
        .pane_command = *command,
        .host_theme = nullptr,
        .session = {},
        .input = {},
        .sequence = envelope.sequence,
    };
  }
  FRAME_ASSERT(envelope.kind == MessageKind::detach);
  return ClientMessage{
      .kind = ClientMessageKind::detach,
      .dimensions = {},
      .pane_command = PaneCommand::none,
      .host_theme = nullptr,
      .session = {},
      .input = {},
      .sequence = envelope.sequence,
  };
}

void ClientDecoder::consume() noexcept {
  FRAME_ASSERT(prepared_);
  FRAME_ASSERT(pending_size_ > 0 && pending_size_ <= used_);
  auto active_storage = mutable_storage();
  std::memmove(active_storage.data(), active_storage.subspan(pending_size_).data(),
               used_ - pending_size_);
  used_ -= pending_size_;
  if (expanded_storage_ != nullptr && used_ <= inline_storage_.size()) {
    std::ranges::copy(std::span(expanded_storage_.get(), used_).first(used_),
                      inline_storage_.begin());
    expanded_storage_.reset();
  }
  pending_size_ = 0;
  expected_sequence_ = next_sequence(expected_sequence_);
  if (expect_hello_) {
    expect_hello_ = false;
  }
}

void ClientDecoder::reset(const std::uint32_t expected_sequence, const bool expect_hello) noexcept {
  FRAME_ASSERT(expected_sequence != 0);
  used_ = 0;
  pending_size_ = 0;
  expected_sequence_ = expected_sequence;
  expect_hello_ = expect_hello;
}

[[nodiscard]] auto ServerDecoder::prepare() noexcept -> std::expected<void, DecodeError> {
  if (storage_ != nullptr) {
    return {};
  }
  try {
    // One connection-lifetime owner; pages remain lazy until received frame bytes touch them.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    storage_ = std::make_unique_for_overwrite<std::byte[]>(server_decoder_bytes_max);
  } catch (const std::bad_alloc&) {
    return std::unexpected(DecodeError::allocation_failed);
  }
  return {};
}

[[nodiscard]] auto ServerDecoder::writable_bytes() noexcept -> std::span<std::byte> {
  FRAME_ASSERT(pending_size_ == 0);
  return storage_ == nullptr
             ? std::span<std::byte>{}
             : std::span<std::byte>(storage_.get(), server_decoder_bytes_max).subspan(used_);
}

[[nodiscard]] auto ServerDecoder::commit(const std::size_t bytes) noexcept
    -> std::expected<void, DecodeError> {
  if (storage_ == nullptr || pending_size_ != 0 || bytes > server_decoder_bytes_max - used_) {
    return std::unexpected(DecodeError::buffer_full);
  }
  used_ += bytes;
  return {};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto ServerDecoder::next() noexcept
    -> std::expected<std::optional<ServerMessage>, DecodeError> {
  if (used_ == 0) {
    return std::optional<ServerMessage>{};
  }
  FRAME_ASSERT(storage_ != nullptr);
  const auto buffered = std::span<const std::byte>(storage_.get(), used_);
  const auto decoded = decode_envelope(buffered, expected_sequence_);
  if (!decoded.has_value()) {
    return std::unexpected(decoded.error());
  }
  if (!decoded->has_value()) {
    return std::optional<ServerMessage>{};
  }
  const auto envelope = **decoded;

  if (expect_hello_) {
    if (envelope.kind != MessageKind::hello && envelope.kind != MessageKind::disconnect) {
      return std::unexpected(DecodeError::invalid_kind);
    }
  } else if (envelope.kind != MessageKind::render_frame &&
             envelope.kind != MessageKind::disconnect) {
    return std::unexpected(DecodeError::invalid_kind);
  }
  if (envelope.kind == MessageKind::render_frame) {
    if ((envelope.flags & ~render_full_redraw_flag) != 0) {
      return std::unexpected(DecodeError::invalid_flags);
    }
    if (envelope.payload_bytes <= render_generation_bytes) {
      return std::unexpected(DecodeError::invalid_length);
    }
    if (envelope.payload_bytes > render_payload_bytes_max) {
      return std::unexpected(DecodeError::oversized);
    }
  } else {
    if (envelope.flags != 0) {
      return std::unexpected(DecodeError::invalid_flags);
    }
    if (envelope.kind == MessageKind::hello && envelope.payload_bytes != 4) {
      return std::unexpected(DecodeError::invalid_length);
    }
    if (envelope.kind == MessageKind::disconnect &&
        (envelope.payload_bytes == 0 || envelope.payload_bytes > 1U + diagnostic_bytes_max)) {
      return std::unexpected(envelope.payload_bytes > 1U + diagnostic_bytes_max
                                 ? DecodeError::oversized
                                 : DecodeError::invalid_length);
    }
  }

  const auto packet_size = attach_header_bytes + envelope.payload_bytes;
  if (packet_size > server_decoder_bytes_max) {
    return std::unexpected(DecodeError::oversized);
  }
  if (buffered.size() < packet_size) {
    return std::optional<ServerMessage>{};
  }
  const auto payload = buffered.subspan(attach_header_bytes, envelope.payload_bytes);
  pending_size_ = packet_size;
  pending_generation_ = 0;
  pending_full_redraw_ = false;

  if (envelope.kind == MessageKind::hello) {
    const auto dimensions = decode_dimensions(std::span(payload).first<4>());
    if (!valid_dimensions(dimensions)) {
      return std::unexpected(DecodeError::invalid_dimensions);
    }
    return ServerMessage{
        .kind = ServerMessageKind::hello,
        .dimensions = dimensions,
        .reason = DisconnectReason::internal_error,
        .diagnostic = {},
        .ansi = {},
        .sequence = envelope.sequence,
        .full_redraw_generation = 0,
        .full_redraw = false,
    };
  }
  if (envelope.kind == MessageKind::disconnect) {
    const auto reason = disconnect_reason(payload.front());
    if (!reason.has_value()) {
      return std::unexpected(DecodeError::invalid_enum);
    }
    const auto diagnostic_bytes = payload.subspan(1);
    if (!valid_diagnostic(diagnostic_bytes)) {
      return std::unexpected(DecodeError::invalid_enum);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view diagnostic(reinterpret_cast<const char*>(diagnostic_bytes.data()),
                                      diagnostic_bytes.size());
    return ServerMessage{
        .kind = ServerMessageKind::disconnect,
        .dimensions = {},
        .reason = *reason,
        .diagnostic = diagnostic,
        .ansi = {},
        .sequence = envelope.sequence,
        .full_redraw_generation = 0,
        .full_redraw = false,
    };
  }

  const auto generation = decode_u32(std::span(payload).first<render_generation_bytes>());
  const bool full_redraw = (envelope.flags & render_full_redraw_flag) != 0;
  if (generation == 0 || (full_redraw && generation != next_sequence(full_redraw_generation_)) ||
      (!full_redraw && (full_redraw_generation_ == 0 || generation != full_redraw_generation_))) {
    return std::unexpected(DecodeError::invalid_generation);
  }
  pending_generation_ = generation;
  pending_full_redraw_ = full_redraw;
  return ServerMessage{
      .kind = ServerMessageKind::render_frame,
      .dimensions = {},
      .reason = DisconnectReason::internal_error,
      .diagnostic = {},
      .ansi = payload.subspan(render_generation_bytes),
      .sequence = envelope.sequence,
      .full_redraw_generation = generation,
      .full_redraw = full_redraw,
  };
}

void ServerDecoder::consume() noexcept {
  FRAME_ASSERT(storage_ != nullptr);
  FRAME_ASSERT(pending_size_ > 0 && pending_size_ <= used_);
  std::memmove(storage_.get(), std::span(storage_.get(), used_).subspan(pending_size_).data(),
               used_ - pending_size_);
  used_ -= pending_size_;
  pending_size_ = 0;
  expected_sequence_ = next_sequence(expected_sequence_);
  if (expect_hello_) {
    expect_hello_ = false;
  }
  if (pending_full_redraw_) {
    full_redraw_generation_ = pending_generation_;
  }
  pending_generation_ = 0;
  pending_full_redraw_ = false;
}

void ServerDecoder::reset(const std::uint32_t expected_sequence, const bool expect_hello) noexcept {
  FRAME_ASSERT(expected_sequence != 0);
  used_ = 0;
  pending_size_ = 0;
  expected_sequence_ = expected_sequence;
  full_redraw_generation_ = 0;
  pending_generation_ = 0;
  pending_full_redraw_ = false;
  expect_hello_ = expect_hello;
}

} // namespace frame::protocol

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
