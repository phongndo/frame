#include "core/input.hpp"

#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "protocol/attachment.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace lemma::core {
namespace {

void write_terminal(vt::Terminal& terminal, const std::string_view bytes) noexcept {
  terminal.write(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(CoreInputTest, ClipboardReplyHeadroomDoesNotIncreaseInputAdmission) {
  auto terminal = vt::Terminal::create({}).value();
  terminal.set_clipboard_access(true, false);
  PanePtyWriteQueue queue;
  const std::vector<std::byte> prior(limits::terminal_pty_response_bytes_max +
                                         limits::normalized_client_input_bytes_max,
                                     std::byte{'x'});
  ASSERT_TRUE(queue.append(prior));
  std::array key{std::byte{'Y'}};
  EXPECT_EQ(queue_normalized_input(queue, terminal, key), InputQueueResult::full);
  EXPECT_EQ(queue_paste_input(queue, terminal, key), InputQueueResult::full);

  write_terminal(terminal, "\x1b]5522;type=read:id=headroom;aW1hZ2UvcG5n\x1b\\");
  const auto request = terminal.clipboard_request().value_or(vt::ClipboardRequest{});
  ASSERT_NE(request.id, 0U);
  const std::vector<std::byte> image(std::size_t{64} * 1024U, std::byte{'p'});
  const std::array content{vt::ClipboardContent{.mime = "image/png", .data = image}};
  ASSERT_TRUE(terminal.complete_clipboard(request.id, vt::ClipboardStatus::success, content));
  const auto reply_size = terminal.pending_pty_response_bytes();
  ASSERT_GT(reply_size, 0U);
  ASSERT_TRUE(queue_terminal_responses(queue, terminal));
  EXPECT_EQ(queue.size(), prior.size() + reply_size);
  EXPECT_EQ(queue_normalized_input(queue, terminal, key), InputQueueResult::full);
  ASSERT_TRUE(queue.consume(prior.size()));
  EXPECT_EQ(queue.readable_span().front(), std::byte{0x1b});
  ASSERT_TRUE(queue.consume(reply_size));
  EXPECT_EQ(queue_normalized_input(queue, terminal, key), InputQueueResult::queued);
  EXPECT_EQ(queue.readable_span().front(), std::byte{'Y'});
}

TEST(CoreInputTest, EncodesEnterSemanticallyWhenKittyKeyboardModeIsActive) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  write_terminal(terminal, "\x1B[>1u");

  PanePtyWriteQueue queue;
  const std::array input{std::byte{'l'}, std::byte{'s'}, std::byte{0x0D}};

  ASSERT_EQ(queue_normalized_input(queue, terminal, input), InputQueueResult::queued);
  std::array<std::byte, input.size()> output{};
  ASSERT_EQ(queue.read(output), output.size());
  EXPECT_EQ(output, input);
}

TEST(CoreInputTest, NormalizesSpecialKeysBetweenPassThroughRuns) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  // Application cursor keys make arrows observable: they must be re-encoded, not copied along
  // with the printable and UTF-8 runs around them.
  write_terminal(terminal, "\x1B[?1h");

  PanePtyWriteQueue queue;
  constexpr std::string_view input = "ab\x1B[Acd\xC3\xA9\x1B[Bz\x1B";
  constexpr std::string_view expected = "ab\x1BOAcd\xC3\xA9\x1BOBz\x1B";
  const auto bytes = std::as_bytes(std::span(input.data(), input.size()));

  ASSERT_EQ(queue_normalized_input(queue, terminal, bytes), InputQueueResult::queued);
  std::array<std::byte, 32> output{};
  const auto size = queue.read(output);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(output.data()), size), expected);

  // A held message that does not fit is rejected whole, leaving the queue unchanged.
  const std::vector<std::byte> prior(queue.input_remaining() - 4U, std::byte{'x'});
  ASSERT_TRUE(queue.append(prior));
  EXPECT_EQ(queue_normalized_input(queue, terminal, bytes), InputQueueResult::full);
  EXPECT_EQ(queue.size(), prior.size());
}

TEST(CoreInputTest, EncodesTypedPrintableKeyThroughGhostty) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  PanePtyWriteQueue queue;
  const protocol::KeyInput key{
      .action = protocol::KeyInputAction::press,
      .key = protocol::KeyInputKey::unidentified,
      .modifiers = 0,
      .consumed_modifiers = 0,
      .unshifted_codepoint = '7',
      .composing = false,
  };
  const std::array text{std::byte{'7'}};

  ASSERT_EQ(queue_key_input(queue, terminal, key, text), InputQueueResult::queued);
  std::array<std::byte, 8> output{};
  const auto size = queue.read(output);
  ASSERT_EQ(size, 1U);
  EXPECT_EQ(output.front(), std::byte{'7'});
}

TEST(CoreInputTest, EncodesShiftedAssociatedTextAsTextInLegacyChildMode) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  PanePtyWriteQueue queue;
  const protocol::KeyInput key{
      .action = protocol::KeyInputAction::press,
      .key = protocol::KeyInputKey::a,
      .modifiers = protocol::key_input_modifier_shift,
      .consumed_modifiers = 0,
      .unshifted_codepoint = 'a',
      .composing = false,
  };
  const std::array text{std::byte{'A'}};

  ASSERT_EQ(queue_key_input(queue, terminal, key, text), InputQueueResult::queued);
  std::array<std::byte, 8> output{};
  const auto size = queue.read(output);
  ASSERT_EQ(size, 1U);
  EXPECT_EQ(output.front(), std::byte{'A'});
}

TEST(CoreInputTest, CommitsDeferredPrefixAndStructuredKeyInOrder) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  PanePtyWriteQueue queue;
  constexpr std::array prefix{std::byte{0x02}};
  const protocol::KeyInput key{
      .action = protocol::KeyInputAction::press,
      .key = protocol::KeyInputKey::x,
      .unshifted_codepoint = 'x',
  };
  constexpr std::array text{std::byte{'x'}};

  ASSERT_EQ(queue_prefixed_key_input(queue, terminal, prefix, key, text), InputQueueResult::queued);
  std::array<std::byte, 2> output{};
  ASSERT_EQ(queue.read(output), output.size());
  EXPECT_EQ(output.front(), std::byte{0x02});
  EXPECT_EQ(output.back(), std::byte{'x'});
}

TEST(CoreInputTest, DeferredPrefixAndStructuredKeyLeaveFullQueueUnchanged) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  PanePtyWriteQueue queue;
  const std::vector<std::byte> filler(PanePtyWriteQueue::capacity());
  ASSERT_TRUE(queue.append(filler));
  const auto original_size = queue.size();
  constexpr std::array prefix{std::byte{0x02}};
  const protocol::KeyInput key{
      .action = protocol::KeyInputAction::press,
      .key = protocol::KeyInputKey::x,
      .unshifted_codepoint = 'x',
  };
  constexpr std::array text{std::byte{'x'}};

  EXPECT_EQ(queue_prefixed_key_input(queue, terminal, prefix, key, text), InputQueueResult::full);
  EXPECT_EQ(queue.size(), original_size);
}

TEST(CoreInputTest, EncodesOneAlternateScrollCursorKeyPerNormalizedWheelReport) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  PanePtyWriteQueue queue;

  ASSERT_EQ(queue_alternate_scroll_input(queue, terminal, true), InputQueueResult::queued);
  ASSERT_EQ(queue_alternate_scroll_input(queue, terminal, false), InputQueueResult::queued);
  std::array<std::byte, 8> output{};
  const auto size = queue.read(output);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(output.data()), size), "\x1B[A\x1B[B");
}

TEST(CoreInputTest, KeepsTypedPasteOpaqueFromMuxPrefixBytes) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  write_terminal(terminal, "\x1B[?2004h");
  PanePtyWriteQueue queue;
  std::array input{std::byte{'a'}, std::byte{0x02}, std::byte{'b'}};

  ASSERT_EQ(queue_paste_input(queue, terminal, input), InputQueueResult::queued);
  std::array<std::byte, 15> output{};
  const auto size = queue.read(output);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view encoded(reinterpret_cast<const char*>(output.data()), size);
  EXPECT_EQ(encoded, std::string_view("\x1B[200~a\x02"
                                      "b\x1B[201~",
                                      15));
}

TEST(CoreInputTest, LeavesFullQueueUnchangedForBackpressure) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  PanePtyWriteQueue queue;
  const std::vector<std::byte> filler(PanePtyWriteQueue::capacity());
  ASSERT_TRUE(queue.append(filler));
  const auto original_size = queue.size();
  const std::array input{std::byte{'x'}};

  EXPECT_EQ(queue_normalized_input(queue, terminal, input), InputQueueResult::full);
  EXPECT_EQ(queue.size(), original_size);
}

TEST(CoreInputTest, SuppliesTextForControlKeys) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);

  PanePtyWriteQueue queue;
  const std::array input{std::byte{0x03}};

  ASSERT_EQ(queue_normalized_input(queue, terminal, input), InputQueueResult::queued);
  std::array<std::byte, input.size()> output{};
  ASSERT_EQ(queue.read(output), output.size());
  EXPECT_EQ(output, input);
}

} // namespace
} // namespace lemma::core
