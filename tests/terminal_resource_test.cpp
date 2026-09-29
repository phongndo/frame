#include "frame/terminal/terminal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace frame::vt {
namespace {

void write_text(Terminal& terminal, const std::string_view text) {
  terminal.write(std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto make_terminal(const TerminalOptions& options = {}) -> Terminal {
  auto result = Terminal::create(options);
  EXPECT_TRUE(result.has_value());
  return std::move(result).value();
}

// Assertion macros and explicit bounded work loops dominate these extended resource checks.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(TerminalResourceTest, GrowsAndPrunesScrollbackUnderItsOwnerQuota) {
  TerminalOptions options;
  options.size = {.columns = 10, .rows = 2};
  options.scrollback_bytes_max = 1'000;
  auto terminal = make_terminal(options);
  constexpr std::string_view line = "history\r\n";
  constexpr std::size_t input_rows = 10'000;

  const auto initial = terminal.scrollback_rows();
  ASSERT_TRUE(initial.has_value());
  EXPECT_EQ(*initial, 0U);
  for (std::size_t row = 0; row < input_rows; ++row) {
    write_text(terminal, line);
  }

  const auto retained = terminal.scrollback_rows();
  ASSERT_TRUE(retained.has_value());
  EXPECT_GT(*retained, 0U);
  EXPECT_LT(*retained, input_rows);
  EXPECT_LE(options.scrollback_bytes_max, limits::terminal_scrollback_bytes_hard_max);
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalResourceTest, DefaultScrollbackRetainsMultipleGhosttyPages) {
  constexpr std::string_view line =
      "history-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789------\r\n";
  // Debug Ghostty verifies every cell in the growing page on each linefeed. Generate only
  // enough history to prune the small quota and retain more than 10,000 rows with the default.
  constexpr std::size_t small_input_rows = 2'000;
  constexpr std::size_t default_input_rows = 12'000;

  TerminalOptions small_options;
  small_options.size = {.columns = 80, .rows = 23};
  small_options.scrollback_bytes_max = 1'000'000;
  auto small = make_terminal(small_options);
  for (std::size_t row = 0; row < small_input_rows; ++row) {
    write_text(small, line);
  }
  const auto small_rows = small.scrollback_rows();
  ASSERT_TRUE(small_rows.has_value());
  EXPECT_GT(*small_rows, 0U);
  EXPECT_LT(*small_rows, small_input_rows - small_options.size.rows + 1U);

  TerminalOptions default_options;
  default_options.size = small_options.size;
  auto terminal = make_terminal(default_options);
  for (std::size_t row = 0; row < default_input_rows; ++row) {
    write_text(terminal, line);
  }
  const auto retained = terminal.scrollback_rows();
  ASSERT_TRUE(retained.has_value());
  EXPECT_EQ(*retained, default_input_rows - default_options.size.rows + 1U);
  EXPECT_GT(*retained, 10'000U);
  EXPECT_GT(*retained, *small_rows * 10U);
}
TEST(TerminalResourceTest, AllocationQuotaRejectsCreationWithoutPublishingPartialState) {
  TerminalOptions options;
  options.size = {.columns = 20, .rows = 3};
  auto probe = make_terminal(options);
  const auto creation_peak = probe.allocation_stats().bytes_peak;
  ASSERT_GT(creation_peak, 1U);

  const std::array rejected_quotas{
      std::size_t{1},
      std::max(std::size_t{1}, creation_peak / 2U),
      creation_peak - 1U,
  };
  for (const auto quota : rejected_quotas) {
    SCOPED_TRACE(testing::Message() << "allocation quota=" << quota);
    options.allocation_bytes_max = quota;
    EXPECT_FALSE(Terminal::create(options).has_value());
  }

  options.allocation_bytes_max = creation_peak;
  auto exact = Terminal::create(options);
  ASSERT_TRUE(exact.has_value());
  EXPECT_LE(exact->allocation_stats().bytes_peak, creation_peak);
  EXPECT_FALSE(exact->integrity_failed());
}

TEST(TerminalResourceTest, CompressesScrollbackIncrementallyWithoutChangingLogicalContent) {
  TerminalOptions options;
  options.size = {.columns = 20, .rows = 3};
  options.scrollback_bytes_max = limits::terminal_scrollback_bytes_hard_max;
  auto terminal = make_terminal(options);
  const auto activity_before = terminal.compression_activity();
  ASSERT_TRUE(activity_before.has_value());
  for (std::size_t row = 0; row < 2'000; ++row) {
    write_text(terminal, "compressible history\r\n");
  }
  const auto activity_after = terminal.compression_activity();
  ASSERT_TRUE(activity_after.has_value());
  EXPECT_NE(*activity_before, *activity_after);
  const auto rows_before = terminal.scrollback_rows();
  ASSERT_TRUE(rows_before.has_value());

  CompressionResult compression = CompressionResult::pending;
  for (std::size_t step = 0; step < 10'000 && compression == CompressionResult::pending; ++step) {
    const auto result = terminal.compress_scrollback();
    ASSERT_TRUE(result.has_value());
    compression = *result;
  }
  EXPECT_NE(compression, CompressionResult::pending);
  EXPECT_EQ(terminal.scrollback_rows(), rows_before);
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace frame::vt
