#ifndef FRAME_PROTOCOL_ATTACHMENT_HPP
#define FRAME_PROTOCOL_ATTACHMENT_HPP

#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace frame::protocol {

inline constexpr std::size_t input_bytes_max = std::size_t{4} * 1'024U;
inline constexpr std::size_t legacy_input_message_bytes_max = input_bytes_max * 2U;
inline constexpr std::size_t input_message_bytes_max = limits::structured_input_payload_bytes_max;
inline constexpr std::uint16_t columns_max = 500;
inline constexpr std::uint16_t rows_max = 200;
inline constexpr std::size_t session_name_bytes_max = limits::session_name_bytes_max;
inline constexpr std::size_t tab_title_bytes_max = limits::tab_title_bytes_max;
inline constexpr std::size_t tab_slots_max =
    static_cast<std::size_t>(limits::tabs_hard_max / limits::sessions_hard_max);
inline constexpr std::size_t working_directory_bytes_max = limits::working_directory_bytes_max;
// create_with_context uses this sentinel to reuse an existing session without creating one when
// the caller cannot capture a fresh launch context.
inline constexpr std::size_t unavailable_working_directory_size = 0;
inline constexpr std::size_t environment_bytes_max = limits::environment_bytes_max;
inline constexpr std::size_t environment_entries_max = limits::environment_entries_max;
inline constexpr std::size_t command_bytes_max = limits::command_bytes_hard_max;
inline constexpr std::size_t command_arguments_max = limits::command_arguments_hard_max;
inline constexpr std::string_view shutdown_response = "frame daemon stopped\n";

struct Dimensions final {
  std::uint16_t columns{80};
  std::uint16_t rows{24};

  [[nodiscard]] constexpr auto operator==(const Dimensions&) const noexcept -> bool = default;
};

struct CellSize final {
  std::uint16_t width{8};
  std::uint16_t height{16};
  constexpr auto operator==(const CellSize&) const noexcept -> bool = default;
};

struct RgbColor final {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};

  [[nodiscard]] constexpr auto operator==(const RgbColor&) const noexcept -> bool = default;
};

// Bounded client observation of the outer terminal. Missing values remain unspecified rather than
// being confused with black; palette presence is represented independently from RGB storage.
struct HostTerminalTheme final {
  [[nodiscard]] constexpr auto has_palette_color(const std::size_t index) const noexcept -> bool {
    return index < palette.size() &&
           (palette_mask & (std::uint16_t{1} << static_cast<unsigned>(index))) != 0;
  }
  constexpr void set_palette_color(const std::size_t index, const RgbColor color) noexcept {
    if (index >= palette.size()) {
      return;
    }
    std::span(palette).subspan(index, 1).front() = color;
    palette_mask |= std::uint16_t{1} << static_cast<unsigned>(index);
  }
  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
    return !foreground.has_value() && !background.has_value() &&
           !selection_foreground.has_value() && !selection_background.has_value() &&
           palette_mask == 0;
  }

  std::optional<RgbColor> foreground;
  std::optional<RgbColor> background;
  std::optional<RgbColor> selection_foreground;
  std::optional<RgbColor> selection_background;
  std::array<RgbColor, 16> palette{};
  std::uint16_t palette_mask{0};

  [[nodiscard]] constexpr auto operator==(const HostTerminalTheme&) const noexcept
      -> bool = default;
};

enum class ControlCommand : std::uint8_t {
  create = 'N',
  create_with_context = 'C',
  create_auto_with_context = 'A',
  list = 'L',
  list_session = 'Q',
  list_tabs = 'W',
  list_panes = 'P',
  query_sessions = 'l',
  query_session = 'q',
  query_tabs = 'w',
  query_panes = 'p',
  rename_session = 'R',
  rename_tab = 'T',
  kill = 'K',
  surface_create = 'F',
  semantic_action = 'O',
  send_pane = 'E',
  capture_pane = 'G',
  pane_status = 'U',
  kill_all = 'X',
  shutdown = 'S',
};

enum class SurfaceCreateKind : std::uint8_t {
  tab = 1,
  split_right = 2,
  split_down = 3,
};

enum class ControlAction : std::uint8_t {
  tab_select = 1,
  tab_move = 2,
  tab_kill = 3,
  pane_focus = 4,
  pane_swap = 5,
  pane_resize_left = 6,
  pane_resize_right = 7,
  pane_resize_up = 8,
  pane_resize_down = 9,
  pane_zoom_on = 10,
  pane_zoom_off = 11,
  pane_kill = 12,
  session_kill = 13,
};

inline constexpr std::size_t control_id_bytes = 8;
inline constexpr std::size_t semantic_action_payload_bytes = 1U + (3U * control_id_bytes) + 4U;
inline constexpr std::size_t control_payload_bytes_max = environment_bytes_max;

template <typename Id>
[[nodiscard]] constexpr auto encode_control_id(const Id id) noexcept
    -> std::array<std::byte, control_id_bytes> {
  const auto slot = id.is_valid() ? id.slot() : std::numeric_limits<std::uint32_t>::max();
  const auto generation = id.is_valid() ? id.generation() : 0U;
  return {
      static_cast<std::byte>((slot >> 24U) & 0xffU),
      static_cast<std::byte>((slot >> 16U) & 0xffU),
      static_cast<std::byte>((slot >> 8U) & 0xffU),
      static_cast<std::byte>(slot & 0xffU),
      static_cast<std::byte>((generation >> 24U) & 0xffU),
      static_cast<std::byte>((generation >> 16U) & 0xffU),
      static_cast<std::byte>((generation >> 8U) & 0xffU),
      static_cast<std::byte>(generation & 0xffU),
  };
}

template <typename Id>
[[nodiscard]] constexpr auto
decode_control_id(const std::span<const std::byte, control_id_bytes> bytes) noexcept
    -> std::optional<Id> {
  const auto decode = [](const std::span<const std::byte, 4> value) {
    return (std::to_integer<std::uint32_t>(value.subspan(0, 1).front()) << 24U) |
           (std::to_integer<std::uint32_t>(value.subspan(1, 1).front()) << 16U) |
           (std::to_integer<std::uint32_t>(value.subspan(2, 1).front()) << 8U) |
           std::to_integer<std::uint32_t>(value.subspan(3, 1).front());
  };
  const auto slot = decode(bytes.template first<4>());
  const auto generation = decode(bytes.template subspan<4, 4>());
  if (slot == std::numeric_limits<std::uint32_t>::max() && generation == 0) {
    return Id{};
  }
  return Id::try_from_parts(slot, generation);
}

enum class ControlResponse : std::uint8_t {
  ready = 'Y',
  no_effect = 'I',
  missing = 'M',
  capacity = 'C',
  conflict = 'D',
  unavailable = 'U',
  failed = 'F',
};

enum class PaneCommand : std::uint8_t {
  none = 0,
  split_left_right = '%',
  split_top_bottom = '"',
  resize_left = 0x80,
  resize_right = 0x81,
  resize_up = 0x82,
  resize_down = 0x83,
  focus_left = 'L',
  focus_right = 'R',
  focus_up = 'U',
  focus_down = 'D',
  focus_next = 'o',
  focus_previous = ';',
  close = 'x',
  zoom = 'z',
  enter_copy_mode = '[',
  enter_copy_search_forward = '/',
  enter_copy_search_backward = '?',
  create_tab = 'c',
  next_tab = 'n',
  previous_tab = 'p',
  begin_rename_session = 0x88,
  begin_rename_tab = 0x89,
  move_tab_left = 'P',
  move_tab_right = 'N',
  swap_pane_left = 0x84,
  swap_pane_right = 0x85,
  swap_pane_up = 0x86,
  swap_pane_down = 0x87,
  kill_tab = '&',
  select_tab_0 = '0',
  select_tab_1 = '1',
  select_tab_2 = '2',
  select_tab_3 = '3',
  select_tab_4 = '4',
  select_tab_5 = '5',
  select_tab_6 = '6',
  select_tab_7 = '7',
  select_tab_8 = '8',
  select_tab_9 = '9',
};

// Private attach protocol v2.11. Every envelope is exactly 16 bytes:
// magic[4], major, minor, kind, flags, payload_length:u32be, sequence:u32be.
struct ProtocolVersion final {
  std::uint8_t major{2};
  std::uint8_t minor{11};

  [[nodiscard]] constexpr auto operator==(const ProtocolVersion&) const noexcept -> bool = default;
};

inline constexpr ProtocolVersion current_version{};
inline constexpr std::array<std::byte, 4> attach_magic{std::byte{0x89}, std::byte{'L'},
                                                       std::byte{'M'}, std::byte{'A'}};
inline constexpr std::size_t attach_header_bytes = 16;
inline constexpr std::size_t render_generation_bytes = 4;
inline constexpr std::size_t render_ansi_bytes_max = limits::frame_chunk_bytes_max;
inline constexpr std::size_t render_payload_bytes_max =
    render_generation_bytes + render_ansi_bytes_max;
inline constexpr std::size_t diagnostic_bytes_max = 255;
inline constexpr std::size_t host_theme_palette_colors = 16;
inline constexpr std::size_t host_theme_palette_mask_bytes = 2;
inline constexpr std::size_t host_theme_wire_bytes =
    1U + host_theme_palette_mask_bytes + 12U + (host_theme_palette_colors * 3U);
inline constexpr std::size_t client_hello_payload_bytes_max =
    6U + host_theme_wire_bytes + session_name_bytes_max;
inline constexpr std::size_t small_message_bytes_max =
    attach_header_bytes +
    std::max({1U + diagnostic_bytes_max, client_hello_payload_bytes_max, std::size_t{267}});
inline constexpr std::size_t client_decoder_bytes_max =
    attach_header_bytes + input_message_bytes_max;
inline constexpr std::size_t server_decoder_bytes_max =
    attach_header_bytes + render_payload_bytes_max;
inline constexpr std::uint8_t render_full_redraw_flag = 0x01;

static_assert(small_message_bytes_max >= attach_header_bytes + client_hello_payload_bytes_max);
static_assert(client_decoder_bytes_max <= (std::size_t{1} * 1'024U * 1'024U) + 32U);
static_assert(client_decoder_bytes_max * limits::sessions_hard_max <
              std::size_t{65} * 1'024U * 1'024U);
static_assert(server_decoder_bytes_max <= (std::size_t{4} * 1'024U * 1'024U) + 32U);

enum class MessageKind : std::uint8_t {
  hello = 1,
  input = 2,
  resize = 3,
  pane_command = 4,
  detach = 5,
  render_frame = 6,
  disconnect = 7,
  host_theme = 8,
  paste = 9,
  focus = 10,
  mouse = 11,
  key = 12,
  terminal_reply = 13,
  cell_size = 14,
  clipboard_support = 15,
};

enum class DisconnectReason : std::uint8_t {
  normal = 1,
  protocol_error = 2,
  version_mismatch = 3,
  session_busy = 4,
  session_missing = 5,
  capacity = 6,
  setup_failed = 7,
  frame_timeout = 8,
  daemon_shutdown = 9,
  internal_error = 10,
};

enum class ClientMessageKind : std::uint8_t {
  hello,
  input,
  key,
  paste,
  focus,
  mouse,
  resize,
  detach,
  pane_command,
  host_theme,
  terminal_reply,
  cell_size,
  clipboard_support,
};

enum class KeyInputAction : std::uint8_t {
  release = 0,
  press = 1,
  repeat = 2,
};

enum class KeyInputKey : std::uint8_t {
  unidentified,
  a,
  b,
  c,
  d,
  e,
  f,
  g,
  h,
  i,
  j,
  k,
  l,
  m,
  n,
  o,
  p,
  q,
  r,
  s,
  t,
  u,
  v,
  w,
  x,
  y,
  z,
  enter,
  tab,
  backspace,
  escape,
  space,
  arrow_up,
  arrow_down,
  arrow_left,
  arrow_right,
  home,
  end,
  insert,
  delete_key,
  page_up,
  page_down,
  f1,
  f2,
  f3,
  f4,
  f5,
  f6,
  f7,
  f8,
  f9,
  f10,
  f11,
  f12,
};

struct KeyInput final {
  KeyInputAction action{KeyInputAction::press};
  KeyInputKey key{KeyInputKey::unidentified};
  std::uint16_t modifiers{0};
  std::uint16_t consumed_modifiers{0};
  std::uint32_t unshifted_codepoint{0};
  bool composing{false};

  [[nodiscard]] constexpr auto operator==(const KeyInput&) const noexcept -> bool = default;
};

inline constexpr std::size_t terminal_reply_bytes_max = 8192;
inline constexpr std::size_t key_input_wire_fixed_bytes = 11;
inline constexpr std::size_t key_input_text_bytes_max = 256;

inline constexpr std::uint16_t key_input_modifier_shift = 1U << 0U;
inline constexpr std::uint16_t key_input_modifier_control = 1U << 1U;
inline constexpr std::uint16_t key_input_modifier_alt = 1U << 2U;
inline constexpr std::uint16_t key_input_modifier_super = 1U << 3U;
inline constexpr std::uint16_t key_input_modifier_caps_lock = 1U << 4U;
inline constexpr std::uint16_t key_input_modifier_num_lock = 1U << 5U;

enum class FocusInput : std::uint8_t {
  lost = 0,
  gained = 1,
};

enum class MouseInputAction : std::uint8_t {
  press = 0,
  release = 1,
  motion = 2,
};

enum class MouseInputButton : std::uint8_t {
  none = 0,
  left = 1,
  right = 2,
  middle = 3,
  four = 4,
  five = 5,
  six = 6,
  seven = 7,
  eight = 8,
  nine = 9,
  ten = 10,
  eleven = 11,
};

struct MouseInput final {
  MouseInputAction action{MouseInputAction::motion};
  MouseInputButton button{MouseInputButton::none};
  std::uint16_t modifiers{0};
  std::uint16_t column{0};
  std::uint16_t row{0};
  Dimensions geometry{};
  bool any_button_pressed{false};

  [[nodiscard]] constexpr auto operator==(const MouseInput&) const noexcept -> bool = default;
};

inline constexpr std::size_t mouse_input_wire_bytes = 13;

enum class ServerMessageKind : std::uint8_t {
  hello,
  render_frame,
  disconnect,
};

enum class DecodeError : std::uint8_t {
  invalid_magic,
  version_mismatch,
  invalid_kind,
  invalid_flags,
  invalid_length,
  oversized,
  invalid_sequence,
  invalid_enum,
  invalid_dimensions,
  invalid_session,
  invalid_generation,
  buffer_full,
  allocation_failed,
};

struct ClientMessage final {
  ClientMessageKind kind{ClientMessageKind::detach};
  Dimensions dimensions{};
  CellSize cell_size{};
  bool clipboard_supported{false};
  PaneCommand pane_command{PaneCommand::none};
  KeyInput key{};
  FocusInput focus{FocusInput::lost};
  MouseInput mouse{};
  // Borrowed from ClientDecoder until consume() or reset().
  const HostTerminalTheme* host_theme{nullptr};
  std::string_view session;
  // Legacy input and paste payloads borrow ClientDecoder storage. Paste is mutable because
  // Ghostty filters unsafe controls in place before encoding.
  std::span<std::byte> input;
  std::uint32_t sequence{0};
};

static_assert(sizeof(ClientMessage) <= 96U);

struct ServerMessage final {
  ServerMessageKind kind{ServerMessageKind::disconnect};
  Dimensions dimensions{};
  DisconnectReason reason{DisconnectReason::internal_error};
  std::string_view diagnostic;
  std::span<const std::byte> ansi;
  std::uint32_t sequence{0};
  std::uint32_t full_redraw_generation{0};
  bool full_redraw{false};
};

class SmallMessage final {
public:
  [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte> {
    return std::span(storage_).first(size_);
  }

private:
  friend auto encode_client_hello(std::string_view session, Dimensions dimensions,
                                  std::uint32_t sequence, ProtocolVersion version,
                                  const std::optional<HostTerminalTheme>& host_theme) noexcept
      -> SmallMessage;
  friend auto encode_daemon_hello(Dimensions dimensions, std::uint32_t sequence) noexcept
      -> SmallMessage;
  friend auto encode_resize(Dimensions dimensions, std::uint32_t sequence) noexcept -> SmallMessage;
  friend auto encode_detach(std::uint32_t sequence) noexcept -> SmallMessage;
  friend auto encode_pane_command(PaneCommand command, std::uint32_t sequence) noexcept
      -> SmallMessage;
  friend auto encode_host_theme_update(const HostTerminalTheme& theme,
                                       std::uint32_t sequence) noexcept -> SmallMessage;
  friend auto encode_key(const KeyInput& key, std::span<const std::byte> text,
                         std::uint32_t sequence) noexcept -> SmallMessage;
  friend auto encode_focus(FocusInput focus, std::uint32_t sequence) noexcept -> SmallMessage;
  friend auto encode_mouse(const MouseInput& mouse, std::uint32_t sequence) noexcept
      -> SmallMessage;
  friend auto encode_cell_size(CellSize size, std::uint32_t sequence) noexcept -> SmallMessage;
  friend auto encode_clipboard_support(bool supported, std::uint32_t sequence) noexcept
      -> SmallMessage;
  friend auto encode_disconnect(DisconnectReason reason, std::string_view diagnostic,
                                std::uint32_t sequence) noexcept -> SmallMessage;

  std::array<std::byte, small_message_bytes_max> storage_{};
  std::size_t size_{0};
};

[[nodiscard]] constexpr auto wire_byte(const ControlCommand command) noexcept -> std::byte {
  return static_cast<std::byte>(command);
}

[[nodiscard]] constexpr auto wire_byte(const ControlResponse response) noexcept -> std::byte {
  return static_cast<std::byte>(response);
}

[[nodiscard]] auto encode_session_header(ControlCommand command, std::string_view session) noexcept
    -> std::array<std::byte, 2>;
[[nodiscard]] auto encode_dimensions(Dimensions dimensions) noexcept -> std::array<std::byte, 4>;
[[nodiscard]] auto encode_bounded_size(std::size_t size) noexcept -> std::array<std::byte, 2>;
[[nodiscard]] auto decode_dimensions(std::span<const std::byte, 4> bytes) noexcept -> Dimensions;
[[nodiscard]] auto decode_bounded_size(std::span<const std::byte, 2> bytes) noexcept -> std::size_t;
[[nodiscard]] constexpr auto decode_session_name_size(const std::byte value) noexcept
    -> std::size_t {
  return std::to_integer<std::size_t>(value);
}

[[nodiscard]] auto encode_header(MessageKind kind, std::uint8_t flags, std::uint32_t payload_bytes,
                                 std::uint32_t sequence,
                                 ProtocolVersion version = current_version) noexcept
    -> std::array<std::byte, attach_header_bytes>;
[[nodiscard]] auto
encode_client_hello(std::string_view session, Dimensions dimensions, std::uint32_t sequence = 1,
                    ProtocolVersion version = current_version,
                    const std::optional<HostTerminalTheme>& host_theme = std::nullopt) noexcept
    -> SmallMessage;
[[nodiscard]] auto encode_daemon_hello(Dimensions dimensions, std::uint32_t sequence = 1) noexcept
    -> SmallMessage;
[[nodiscard]] auto encode_input_header(std::size_t bytes, std::uint32_t sequence) noexcept
    -> std::array<std::byte, attach_header_bytes>;
[[nodiscard]] auto encode_paste_header(std::size_t bytes, std::uint32_t sequence) noexcept
    -> std::array<std::byte, attach_header_bytes>;
[[nodiscard]] auto encode_cell_size(CellSize size, std::uint32_t sequence) noexcept -> SmallMessage;
[[nodiscard]] auto encode_clipboard_support(bool supported, std::uint32_t sequence) noexcept
    -> SmallMessage;
// Payloads are complete OSC 5522 records or bounded consecutive parts of one OSC 52 reply.
[[nodiscard]] auto encode_terminal_reply_header(std::size_t bytes, std::uint32_t sequence) noexcept
    -> std::array<std::byte, attach_header_bytes>;
[[nodiscard]] auto encode_key(const KeyInput& key, std::span<const std::byte> text,
                              std::uint32_t sequence) noexcept -> SmallMessage;
[[nodiscard]] auto encode_focus(FocusInput focus, std::uint32_t sequence) noexcept -> SmallMessage;
[[nodiscard]] auto encode_mouse(const MouseInput& mouse, std::uint32_t sequence) noexcept
    -> SmallMessage;
[[nodiscard]] auto encode_resize(Dimensions dimensions, std::uint32_t sequence) noexcept
    -> SmallMessage;
[[nodiscard]] auto encode_detach(std::uint32_t sequence) noexcept -> SmallMessage;
[[nodiscard]] auto encode_pane_command(PaneCommand command, std::uint32_t sequence) noexcept
    -> SmallMessage;
[[nodiscard]] auto encode_host_theme_update(const HostTerminalTheme& theme,
                                            std::uint32_t sequence) noexcept -> SmallMessage;
[[nodiscard]] auto encode_render_frame_header(std::size_t ansi_bytes, std::uint32_t sequence,
                                              std::uint32_t full_redraw_generation,
                                              bool full_redraw) noexcept
    -> std::array<std::byte, attach_header_bytes + render_generation_bytes>;
[[nodiscard]] auto encode_disconnect(DisconnectReason reason, std::string_view diagnostic,
                                     std::uint32_t sequence = 1) noexcept -> SmallMessage;
[[nodiscard]] auto decode_error_diagnostic(DecodeError error) noexcept -> std::string_view;

// Daemon-side incremental decoder. Returned payload views borrow storage until consume() or
// reset().
class ClientDecoder final {
public:
  ClientDecoder() = default;
  ClientDecoder(const ClientDecoder&) = delete;
  auto operator=(const ClientDecoder&) -> ClientDecoder& = delete;
  ClientDecoder(ClientDecoder&&) noexcept = default;
  auto operator=(ClientDecoder&&) noexcept -> ClientDecoder& = default;
  ~ClientDecoder() = default;

  [[nodiscard]] auto prepare() noexcept -> std::expected<void, DecodeError>;
  void release() noexcept;
  [[nodiscard]] auto writable_bytes() noexcept -> std::span<std::byte>;
  [[nodiscard]] auto commit(std::size_t bytes) noexcept -> std::expected<void, DecodeError>;
  [[nodiscard]] auto next() noexcept -> std::expected<std::optional<ClientMessage>, DecodeError>;
  void consume() noexcept;
  void reset(std::uint32_t expected_sequence = 1, bool expect_hello = true) noexcept;

private:
  [[nodiscard]] auto mutable_storage() noexcept -> std::span<std::byte>;
  [[nodiscard]] auto storage() const noexcept -> std::span<const std::byte>;

  static constexpr std::size_t inline_storage_bytes =
      attach_header_bytes + legacy_input_message_bytes_max;

  // Setup and ordinary input stay inline. Only an accepted live decoder that sees a large paste
  // envelope grows to the structured-input bound.
  std::array<std::byte, inline_storage_bytes> inline_storage_{};
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::unique_ptr<std::byte[]> expanded_storage_;
  HostTerminalTheme host_theme_{};
  std::size_t used_{0};
  std::size_t pending_size_{0};
  std::uint32_t expected_sequence_{1};
  bool expect_hello_{true};
  bool prepared_{false};
};

// Attached-client incremental decoder. Its one bounded RAII allocation is prepared before terminal
// mutation; render payloads borrow it until consume() or reset().
class ServerDecoder final {
public:
  ServerDecoder() = default;
  ServerDecoder(const ServerDecoder&) = delete;
  auto operator=(const ServerDecoder&) -> ServerDecoder& = delete;
  ServerDecoder(ServerDecoder&&) = delete;
  auto operator=(ServerDecoder&&) -> ServerDecoder& = delete;
  ~ServerDecoder() = default;

  [[nodiscard]] auto prepare() noexcept -> std::expected<void, DecodeError>;
  [[nodiscard]] auto writable_bytes() noexcept -> std::span<std::byte>;
  [[nodiscard]] auto commit(std::size_t bytes) noexcept -> std::expected<void, DecodeError>;
  [[nodiscard]] auto next() noexcept -> std::expected<std::optional<ServerMessage>, DecodeError>;
  void consume() noexcept;
  void reset(std::uint32_t expected_sequence = 1, bool expect_hello = true) noexcept;

private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::unique_ptr<std::byte[]> storage_;
  std::size_t used_{0};
  std::size_t pending_size_{0};
  std::uint32_t expected_sequence_{1};
  std::uint32_t full_redraw_generation_{0};
  std::uint32_t pending_generation_{0};
  bool pending_full_redraw_{false};
  bool expect_hello_{true};
};

} // namespace frame::protocol

#endif // FRAME_PROTOCOL_ATTACHMENT_HPP
