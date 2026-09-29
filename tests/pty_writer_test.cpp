#include "core/pty_writer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace frame::core {
namespace {

struct ScriptedWriter final {
  std::vector<PtyWriteAttempt> attempts;
  std::size_t next{0};
  std::array<std::byte, pty_write_bytes_per_pane_turn_max + 1'024U> written{};
  std::size_t written_size{0};
  bool complete_after_script{true};
};

[[nodiscard]] auto scripted_write(void* const context,
                                  const std::span<const std::byte> bytes) noexcept
    -> PtyWriteAttempt {
  auto& script = *static_cast<ScriptedWriter*>(context);
  PtyWriteAttempt result{};
  if (script.next < script.attempts.size()) {
    result = std::span(script.attempts).subspan(script.next, 1).front();
    ++script.next;
  } else if (script.complete_after_script) {
    result.bytes = static_cast<std::ptrdiff_t>(bytes.size());
  } else {
    result = {.bytes = -1, .error = EAGAIN};
  }
  if (result.bytes > 0) {
    const auto size = static_cast<std::size_t>(result.bytes);
    if (size <= bytes.size() && size <= script.written.size() - script.written_size) {
      std::ranges::copy(bytes.first(size),
                        std::span(script.written).subspan(script.written_size, size).begin());
      script.written_size += size;
    }
  }
  return result;
}

TEST(PtyWriterTest, ConsumesOnlyPartialWritesAndRecoversAfterEagain) {
  PanePtyWriteQueue queue;
  const std::array input{std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
                         std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};
  ASSERT_TRUE(queue.append(input));
  ScriptedWriter script;
  script.attempts = {
      {.bytes = 2}, {.bytes = -1, .error = EINTR}, {.bytes = 1}, {.bytes = -1, .error = EAGAIN}};
  std::size_t budget = 1'024;

  EXPECT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::blocked);
  EXPECT_EQ(queue.size(), 3U);
  EXPECT_EQ(budget, 1'021U);
  EXPECT_TRUE(std::ranges::equal(std::span(script.written).first(script.written_size),
                                 std::span(input).first(3)));

  EXPECT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::drained);
  EXPECT_TRUE(queue.empty());
  EXPECT_TRUE(std::ranges::equal(std::span(script.written).first(script.written_size), input));
}

TEST(PtyWriterTest, KeepsBlockedTerminalResponsesAheadOfSubsequentInput) {
  auto terminal_result = vt::Terminal::create({});
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  constexpr std::string_view query = "\x1B[5n";
  terminal.write(std::as_bytes(std::span(query.data(), query.size())));

  PanePtyWriteQueue queue;
  ASSERT_TRUE(queue_terminal_responses(queue, terminal));
  ScriptedWriter script;
  script.attempts = {{.bytes = -1, .error = EAGAIN}};
  std::size_t budget = 1'024;
  ASSERT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::blocked);

  constexpr std::string_view user_input = "USER_AFTER_RESPONSE";
  const auto user_bytes = std::as_bytes(std::span(user_input.data(), user_input.size()));
  ASSERT_EQ(queue_normalized_input(queue, terminal, user_bytes), InputQueueResult::queued);
  budget = 1'024;
  ASSERT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::drained);

  constexpr std::string_view expected = "\x1B[0nUSER_AFTER_RESPONSE";
  const auto expected_bytes = std::as_bytes(std::span(expected.data(), expected.size()));
  ASSERT_EQ(script.written_size, expected_bytes.size());
  EXPECT_TRUE(
      std::ranges::equal(std::span(script.written).first(script.written_size), expected_bytes));
}

TEST(PtyWriterTest, RetriesEintrOnlyWithinAttemptBound) {
  PanePtyWriteQueue queue;
  const std::array input{std::byte{'x'}};
  ASSERT_TRUE(queue.append(input));
  ScriptedWriter script;
  script.attempts = std::vector<PtyWriteAttempt>(pty_write_attempts_per_pane_turn_max + 1U,
                                                 {.bytes = -1, .error = EINTR});
  std::size_t budget = 1'024;

  EXPECT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::pending);
  EXPECT_EQ(script.next, pty_write_attempts_per_pane_turn_max);
  EXPECT_EQ(queue.size(), 1U);
  EXPECT_EQ(budget, 1'024U);
}

TEST(PtyWriterTest, EnforcesPerPaneAndGlobalBudgets) {
  PanePtyWriteQueue queue;
  const std::vector<std::byte> input(pty_write_bytes_per_pane_turn_max + 1'024U, std::byte{'q'});
  ASSERT_TRUE(queue.append(input));
  ScriptedWriter script;
  std::size_t budget = std::size_t{1} * 1'024U * 1'024U;

  EXPECT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::pending);
  EXPECT_EQ(script.written_size, pty_write_bytes_per_pane_turn_max);
  EXPECT_EQ(queue.size(), 1'024U);
  EXPECT_EQ(budget, (std::size_t{1} * 1'024U * 1'024U) - pty_write_bytes_per_pane_turn_max);

  std::size_t smaller_budget = 512;
  EXPECT_EQ(flush_pty_write_queue(queue, smaller_budget, &scripted_write, &script),
            PtyFlushStatus::pending);
  EXPECT_EQ(smaller_budget, 0U);
  EXPECT_EQ(queue.size(), 512U);
}

TEST(PtyWriterTest, ReportsHardErrorsWithoutConsumingUnwrittenSuffix) {
  PanePtyWriteQueue queue;
  const std::array input{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  ASSERT_TRUE(queue.append(input));
  ScriptedWriter script;
  script.attempts = {{.bytes = 1}, {.bytes = -1, .error = EIO}};
  std::size_t budget = 1'024;

  EXPECT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::hard_error);
  ASSERT_EQ(queue.size(), 2U);
  EXPECT_EQ(queue.readable_span().front(), std::byte{'b'});
  EXPECT_EQ(budget, 1'023U);
}

TEST(PtyWriterTest, ReusesDrainedCapacityAndReleasesItAtOwnerDestruction) {
  const auto baseline = PanePtyWriteQueue::allocated_bytes_current();
  {
    PanePtyWriteQueue queue;
    const std::vector<std::byte> input(std::size_t{8} * 1'024U, std::byte{'q'});
    ASSERT_TRUE(queue.append(input));
    const auto allocated = PanePtyWriteQueue::allocated_bytes_current();
    EXPECT_GT(allocated, baseline);
    std::size_t budget = input.size();
    ScriptedWriter script;
    EXPECT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
              PtyFlushStatus::drained);
    EXPECT_EQ(PanePtyWriteQueue::allocated_bytes_current(), allocated);

    ASSERT_TRUE(queue.append(input));
    EXPECT_EQ(PanePtyWriteQueue::allocated_bytes_current(), allocated);
  }
  EXPECT_EQ(PanePtyWriteQueue::allocated_bytes_current(), baseline);
}

TEST(PtyWriterTest, QueuesInBandSizeReportsGeneratedByResize) {
  vt::TerminalOptions options;
  options.size = {.columns = 80, .rows = 24, .cell_width_px = 8, .cell_height_px = 16};
  auto terminal_result = vt::Terminal::create(options);
  ASSERT_TRUE(terminal_result.has_value());
  auto terminal = std::move(*terminal_result);
  constexpr std::string_view enable = "\x1B[?2048h";
  terminal.write(std::as_bytes(std::span(enable.data(), enable.size())));

  PanePtyWriteQueue queue;
  ASSERT_TRUE(queue_terminal_responses(queue, terminal));
  ScriptedWriter script;
  std::size_t budget = 1'024;
  ASSERT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::drained);

  const vt::TerminalSize resized{
      .columns = 40,
      .rows = 12,
      .cell_width_px = 8,
      .cell_height_px = 16,
  };
  ASSERT_TRUE(terminal.resize(resized).has_value());
  ASSERT_TRUE(queue_terminal_responses(queue, terminal));
  budget = 1'024;
  ASSERT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::drained);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view written(reinterpret_cast<const char*>(script.written.data()),
                                 script.written_size);
  EXPECT_NE(written.find("\x1B[48;12;40;192;320t"), std::string_view::npos);
}

TEST(PtyWriterTest, RejectsInvalidWriterProgress) {
  PanePtyWriteQueue queue;
  const std::array input{std::byte{'a'}, std::byte{'b'}};
  ASSERT_TRUE(queue.append(input));
  ScriptedWriter script;
  script.attempts = {{.bytes = 3}};
  std::size_t budget = 1'024;

  EXPECT_EQ(flush_pty_write_queue(queue, budget, &scripted_write, &script),
            PtyFlushStatus::hard_error);
  EXPECT_EQ(queue.size(), input.size());
  EXPECT_EQ(budget, 1'024U);
}

} // namespace
} // namespace frame::core
