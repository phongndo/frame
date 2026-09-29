#ifndef FRAME_BASE64_HPP
#define FRAME_BASE64_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frame::base64 {
// Allocation belongs to the caller's cold-path failure boundary. Decoding is strict, including
// canonical padding bits; no malformed payload is interpreted as an alternative encoding.
inline auto encode(const std::string_view bytes) -> std::string {
  constexpr std::string_view digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t i = 0; i < bytes.size(); i += 3U) {
    const auto remaining = bytes.size() - i;
    const auto value =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(i))) << 16U) |
        (remaining > 1U
             ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(i + 1U))) << 8U
             : 0U) |
        (remaining > 2U ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.at(i + 2U)))
                        : 0U);
    result += digits.at((value >> 18U) & 63U);
    result += digits.at((value >> 12U) & 63U);
    result += remaining > 1U ? digits.at((value >> 6U) & 63U) : '=';
    result += remaining > 2U ? digits.at(value & 63U) : '=';
  }
  return result;
}
// One strict quartet pass keeps padding, size, and alphabet rejection in the same boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
inline auto decode(const std::string_view text, const std::size_t limit)
    -> std::optional<std::string> {
  if (text.size() % 4U != 0 || text.size() / 4U * 3U > limit + 2U) {
    return std::nullopt;
  }
  const auto digit = [](const char value) -> int {
    if (value >= 'A' && value <= 'Z') {
      return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
      return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
      return value - '0' + 52;
    }
    if (value == '+') {
      return 62;
    }
    if (value == '/') {
      return 63;
    }
    return -1;
  };
  std::string result;
  result.reserve(text.size() / 4U * 3U);
  for (std::size_t i = 0; i < text.size(); i += 4U) {
    const bool pad2 = text.at(i + 2U) == '=';
    const bool pad3 = text.at(i + 3U) == '=';
    const int first = digit(text.at(i));
    const int second = digit(text.at(i + 1U));
    const int third = pad2 ? 0 : digit(text.at(i + 2U));
    const int fourth = pad3 ? 0 : digit(text.at(i + 3U));
    if (first < 0 || second < 0 || third < 0 || fourth < 0 || (pad2 && !pad3) ||
        ((pad2 || pad3) && i + 4U != text.size()) || (pad2 && (second & 15) != 0) ||
        (pad3 && !pad2 && (third & 3) != 0)) {
      return std::nullopt;
    }
    const auto value =
        (static_cast<std::uint32_t>(first) << 18U) | (static_cast<std::uint32_t>(second) << 12U) |
        (static_cast<std::uint32_t>(third) << 6U) | static_cast<std::uint32_t>(fourth);
    result += static_cast<char>((value >> 16U) & 255U);
    if (!pad2) {
      result += static_cast<char>((value >> 8U) & 255U);
    }
    if (!pad3) {
      result += static_cast<char>(value & 255U);
    }
  }
  if (result.size() > limit) {
    return std::nullopt;
  }
  return result;
}
} // namespace frame::base64
#endif
