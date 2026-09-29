#include "terminal/terminal_impl.hpp"

#include "frame/assert.hpp"
#include "frame/limits.hpp"
#include "frame/terminal/terminal.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace frame::vt {
namespace {

constexpr std::uint16_t modifiers_valid =
    key_modifier_shift | key_modifier_control | key_modifier_alt | key_modifier_super |
    key_modifier_caps_lock | key_modifier_num_lock | key_modifier_shift_side |
    key_modifier_control_side | key_modifier_alt_side | key_modifier_super_side;

[[nodiscard]] constexpr auto ghostty_key_action(const KeyAction action) noexcept
    -> GhosttyKeyAction {
  switch (action) {
  case KeyAction::release:
    return GHOSTTY_KEY_ACTION_RELEASE;
  case KeyAction::press:
    return GHOSTTY_KEY_ACTION_PRESS;
  case KeyAction::repeat:
    return GHOSTTY_KEY_ACTION_REPEAT;
  }
  return GHOSTTY_KEY_ACTION_PRESS;
}

[[nodiscard]] auto ghostty_key(const Key key) noexcept -> GhosttyKey {
  constexpr std::array mapping{
      GHOSTTY_KEY_UNIDENTIFIED,
      GHOSTTY_KEY_A,
      GHOSTTY_KEY_B,
      GHOSTTY_KEY_C,
      GHOSTTY_KEY_D,
      GHOSTTY_KEY_E,
      GHOSTTY_KEY_F,
      GHOSTTY_KEY_G,
      GHOSTTY_KEY_H,
      GHOSTTY_KEY_I,
      GHOSTTY_KEY_J,
      GHOSTTY_KEY_K,
      GHOSTTY_KEY_L,
      GHOSTTY_KEY_M,
      GHOSTTY_KEY_N,
      GHOSTTY_KEY_O,
      GHOSTTY_KEY_P,
      GHOSTTY_KEY_Q,
      GHOSTTY_KEY_R,
      GHOSTTY_KEY_S,
      GHOSTTY_KEY_T,
      GHOSTTY_KEY_U,
      GHOSTTY_KEY_V,
      GHOSTTY_KEY_W,
      GHOSTTY_KEY_X,
      GHOSTTY_KEY_Y,
      GHOSTTY_KEY_Z,
      GHOSTTY_KEY_ENTER,
      GHOSTTY_KEY_TAB,
      GHOSTTY_KEY_BACKSPACE,
      GHOSTTY_KEY_ESCAPE,
      GHOSTTY_KEY_SPACE,
      GHOSTTY_KEY_ARROW_UP,
      GHOSTTY_KEY_ARROW_DOWN,
      GHOSTTY_KEY_ARROW_LEFT,
      GHOSTTY_KEY_ARROW_RIGHT,
      GHOSTTY_KEY_HOME,
      GHOSTTY_KEY_END,
      GHOSTTY_KEY_INSERT,
      GHOSTTY_KEY_DELETE,
      GHOSTTY_KEY_PAGE_UP,
      GHOSTTY_KEY_PAGE_DOWN,
      GHOSTTY_KEY_F1,
      GHOSTTY_KEY_F2,
      GHOSTTY_KEY_F3,
      GHOSTTY_KEY_F4,
      GHOSTTY_KEY_F5,
      GHOSTTY_KEY_F6,
      GHOSTTY_KEY_F7,
      GHOSTTY_KEY_F8,
      GHOSTTY_KEY_F9,
      GHOSTTY_KEY_F10,
      GHOSTTY_KEY_F11,
      GHOSTTY_KEY_F12,
  };
  static_assert(static_cast<std::size_t>(Key::f12) + 1U == mapping.size());
  const auto index = static_cast<std::size_t>(key);
  return index < mapping.size() ? std::span(mapping).subspan(index, 1).front()
                                : GHOSTTY_KEY_UNIDENTIFIED;
}

[[nodiscard]] constexpr auto ghostty_mouse_action(const MouseAction action) noexcept
    -> GhosttyMouseAction {
  switch (action) {
  case MouseAction::press:
    return GHOSTTY_MOUSE_ACTION_PRESS;
  case MouseAction::release:
    return GHOSTTY_MOUSE_ACTION_RELEASE;
  case MouseAction::motion:
    return GHOSTTY_MOUSE_ACTION_MOTION;
  }
  return GHOSTTY_MOUSE_ACTION_MOTION;
}

[[nodiscard]] constexpr auto ghostty_mouse_button(const MouseButton button) noexcept
    -> GhosttyMouseButton {
  switch (button) {
  case MouseButton::left:
    return GHOSTTY_MOUSE_BUTTON_LEFT;
  case MouseButton::right:
    return GHOSTTY_MOUSE_BUTTON_RIGHT;
  case MouseButton::middle:
    return GHOSTTY_MOUSE_BUTTON_MIDDLE;
  case MouseButton::four:
    return GHOSTTY_MOUSE_BUTTON_FOUR;
  case MouseButton::five:
    return GHOSTTY_MOUSE_BUTTON_FIVE;
  case MouseButton::six:
    return GHOSTTY_MOUSE_BUTTON_SIX;
  case MouseButton::seven:
    return GHOSTTY_MOUSE_BUTTON_SEVEN;
  case MouseButton::eight:
    return GHOSTTY_MOUSE_BUTTON_EIGHT;
  case MouseButton::nine:
    return GHOSTTY_MOUSE_BUTTON_NINE;
  case MouseButton::ten:
    return GHOSTTY_MOUSE_BUTTON_TEN;
  case MouseButton::eleven:
    return GHOSTTY_MOUSE_BUTTON_ELEVEN;
  }
  return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
}

} // namespace

auto Terminal::encode_key(const KeyEvent& event, const std::span<std::byte> output) noexcept
    -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  FRAME_ASSERT(impl_->key_encoder != nullptr);
  FRAME_ASSERT(impl_->key_event != nullptr);

  if ((event.modifiers & ~modifiers_valid) != 0 ||
      (event.consumed_modifiers & ~modifiers_valid) != 0 || event.text.size() > 256) {
    return std::unexpected(Error::invalid_options);
  }

  ghostty_key_encoder_setopt_from_terminal(impl_->key_encoder, impl_->terminal);
  ghostty_key_event_set_action(impl_->key_event, ghostty_key_action(event.action));
  ghostty_key_event_set_key(impl_->key_event, ghostty_key(event.key));
  ghostty_key_event_set_mods(impl_->key_event, static_cast<GhosttyMods>(event.modifiers));
  ghostty_key_event_set_consumed_mods(impl_->key_event,
                                      static_cast<GhosttyMods>(event.consumed_modifiers));
  ghostty_key_event_set_composing(impl_->key_event, event.composing);
  ghostty_key_event_set_unshifted_codepoint(impl_->key_event, event.unshifted_codepoint);
  ghostty_key_event_set_utf8(impl_->key_event, event.text.data(), event.text.size());

  // std::byte and char are both byte views; Ghostty's C ABI uses the latter.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* encoded = reinterpret_cast<char*>(output.data());
  std::size_t bytes_written = 0;
  const auto result = ghostty_key_encoder_encode(impl_->key_encoder, impl_->key_event, encoded,
                                                 output.size(), &bytes_written);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  FRAME_ASSERT(bytes_written <= output.size());
  return bytes_written;
}

auto Terminal::encode_paste(const std::span<std::byte> input,
                            const std::span<std::byte> output) noexcept
    -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  if (input.size() > limits::paste_payload_bytes_max) {
    return std::unexpected(Error::limit_exceeded);
  }

  GhosttyTerminalModeConfig bracketed{
      .mode = GHOSTTY_MODE_BRACKETED_PASTE,
      .value = false,
  };
  auto result = ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_MODE, &bracketed);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  // std::byte and char are both byte views; Ghostty's C ABI uses the latter.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* const input_data = reinterpret_cast<char*>(input.data());
  auto* const output_data = reinterpret_cast<char*>(output.data());
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  std::size_t bytes_written = 0;
  result = ghostty_paste_encode(input_data, input.size(), bracketed.value, output_data,
                                output.size(), &bytes_written);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  FRAME_ASSERT(bytes_written <= output.size());
  return bytes_written;
}

[[nodiscard]] auto Terminal::paste_is_safe(const std::span<const std::byte> input) const noexcept
    -> bool {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  if (input.size() > limits::paste_payload_bytes_max) {
    return false;
  }
  // std::byte and char are both byte views; Ghostty's C ABI uses the latter.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* const data = reinterpret_cast<const char*>(input.data());
  return ghostty_paste_is_safe(data, input.size());
}

auto Terminal::encode_focus(const FocusEvent event,
                            const std::span<std::byte> output) const noexcept
    -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  GhosttyTerminalModeConfig focus{.mode = GHOSTTY_MODE_FOCUS_EVENT, .value = false};
  auto result = ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_MODE, &focus);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  if (!focus.value) {
    return 0;
  }
  std::size_t written = 0;
  // std::byte and char are both byte views; Ghostty's C ABI uses the latter.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* const data = reinterpret_cast<char*>(output.data());
  result =
      ghostty_focus_encode(event == FocusEvent::gained ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST,
                           data, output.size(), &written);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  FRAME_ASSERT(written <= output.size());
  return written;
}

auto Terminal::encode_mouse(const MouseEvent& event, const std::span<std::byte> output) noexcept
    -> std::expected<std::size_t, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  FRAME_ASSERT(impl_->mouse_encoder != nullptr);
  FRAME_ASSERT(impl_->mouse_event != nullptr);
  const auto& geometry = event.geometry;
  if ((event.modifiers & ~modifiers_valid) != 0 || geometry.screen_width == 0 ||
      geometry.screen_height == 0 || geometry.cell_width == 0 || geometry.cell_height == 0 ||
      geometry.padding_left > geometry.screen_width ||
      geometry.padding_right > geometry.screen_width - geometry.padding_left ||
      geometry.padding_top > geometry.screen_height ||
      geometry.padding_bottom > geometry.screen_height - geometry.padding_top ||
      !std::isfinite(event.x) || !std::isfinite(event.y) || event.x < 0 || event.y < 0) {
    return std::unexpected(Error::invalid_options);
  }

  ghostty_mouse_encoder_setopt_from_terminal(impl_->mouse_encoder, impl_->terminal);
  const GhosttyMouseEncoderSize native_geometry{
      .size = sizeof(GhosttyMouseEncoderSize),
      .screen_width = geometry.screen_width,
      .screen_height = geometry.screen_height,
      .cell_width = geometry.cell_width,
      .cell_height = geometry.cell_height,
      .padding_top = geometry.padding_top,
      .padding_bottom = geometry.padding_bottom,
      .padding_right = geometry.padding_right,
      .padding_left = geometry.padding_left,
  };
  constexpr bool track_last_cell = true;
  ghostty_mouse_encoder_setopt(impl_->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE,
                               &native_geometry);
  ghostty_mouse_encoder_setopt(impl_->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                               &event.any_button_pressed);
  ghostty_mouse_encoder_setopt(impl_->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL,
                               &track_last_cell);
  ghostty_mouse_event_set_action(impl_->mouse_event, ghostty_mouse_action(event.action));
  if (event.button.has_value()) {
    ghostty_mouse_event_set_button(impl_->mouse_event, ghostty_mouse_button(*event.button));
  } else {
    ghostty_mouse_event_clear_button(impl_->mouse_event);
  }
  ghostty_mouse_event_set_mods(impl_->mouse_event, static_cast<GhosttyMods>(event.modifiers));
  ghostty_mouse_event_set_position(impl_->mouse_event, {.x = event.x, .y = event.y});

  std::size_t written = 0;
  // std::byte and char are both byte views; Ghostty's C ABI uses the latter.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto* const data = reinterpret_cast<char*>(output.data());
  const auto result = ghostty_mouse_encoder_encode(impl_->mouse_encoder, impl_->mouse_event, data,
                                                   output.size(), &written);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  FRAME_ASSERT(written <= output.size());
  return written;
}

[[nodiscard]] auto Terminal::mouse_tracking() const noexcept
    -> std::expected<MouseTrackingState, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);

  bool enabled = false;
  auto result =
      ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &enabled);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  GhosttyTerminalModeConfig any_motion{.mode = GHOSTTY_MODE_ANY_MOUSE, .value = false};
  result = ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_MODE, &any_motion);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return MouseTrackingState{
      .enabled = enabled,
      .unbuttoned_motion = any_motion.value,
  };
}

[[nodiscard]] auto Terminal::wheel_uses_alternate_scroll() const noexcept
    -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);

  bool tracking = false;
  GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
  GhosttyTerminalModeConfig alternate_scroll{
      .mode = GHOSTTY_MODE_ALT_SCROLL,
      .value = false,
  };
  const std::array keys{GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
                        GHOSTTY_TERMINAL_DATA_MODE};
  std::array<void*, keys.size()> values{&tracking, &screen, &alternate_scroll};
  std::size_t written = 0;
  const auto result = ghostty_terminal_get_multi(impl_->terminal, keys.size(), keys.data(),
                                                 values.data(), &written);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  if (written != keys.size()) {
    return std::unexpected(Error::invalid_state);
  }
  return !tracking && screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE && alternate_scroll.value;
}

auto Terminal::synchronized_output() const noexcept -> std::expected<bool, Error> {
  FRAME_ASSERT(impl_ != nullptr);
  FRAME_ASSERT(impl_->terminal != nullptr);
  GhosttyTerminalModeConfig synchronized{
      .mode = GHOSTTY_MODE_SYNC_OUTPUT,
      .value = false,
  };
  const auto result =
      ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_MODE, &synchronized);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return synchronized.value;
}

} // namespace frame::vt
