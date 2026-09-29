#include "frame/terminal/terminal.hpp"

#include <ghostty/vt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace frame::vt {
namespace {

// The direct exclusions mirror Unicode scalar/control/noncharacter categories.
// NOLINTBEGIN(readability-simplify-boolean-expr)
[[nodiscard]] constexpr auto printable_grid_codepoint(const std::uint32_t value) noexcept -> bool {
  return value >= 0x20U && value != 0x7fU && !(value >= 0x80U && value <= 0x9fU) &&
         !(value >= 0xd800U && value <= 0xdfffU) && value <= 0x10ffffU &&
         !(value >= 0xfdd0U && value <= 0xfdefU) && (value & 0xffffU) != 0xfffeU &&
         (value & 0xffffU) != 0xffffU;
}
// NOLINTEND(readability-simplify-boolean-expr)

[[nodiscard]] auto decode_grid_codepoint(const std::string_view text, std::size_t& offset) noexcept
    -> std::optional<std::uint32_t> {
  if (offset >= text.size()) {
    return std::nullopt;
  }
  const auto first = static_cast<std::uint8_t>(std::span(text).subspan(offset, 1).front());
  std::uint32_t value = 0;
  std::size_t count = 0;
  std::uint32_t minimum = 0;
  if (first < 0x80U) {
    value = first;
    count = 1;
  } else if ((first & 0xe0U) == 0xc0U) {
    value = first & 0x1fU;
    count = 2;
    minimum = 0x80U;
  } else if ((first & 0xf0U) == 0xe0U) {
    value = first & 0x0fU;
    count = 3;
    minimum = 0x800U;
  } else if ((first & 0xf8U) == 0xf0U) {
    value = first & 0x07U;
    count = 4;
    minimum = 0x10000U;
  } else {
    return std::nullopt;
  }
  if (count > text.size() - offset) {
    return std::nullopt;
  }
  for (std::size_t index = 1; index < count; ++index) {
    const auto continuation =
        static_cast<std::uint8_t>(std::span(text).subspan(offset + index, 1).front());
    if ((continuation & 0xc0U) != 0x80U) {
      return std::nullopt;
    }
    value = (value << 6U) | (continuation & 0x3fU);
  }
  if (value < minimum || !printable_grid_codepoint(value)) {
    return std::nullopt;
  }
  offset += count;
  return value;
}

} // namespace

[[nodiscard]] auto measure_grid_text(const std::string_view text) noexcept
    -> std::expected<TextMetrics, Error> {
  if (std::ranges::all_of(text, [](const char character) {
        const auto byte = static_cast<std::uint8_t>(character);
        return byte >= 0x20U && byte <= 0x7eU;
      })) {
    return TextMetrics{.codepoints = text.size(), .graphemes = text.size(), .columns = text.size()};
  }
  try {
    std::vector<std::uint32_t> codepoints;
    codepoints.reserve(text.size());
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto value = decode_grid_codepoint(text, offset);
      if (!value.has_value()) {
        return std::unexpected(Error::invalid_options);
      }
      codepoints.push_back(*value);
    }

    TextMetrics result{.codepoints = codepoints.size()};
    for (std::size_t index = 0; index < codepoints.size();) {
      std::uint8_t width = 0;
      // The pinned C API takes a pointer to the current suffix.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      const auto consumed = ghostty_unicode_grapheme_width(codepoints.data() + index,
                                                           codepoints.size() - index, &width);
      if (consumed == 0 || consumed > codepoints.size() - index) {
        return std::unexpected(Error::invalid_state);
      }
      if (width > 2U || result.columns > std::numeric_limits<std::size_t>::max() - width) {
        return std::unexpected(Error::limit_exceeded);
      }
      result.columns += width;
      ++result.graphemes;
      index += consumed;
    }
    return result;
  } catch (...) {
    return std::unexpected(Error::out_of_memory);
  }
}

} // namespace frame::vt
