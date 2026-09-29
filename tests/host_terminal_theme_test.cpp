#include "client/host_terminal_theme.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace frame::client {
namespace {

[[nodiscard]] auto bytes(const std::string_view text) noexcept -> std::span<const std::byte> {
  return std::as_bytes(std::span(text.data(), text.size()));
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostTerminalThemeParserTest, DecodesFragmentedDefaultAndPaletteReplies) {
  constexpr std::string_view replies = "\x1B]10;rgb:d0d0/d0d0/d8d8\x1B\\"
                                       "\x1B]11;#0a0b0a\x07"
                                       "\x1B]4;1;rgb:ffff/8080/0000\x1B\\";
  for (std::size_t split = 0; split <= replies.size(); ++split) {
    HostTerminalThemeParser parser;
    parser.push(bytes(replies).first(split));
    parser.push(bytes(replies).subspan(split));
    parser.finish();
    const auto theme = parser.theme();
    ASSERT_TRUE(theme.has_value()) << split;
    const auto value = theme.value_or(protocol::HostTerminalTheme{});
    EXPECT_EQ(value.foreground, (protocol::RgbColor{.red = 0xD0, .green = 0xD0, .blue = 0xD8}));
    EXPECT_EQ(value.background, (protocol::RgbColor{.red = 0x0A, .green = 0x0B, .blue = 0x0A}));
    EXPECT_TRUE(value.has_palette_color(1));
    EXPECT_EQ(std::span(value.palette).subspan(1, 1).front(),
              (protocol::RgbColor{.red = 0xFF, .green = 0x80, .blue = 0x00}));
    EXPECT_TRUE(parser.pending_input().empty());
    EXPECT_FALSE(parser.overflowed());
  }
}

TEST(HostTerminalThemeParserTest, RetainsNonThemeAndMalformedInputExactly) {
  constexpr std::string_view input = "typed\x1B[A\x1B]2;title\x1B\\"
                                     "\x1B]4;16;rgb:ff/00/00\x1B\\"
                                     "\x1B]4;999;rgb:ff/00/00\x1B\\tail";
  HostTerminalThemeParser parser;

  parser.push(bytes(input));
  parser.finish();

  EXPECT_TRUE(parser.theme() == std::nullopt);
  EXPECT_TRUE(std::ranges::equal(parser.pending_input(), bytes(input)));
}

TEST(HostTerminalThemeParserTest, ReusesRetainedInputCapacityAfterForwarding) {
  const std::string input(protocol::input_bytes_max, 'x');
  HostTerminalThemeParser parser;

  parser.push(bytes(input));
  EXPECT_EQ(parser.pending_input().size(), input.size());
  parser.consume_pending_input();
  parser.push(bytes(input));

  EXPECT_EQ(parser.pending_input().size(), input.size());
  EXPECT_FALSE(parser.overflowed());
}

TEST(HostTerminalThemeParserTest, BuildsBoundedCompleteQuery) {
  std::array<char, host_theme_query_bytes_max> storage{};

  const auto size = encode_host_terminal_theme_query(storage);
  const std::string_view query(storage.data(), size);

  EXPECT_GT(size, 0U);
  EXPECT_TRUE(query.starts_with("\x1B]17;?\x1B\\\x1B]19;?\x1B\\\x1B]10;?\x1B\\\x1B]11;?\x1B\\"));
  EXPECT_TRUE(query.contains("\x1B]4;0;?\x1B\\"));
  EXPECT_TRUE(query.ends_with("\x1B]4;15;?\x1B\\"));
}

TEST(HostTerminalThemeParserTest, DecodesHighlightColorReplies) {
  constexpr std::string_view replies = "\x1B]17;rgb:2020/2020/3030\x1B\\"
                                       "\x1B]19;#c0c0c0\x07"
                                       "\x1B]10;#d0d0d8\x07"
                                       "\x1B]11;#0a0b0a\x07";
  HostTerminalThemeParser parser;
  parser.push(bytes(replies));
  parser.finish();
  const auto theme = parser.theme();
  ASSERT_TRUE(theme.has_value());
  const auto value = theme.value_or(protocol::HostTerminalTheme{});
  EXPECT_EQ(value.selection_background,
            (protocol::RgbColor{.red = 0x20, .green = 0x20, .blue = 0x30}));
  EXPECT_EQ(value.selection_foreground,
            (protocol::RgbColor{.red = 0xC0, .green = 0xC0, .blue = 0xC0}));
  EXPECT_EQ(value.foreground, (protocol::RgbColor{.red = 0xD0, .green = 0xD0, .blue = 0xD8}));
  EXPECT_EQ(value.background, (protocol::RgbColor{.red = 0x0A, .green = 0x0B, .blue = 0x0A}));
}

} // namespace
} // namespace frame::client
