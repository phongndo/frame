#include "core/connection_output.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace frame::core {
namespace {

struct ScriptedConnectionWriter final {
  std::vector<ConnectionWriteAttempt> attempts;
  std::size_t next{0};
  std::array<std::byte, 1'024> written{};
  std::size_t written_size{0};
};

[[nodiscard]] auto scripted_connection_write(void* const context,
                                             const std::span<const std::byte> bytes) noexcept
    -> ConnectionWriteAttempt {
  auto& script = *static_cast<ScriptedConnectionWriter*>(context);
  ConnectionWriteAttempt result{.bytes = static_cast<std::ptrdiff_t>(bytes.size())};
  if (script.next < script.attempts.size()) {
    result = std::span(script.attempts).subspan(script.next, 1).front();
    ++script.next;
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

TEST(ConnectionOutputTest, RetainsPartialWritesAcrossEagainAndRecovers) {
  ConnectionOutput output;
  constexpr std::string_view message = "slow-control-response";
  ASSERT_TRUE(output.append_text(message));
  ScriptedConnectionWriter script;
  script.attempts = {
      {.bytes = 2}, {.bytes = -1, .error = EINTR}, {.bytes = 3}, {.bytes = -1, .error = EAGAIN}};
  std::size_t budget = 1'024;

  EXPECT_EQ(flush_connection_output(output, budget, &scripted_connection_write, &script),
            ConnectionFlushStatus::blocked);
  EXPECT_EQ(budget, 1'019U);
  EXPECT_EQ(output.readable().size(), message.size() - 5U);

  EXPECT_EQ(flush_connection_output(output, budget, &scripted_connection_write, &script),
            ConnectionFlushStatus::drained);
  EXPECT_FALSE(output.busy());
  const auto expected = std::as_bytes(std::span(message.data(), message.size()));
  EXPECT_TRUE(std::ranges::equal(std::span(script.written).first(script.written_size), expected));
}

} // namespace
} // namespace frame::core
