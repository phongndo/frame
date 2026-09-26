#include "core/outer_attention.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace lemma::core {
namespace {

using namespace std::chrono_literals;

constexpr auto origin = AttentionRateLimit::TimePoint{} + 1h;

[[nodiscard]] auto as_text(const std::span<const std::byte> bytes) -> std::string {
  std::string text;
  for (const auto byte : bytes) {
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(OuterAttentionTest, SanitizesTextAndReplacesFieldSeparators) {
  OuterText<16> text(';');
  text.append("a;b\x1B]\x07\xC2\x9D\xFF c");
  EXPECT_EQ(text.view(), "a b] c");

  // Truncation stops at a code point boundary rather than splitting UTF-8.
  OuterText<5> bounded;
  bounded.append("ab\xE2\x98\x83\xE2\x98\x83");
  EXPECT_EQ(bounded.view(), "ab\xE2\x98\x83");
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(OuterAttentionTest, RateLimitAdmitsBurstThenRefillsLazily) {
  AttentionRateLimit limit(2, 100ms);
  EXPECT_TRUE(limit.take(origin));
  EXPECT_TRUE(limit.take(origin + 10ms));
  EXPECT_FALSE(limit.take(origin + 20ms));
  EXPECT_EQ(limit.next_token(), origin + 100ms);
  EXPECT_FALSE(limit.take(origin + 99ms));
  EXPECT_TRUE(limit.take(origin + 100ms));
  EXPECT_FALSE(limit.take(origin + 150ms));
  EXPECT_EQ(limit.next_token(), origin + 200ms);

  // A long idle period refills only to the burst.
  EXPECT_TRUE(limit.take(origin + 10s));
  EXPECT_TRUE(limit.take(origin + 10s));
  EXPECT_FALSE(limit.take(origin + 10s));
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(OuterAttentionTest, EncodesNotificationProgressAndDirectory) {
  std::array<std::byte, 64> storage{};
  std::size_t used = 0;
  ASSERT_TRUE(append_outer_notification(storage, used, "work: build", "done; ok"));
  EXPECT_EQ(as_text(std::span(storage).first(used)), "\x1B]777;notify;work: build;done; ok\x1B\\");

  const auto progress = [](const vt::ProgressState state, const std::optional<std::uint8_t> value) {
    std::array<std::byte, limits::outer_progress_frame_bytes_max> bytes{};
    std::size_t size = 0;
    EXPECT_TRUE(append_outer_progress(bytes, size, state, value));
    return as_text(std::span(bytes).first(size));
  };
  EXPECT_EQ(progress(vt::ProgressState::normal, 100), "\x1B]9;4;1;100\x1B\\");
  EXPECT_EQ(progress(vt::ProgressState::normal, 7), "\x1B]9;4;1;7\x1B\\");
  EXPECT_EQ(progress(vt::ProgressState::normal, std::nullopt), "\x1B]9;4;1;0\x1B\\");
  EXPECT_EQ(progress(vt::ProgressState::error, 42), "\x1B]9;4;2;42\x1B\\");
  EXPECT_EQ(progress(vt::ProgressState::error, std::nullopt), "\x1B]9;4;2\x1B\\");
  EXPECT_EQ(progress(vt::ProgressState::indeterminate, 50), "\x1B]9;4;3\x1B\\");
  EXPECT_EQ(progress(vt::ProgressState::paused, 5), "\x1B]9;4;4;5\x1B\\");
  EXPECT_EQ(progress(vt::ProgressState::none, std::nullopt), "\x1B]9;4;0\x1B\\");

  used = 0;
  ASSERT_TRUE(append_outer_cwd(storage, used, "file://host/tmp"));
  EXPECT_EQ(as_text(std::span(storage).first(used)), "\x1B]7;file://host/tmp\x1B\\");
  EXPECT_TRUE(outer_cwd_forwardable("file://host/tmp/%E2%98%83"));
  EXPECT_FALSE(outer_cwd_forwardable(""));
  EXPECT_FALSE(outer_cwd_forwardable("file://host/tmp\x1B\\"));
  EXPECT_FALSE(outer_cwd_forwardable(std::string(limits::outer_cwd_bytes_max + 1U, 'a')));

  // A sequence that does not fit leaves the output unchanged.
  std::array<std::byte, 8> small{};
  used = 3;
  EXPECT_FALSE(append_outer_cwd(small, used, "file:///"));
  EXPECT_EQ(used, 3U);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(OuterAttentionTest, DecodesOnlyLocalOsc7Directories) {
  std::array<char, 32> storage{};
  EXPECT_EQ(local_directory_from_osc7("file:///tmp/project", "box", storage), "/tmp/project");
  EXPECT_EQ(local_directory_from_osc7("file://localhost/a%20b", "box", storage), "/a b");
  EXPECT_EQ(local_directory_from_osc7("FILE://BOX/x%2fy", "box", storage), "/x/y");
  EXPECT_EQ(local_directory_from_osc7("file://remote/tmp", "box", storage), std::nullopt);
  EXPECT_EQ(local_directory_from_osc7("file://remote/tmp", "", storage), std::nullopt);
  EXPECT_EQ(local_directory_from_osc7("kitty-shell-cwd://box/tmp", "box", storage), std::nullopt);
  EXPECT_EQ(local_directory_from_osc7("file://box", "box", storage), std::nullopt);
  EXPECT_EQ(local_directory_from_osc7("file:///a%00b", "box", storage), std::nullopt);
  EXPECT_EQ(local_directory_from_osc7("file:///a%zzb", "box", storage), std::nullopt);
  EXPECT_EQ(local_directory_from_osc7("file:///a%2", "box", storage), std::nullopt);
  EXPECT_EQ(
      local_directory_from_osc7("file:///" + std::string(storage.size(), 'a'), "box", storage),
      std::nullopt);
}

} // namespace
} // namespace lemma::core
