#include "core/command_history.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

#include <unistd.h>

namespace frame::core {
namespace {

class TemporaryHistoryPath final {
public:
  TemporaryHistoryPath() : path_("/tmp/frame-command-history-test-XXXXXX") {
    const auto descriptor = ::mkstemp(path_.data());
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      static_cast<void>(::unlink(path_.c_str()));
    } else {
      path_.clear();
    }
  }

  TemporaryHistoryPath(const TemporaryHistoryPath&) = delete;
  TemporaryHistoryPath(TemporaryHistoryPath&&) = delete;
  auto operator=(const TemporaryHistoryPath&) -> TemporaryHistoryPath& = delete;
  auto operator=(TemporaryHistoryPath&&) -> TemporaryHistoryPath& = delete;
  ~TemporaryHistoryPath() {
    if (!path_.empty()) {
      static_cast<void>(::unlink(path_.c_str()));
    }
  }

  [[nodiscard]] auto path() const noexcept -> std::string_view { return path_; }

private:
  std::string path_;
};

// GTest assertion macros inflate the apparent branch count in this bounded container test.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(CommandHistoryTest, RetainsDistinctNewestCommandsWithinTheFixedLimit) {
  CommandLineHistory history;
  remember_command_line(history, "pane split --right");
  remember_command_line(history, "pane split --right");
  remember_command_line(history, "tab new");

  ASSERT_EQ(history.size, 2U);
  EXPECT_EQ(history.entries.front().view(), "tab new");
  EXPECT_EQ(std::span(history.entries).subspan(1U, 1U).front().view(), "pane split --right");

  for (std::size_t index = 0; index < limits::command_line_history_max + 4U; ++index) {
    const auto command = "switch session-" + std::to_string(index);
    remember_command_line(history, command);
  }
  ASSERT_EQ(history.size, limits::command_line_history_max);
  EXPECT_EQ(history.entries.front().view(), "switch session-19");
  EXPECT_EQ(history.entries.back().view(), "switch session-4");
}

TEST(CommandHistoryTest, RoundTripsTheBoundedHistoryFile) {
  TemporaryHistoryPath file;
  ASSERT_FALSE(file.path().empty());
  CommandLineHistory history;
  remember_command_line(history, "pane split --right");
  remember_command_line(history, "tab new --title tests");

  ASSERT_TRUE(save_command_line_history(file.path(), history));
  const auto loaded = load_command_line_history(file.path());

  EXPECT_TRUE(loaded.replace_on_shutdown);
  ASSERT_EQ(loaded.history.size, 2U);
  EXPECT_EQ(loaded.history.entries.front().view(), "tab new --title tests");
  EXPECT_EQ(std::span(loaded.history.entries).subspan(1U, 1U).front().view(), "pane split --right");
}

TEST(CommandHistoryTest, ReplacesMissingOrValidPersistenceButPreservesMalformedFiles) {
  TemporaryHistoryPath file;
  ASSERT_FALSE(file.path().empty());
  const auto missing = load_command_line_history(file.path());
  EXPECT_EQ(missing.history.size, 0U);
  EXPECT_TRUE(missing.replace_on_shutdown);

  std::ofstream output(std::string(file.path()), std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output << "not-frame-history\ncommand\n";
  output.close();
  ASSERT_TRUE(output.good());
  const auto malformed = load_command_line_history(file.path());
  EXPECT_EQ(malformed.history.size, 0U);
  EXPECT_FALSE(malformed.replace_on_shutdown);
}

} // namespace
} // namespace frame::core
