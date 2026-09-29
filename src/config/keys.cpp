#include "config/config.hpp"
#include "input/input_router.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace frame::config {
namespace {
using input::PhysicalKey;
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
[[nodiscard]] constexpr auto physical_key(const std::string_view name) noexcept
    -> std::optional<PhysicalKey> {
  if (name == "Enter") {
    return PhysicalKey::enter;
  }
  if (name == "Tab") {
    return PhysicalKey::tab;
  }
  if (name == "Backspace") {
    return PhysicalKey::backspace;
  }
  if (name == "Escape" || name == "Esc") {
    return PhysicalKey::escape;
  }
  if (name == "Up") {
    return PhysicalKey::arrow_up;
  }
  if (name == "Down") {
    return PhysicalKey::arrow_down;
  }
  if (name == "Left") {
    return PhysicalKey::arrow_left;
  }
  if (name == "Right") {
    return PhysicalKey::arrow_right;
  }
  if (name == "Home") {
    return PhysicalKey::home;
  }
  if (name == "End") {
    return PhysicalKey::end;
  }
  if (name == "Insert") {
    return PhysicalKey::insert;
  }
  if (name == "Delete") {
    return PhysicalKey::delete_key;
  }
  if (name == "PageUp") {
    return PhysicalKey::page_up;
  }
  if (name == "PageDown") {
    return PhysicalKey::page_down;
  }
  constexpr std::array functions{
      PhysicalKey::f1, PhysicalKey::f2,  PhysicalKey::f3,  PhysicalKey::f4,
      PhysicalKey::f5, PhysicalKey::f6,  PhysicalKey::f7,  PhysicalKey::f8,
      PhysicalKey::f9, PhysicalKey::f10, PhysicalKey::f11, PhysicalKey::f12,
  };
  if (name.size() >= 2U && name.front() == 'F') {
    unsigned number = 0;
    for (const char character : name.substr(1)) {
      if (character < '0' || character > '9') {
        return std::nullopt;
      }
      number = (number * 10U) + static_cast<unsigned>(character - '0');
    }
    if (number > 0U && number <= functions.size()) {
      return functions.at(number - 1U);
    }
  }
  return std::nullopt;
}

} // namespace
using input::InputChord;
// Key names intentionally describe physical command chords rather than terminal escape strings.
// Printable ASCII remains a byte chord so structured and legacy clients share the fast lookup.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto parse_key(std::string_view value) noexcept -> std::optional<InputChord> {
  if (value.empty() || value.contains('\0')) {
    return std::nullopt;
  }
  std::uint16_t modifiers = 0;
  const auto take = [&value, &modifiers](const std::string_view prefix,
                                         const std::uint16_t modifier) {
    if (!value.starts_with(prefix) || (modifiers & modifier) != 0U) {
      return false;
    }
    modifiers = static_cast<std::uint16_t>(modifiers | modifier);
    value.remove_prefix(prefix.size());
    return true;
  };
  bool consumed = true;
  while (consumed) {
    consumed = take("C-", input::key_modifier_control) || take("S-", input::key_modifier_shift) ||
               take("M-", input::key_modifier_alt) || take("A-", input::key_modifier_alt) ||
               take("Super-", input::key_modifier_super) ||
               take("Cmd-", input::key_modifier_super) ||
               take("Command-", input::key_modifier_super) ||
               take("Win-", input::key_modifier_super) || take("D-", input::key_modifier_super);
  }
  if (value == "Space") {
    return InputChord::byte(' ', modifiers);
  }
  if (value == "Enter") {
    return InputChord::byte(0x0DU, modifiers);
  }
  if (value == "Tab") {
    return InputChord::byte(0x09U, modifiers);
  }
  if (value == "Backspace") {
    return InputChord::byte(0x7FU, modifiers);
  }
  if (value == "Escape" || value == "Esc") {
    return InputChord::byte(0x1BU, modifiers);
  }
  if (value.size() == 1U) {
    auto byte = static_cast<std::uint8_t>(value.front());
    if (byte < 0x20U || byte > 0x7EU) {
      return std::nullopt;
    }
    if (modifiers == input::key_modifier_shift && byte >= 'a' && byte <= 'z') {
      byte = static_cast<std::uint8_t>(byte - static_cast<std::uint8_t>('a') +
                                       static_cast<std::uint8_t>('A'));
      modifiers = 0;
    }
    return InputChord::byte(byte, modifiers);
  }
  const auto key = physical_key(value);
  return key.has_value() ? std::optional{InputChord::key(*key, modifiers)} : std::nullopt;
}

} // namespace frame::config
