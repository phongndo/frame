#include "client/host_input_parser.hpp"
#include "clipboard/transaction.hpp"
#include "frame/base64.hpp"

#include <gtest/gtest.h>

namespace frame::clipboard {
namespace {
// GoogleTest assertions check optionals before use; clang-tidy cannot follow their macro control
// flow. NOLINTBEGIN(bugprone-unchecked-optional-access)
void write(vt::Terminal& terminal, const std::string_view text) {
  terminal.write(std::as_bytes(std::span(text)));
}
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void deliver(vt::Terminal& terminal, Transaction& transaction) {
  client::HostInputParser parser;
  ASSERT_TRUE(parser.prepare());
  std::vector<std::byte> output(client::host_input_output_bytes_max);
  std::array<std::byte, 777> input{};
  while (const auto count = terminal.read_pty_responses(input)) {
    const auto batch = parser.parse(std::span(input).first(count), output, {});
    ASSERT_TRUE(batch);
    for (const auto& event : std::span(batch->events).first(batch->event_count)) {
      ASSERT_EQ(event.kind, transaction.protocol() == vt::ClipboardProtocol::osc52
                                ? client::HostInputKind::terminal_reply_stream
                                : client::HostInputKind::terminal_reply);
      ASSERT_LE(event.size, protocol::terminal_reply_bytes_max);
      const auto record = std::span(output).subspan(event.offset, event.size);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      const std::string_view text{reinterpret_cast<const char*>(record.data()), record.size()};
      transaction.consume(text);
    }
  }
  EXPECT_FALSE(parser.has_pending_sequence());
}
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ClipboardTest, Osc52MaximumReadPreservesProtocolWithBoundedReplyPackets) {
  auto outer = vt::Terminal::create({}).value();
  outer.set_clipboard_access(true, true);
  const std::array desired{vt::ClipboardContent{.mime = "text/plain", .data = {}}};
  Transaction transaction;
  const auto request = transaction.begin(
      {.id = 82, .protocol = vt::ClipboardProtocol::osc52, .read = true, .contents = desired},
      Transaction::Clock::now());
  ASSERT_TRUE(request);
  EXPECT_EQ(*request, "\x1b]52;c;?\x1b\\");
  transaction.published();
  EXPECT_TRUE(transaction.uncorrelated_read_outstanding());
  transaction.consume("\x1b]5522;type=read:id=old:status=DONE\x1b\\");
  EXPECT_FALSE(transaction.done());
  write(outer, *request);
  const auto pending = outer.clipboard_request();
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending->protocol, vt::ClipboardProtocol::osc52);
  const std::vector<std::byte> text(limits::clipboard_decoded_bytes_max, std::byte{'x'});
  const std::array contents{vt::ClipboardContent{.mime = "text/plain", .data = text}};
  ASSERT_TRUE(outer.complete_clipboard(pending->id, vt::ClipboardStatus::success, contents));
  deliver(outer, transaction);
  ASSERT_TRUE(transaction.done());
  EXPECT_FALSE(transaction.uncorrelated_read_outstanding());
  EXPECT_EQ(transaction.status(), vt::ClipboardStatus::success);
  ASSERT_EQ(transaction.contents().size(), 1U);
  EXPECT_TRUE(std::ranges::equal(transaction.contents().front().data, text));
}

TEST(ClipboardTest, Osc52WriteCompletesOnlyWhenPublishedAndCanAbortAnIncompleteRecord) {
  auto outer = vt::Terminal::create({}).value();
  outer.set_clipboard_access(true, true);
  const std::vector<std::byte> text(100'003, std::byte{'x'});
  const std::array contents{vt::ClipboardContent{.mime = "text/plain", .data = text}};
  Transaction transaction;
  const auto request =
      transaction
          .begin({.id = 83, .protocol = vt::ClipboardProtocol::osc52, .contents = contents},
                 Transaction::Clock::now())
          .value();
  EXPECT_TRUE(request.starts_with("\x1b]52;c;"));
  write(outer, std::string_view(request).substr(0, 65'536));
  EXPECT_FALSE(outer.clipboard_request());
  std::array<std::byte, 1> abort{};
  ASSERT_EQ(transaction.abort_write(abort), 1U);
  outer.write(abort);
  write(outer, "\x1b[Hrendering continues");
  EXPECT_FALSE(outer.clipboard_request());
  EXPECT_FALSE(transaction.done());
  transaction.fail(vt::ClipboardStatus::denied);
  transaction.published();
  EXPECT_EQ(transaction.status(), vt::ClipboardStatus::denied);

  Transaction next;
  write(outer, next.begin({.protocol = vt::ClipboardProtocol::osc52, .contents = contents},
                          Transaction::Clock::now())
                   .value());
  ASSERT_TRUE(outer.clipboard_request());
  EXPECT_FALSE(next.done());
  next.published();
  EXPECT_TRUE(next.done());
  EXPECT_EQ(next.status(), vt::ClipboardStatus::success);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ClipboardTest, Osc52RejectsBadPaddingSelectionAndTrailingDataWithoutPublishingPartialText) {
  const std::array desired{vt::ClipboardContent{.mime = "text/plain", .data = {}}};
  for (const std::string_view reply :
       {"\x1b]52;p;YQ==\x1b\\", "\x1b]52;c;YR==\x1b\\", "\x1b]52;c;YQ==Yg==\x1b\\",
        "\x1b]52;c;YQ=\x1b\\", "\x1b]52;c;YQ==\x1b\\trailing"}) {
    Transaction transaction;
    ASSERT_TRUE(transaction.begin(
        {.protocol = vt::ClipboardProtocol::osc52, .read = true, .contents = desired},
        Transaction::Clock::now()));
    transaction.published();
    transaction.consume(reply);
    ASSERT_TRUE(transaction.done());
    EXPECT_EQ(transaction.status(), vt::ClipboardStatus::invalid_data);
    EXPECT_TRUE(transaction.contents().empty());
    EXPECT_TRUE(transaction.uncorrelated_read_outstanding());
  }
}

TEST(ClipboardTest, ReadImageRoundTripsThroughTheRealOuterTerminalProtocol) {
  auto outer = vt::Terminal::create({});
  ASSERT_TRUE(outer);
  outer->set_clipboard_access(true, true);
  const std::array desired{vt::ClipboardContent{.mime = "image/png", .data = {}}};
  Transaction transaction;
  const auto request =
      transaction.begin({.id = 47, .read = true, .contents = desired}, Transaction::Clock::now());
  ASSERT_TRUE(request);
  write(*outer, *request);
  const auto pending = outer->clipboard_request();
  ASSERT_TRUE(pending.has_value());
  ASSERT_EQ(pending->contents.size(), 1U);
  EXPECT_EQ(pending->contents.front().mime, "image/png");
  const std::vector<std::byte> image(12'001, std::byte{0xA7});
  const std::array contents{vt::ClipboardContent{.mime = "image/png", .data = image}};
  ASSERT_TRUE(outer->complete_clipboard(pending->id, vt::ClipboardStatus::success, contents));
  deliver(*outer, transaction);
  ASSERT_TRUE(transaction.done());
  EXPECT_EQ(transaction.status(), vt::ClipboardStatus::success);
  EXPECT_EQ(transaction.request_id(), 47U);
  ASSERT_EQ(transaction.contents().size(), 1U);
  EXPECT_TRUE(std::ranges::equal(transaction.contents().front().data, image));
}
TEST(ClipboardTest, ChunkedImageAndTextWriteRoundTripWithoutCachingOrReplay) {
  auto outer = vt::Terminal::create({});
  ASSERT_TRUE(outer);
  outer->set_clipboard_access(true, true);
  const std::vector<std::byte> image(10'003, std::byte{0xFD});
  const std::string text = "image description";
  const std::array contents{
      vt::ClipboardContent{.mime = "image/png", .data = image},
      vt::ClipboardContent{.mime = "text/plain", .data = std::as_bytes(std::span(text))}};
  Transaction transaction;
  const auto request =
      transaction.begin({.id = 4, .contents = contents}, Transaction::Clock::now());
  ASSERT_TRUE(request);
  write(*outer, *request);
  const auto pending = outer->clipboard_request();
  ASSERT_TRUE(pending.has_value());
  ASSERT_EQ(pending->contents.size(), 2U);
  EXPECT_TRUE(std::ranges::equal(pending->contents.front().data, image));
  ASSERT_TRUE(outer->complete_clipboard(pending->id, vt::ClipboardStatus::success));
  deliver(*outer, transaction);
  EXPECT_TRUE(transaction.done());
  EXPECT_EQ(transaction.status(), vt::ClipboardStatus::success);
  EXPECT_FALSE(transaction.begin({}, Transaction::Clock::now()));
}
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ClipboardTest, MaximumWriteInterleavesOnlyWholeRecordsWithRendering) {
  auto outer = vt::Terminal::create({}).value();
  outer.set_clipboard_access(true, true);
  const std::vector<std::byte> image(limits::clipboard_decoded_bytes_max, std::byte{0xAB});
  const std::array contents{vt::ClipboardContent{.mime = "image/png", .data = image}};
  Transaction transaction;
  const auto request =
      transaction.begin({.id = 71, .contents = contents}, Transaction::Clock::now()).value();
  const auto bytes = std::as_bytes(std::span(request));
  std::size_t offset = 0;
  std::size_t frames = 0;
  while (offset < bytes.size()) {
    const auto count = Transaction::frame_prefix(bytes.subspan(offset), 123'456);
    ASSERT_GT(count, 0U);
    ASSERT_LE(count, 64U * 1024U);
    outer.write(bytes.subspan(offset, count));
    offset += count;
    if (offset < bytes.size()) {
      EXPECT_FALSE(outer.clipboard_request());
    }
    write(outer, "\x1b[Hprogress");
    ++frames;
  }
  EXPECT_GT(frames, 16U);
  const auto pending = outer.clipboard_request();
  ASSERT_TRUE(pending.has_value());
  ASSERT_EQ(pending->contents.size(), 1U);
  EXPECT_TRUE(std::ranges::equal(pending->contents.front().data, image));
  ASSERT_TRUE(outer.complete_clipboard(pending->id, vt::ClipboardStatus::success));
  deliver(outer, transaction);
  EXPECT_EQ(transaction.status(), vt::ClipboardStatus::success);
}
TEST(ClipboardTest, CancellationAbortsPartialDataInsteadOfCommittingIt) {
  auto outer = vt::Terminal::create({}).value();
  outer.set_clipboard_access(true, true);
  const std::vector<std::byte> image(100'000, std::byte{0xAB});
  const std::array contents{vt::ClipboardContent{.mime = "image/png", .data = image}};
  Transaction transaction;
  const auto request =
      transaction.begin({.id = 72, .contents = contents}, Transaction::Clock::now()).value();
  const auto bytes = std::as_bytes(std::span(request));
  outer.write(bytes.first(Transaction::frame_prefix(bytes, std::size_t{64} * 1024U)));
  EXPECT_FALSE(outer.clipboard_request());
  std::array<std::byte, 128> abort{};
  const auto count = transaction.abort_write(abort);
  ASSERT_GT(count, 0U);
  outer.write(std::span(abort).first(count));
  EXPECT_FALSE(outer.clipboard_request());
  deliver(outer, transaction);
  EXPECT_TRUE(transaction.done());
  EXPECT_NE(transaction.status(), vt::ClipboardStatus::success);
  // Aborting also releases the outer write owner; an explicit empty value can clear text.
  Transaction next;
  const std::array empty{vt::ClipboardContent{.mime = "text/plain", .data = {}}};
  write(outer, next.begin({.id = 73, .contents = empty}, Transaction::Clock::now()).value());
  const auto pending = outer.clipboard_request();
  ASSERT_TRUE(pending.has_value());
  ASSERT_EQ(pending->contents.size(), 1U);
  EXPECT_TRUE(pending->contents.front().data.empty());
}
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ClipboardTest, MimeListingPreservesTheNativeWholeNameSizeLimit) {
  auto outer = vt::Terminal::create({}).value();
  outer.set_clipboard_access(true, true);
  Transaction transaction;
  write(
      outer,
      transaction.begin({.id = 74, .read = true, .list = true}, Transaction::Clock::now()).value());
  const auto pending = outer.clipboard_request();
  ASSERT_TRUE(pending.has_value());
  std::array<std::string, 32> names;
  std::array<vt::ClipboardContent, 32> contents{};
  for (std::size_t i = 0; i < names.size(); ++i) {
    names.at(i) = "application/x-" + std::string(220, 'x') + std::to_string(i);
    contents.at(i) = {.mime = names.at(i), .data = {}};
  }
  ASSERT_TRUE(outer.complete_clipboard(pending->id, vt::ClipboardStatus::success, contents));
  deliver(outer, transaction);
  ASSERT_TRUE(transaction.done());
  ASSERT_EQ(transaction.status(), vt::ClipboardStatus::success);
  // Ghostty's listing contract is one 4096-byte packet, retaining only whole names.
  ASSERT_EQ(transaction.contents().size(), 17U);
  for (std::size_t i = 0; i < transaction.contents().size(); ++i) {
    EXPECT_EQ(transaction.contents().subspan(i, 1).front().mime, names.at(i));
  }
}
TEST(ClipboardTest, FragmentedListingNeverPublishesPartialMimeNames) {
  Transaction transaction;
  const auto request =
      transaction.begin({.id = 75, .read = true, .list = true}, Transaction::Clock::now()).value();
  const auto id_start = request.find(":id=") + 4U;
  const auto id = request.substr(id_start, request.find(':', id_start) - id_start);
  const auto prefix = "\x1b]5522;type=read:id=" + id + ":status=";
  transaction.consume(prefix + "OK\x1b\\");
  transaction.consume(prefix + "DATA:mime=Lg==;" + base64::encode("image/p") + "\x1b\\");
  transaction.consume(prefix + "DATA:mime=Lg==;" + base64::encode("ng text/plain\n") + "\x1b\\");
  EXPECT_FALSE(transaction.done());
  EXPECT_TRUE(transaction.contents().empty());
  transaction.consume(prefix + "DONE\x1b\\");
  ASSERT_EQ(transaction.status(), vt::ClipboardStatus::success);
  ASSERT_EQ(transaction.contents().size(), 2U);
  EXPECT_EQ(transaction.contents().front().mime, "image/png");
  EXPECT_EQ(transaction.contents().subspan(1, 1).front().mime, "text/plain");
}
TEST(ClipboardTest, UnsolicitedDataCannotCompleteAnOwnedRead) {
  Transaction transaction;
  ASSERT_TRUE(transaction.begin({.read = true}, Transaction::Clock::now()));
  transaction.consume("\x1b]5522;type=read:status=DONE:id=unowned\x1b\\");
  EXPECT_FALSE(transaction.done());
  EXPECT_TRUE(transaction.expired(Transaction::Clock::now() + std::chrono::seconds(31)));
}
TEST(ClipboardTest, CanonicalBase64IsRequired) {
  EXPECT_EQ(base64::decode("AP8=", 2), std::optional<std::string>(std::string("\0\xff", 2)));
  for (const auto* const bad : {"AP9=", "AA=A", "AA==AAAA", "AA", "AA==\n", "AAA?"}) {
    EXPECT_FALSE(base64::decode(bad, 99)) << bad;
  }
  EXPECT_FALSE(base64::decode("AAAA", 2));
}
TEST(ClipboardTest, RepliesInsideBracketedPasteStayOpaque) {
  client::HostInputParser parser;
  ASSERT_TRUE(parser.prepare());
  std::vector<std::byte> output(client::host_input_output_bytes_max);
  const std::string text = "\x1b[200~\x1b]5522;type=read:status=DATA;secret\x1b\\\x1b[201~";
  const auto batch = parser.parse(std::as_bytes(std::span(text)), output, {});
  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->event_count, 1U);
  EXPECT_EQ(batch->events.front().kind, client::HostInputKind::paste);
}
TEST(ClipboardTest, IncompleteReplyFailsClosedInsteadOfBecomingKeyboardInput) {
  client::HostInputParser parser;
  ASSERT_TRUE(parser.prepare());
  std::vector<std::byte> output(client::host_input_output_bytes_max);
  const std::string text = "\x1b]5522;type=read:status=DATA;secret";
  const auto batch = parser.parse(std::as_bytes(std::span(text)), output, {});
  ASSERT_TRUE(batch);
  EXPECT_EQ(batch->event_count, 0U);
  EXPECT_FALSE(parser.flush_pending(output));
}
// NOLINTEND(bugprone-unchecked-optional-access)
} // namespace
} // namespace frame::clipboard
