#include "platform/readiness.hpp"

#include "platform/io.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <memory>
#include <span>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace frame::platform {
namespace {

struct ReadinessTest : ::testing::Test {
  void SetUp() override {
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, first.data()), 0);
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, second.data()), 0);
    descriptors = {{{.fd = first.front(), .events = POLLIN, .revents = 0},
                    {.fd = second.front(), .events = POLLIN, .revents = 0}}};
  }
  void TearDown() override {
    for (auto& fd : first) {
      close_descriptor(fd);
    }
    for (auto& fd : second) {
      close_descriptor(fd);
    }
    close_descriptor(duplicate);
  }
  static void signal(const int fd) { ASSERT_EQ(::write(fd, "x", 1), 1); }
  static void consume(const int fd) {
    char value = 0;
    ASSERT_EQ(::read(fd, &value, 1), 1);
    EXPECT_EQ(value, 'x');
  }
  [[nodiscard]] auto wait() -> int { return readiness.wait(descriptors, identities, 0); }
  void expect_matches_poll() {
    auto expected = descriptors;
    const auto count = ::poll(expected.data(), static_cast<nfds_t>(expected.size()), 0);
    ASSERT_GE(count, 0);
    EXPECT_EQ(wait(), count);
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      EXPECT_EQ(descriptors.at(index).revents, expected.at(index).revents);
    }
  }

  Readiness readiness{8};
  std::array<int, 2> first{-1, -1};
  std::array<int, 2> second{-1, -1};
  int duplicate{-1};
  std::array<pollfd, 2> descriptors{};
  std::array<ReadinessIdentity, 2> identities{
      {{.domain = 1, .owner = 0, .generation = 1}, {.domain = 1, .owner = 1, .generation = 1}}};
};

TEST_F(ReadinessTest, LevelTriggeredUntilConsumed) {
  EXPECT_EQ(wait(), 0);
  signal(first.back());
  EXPECT_EQ(wait(), 1);
  EXPECT_EQ(descriptors.front().revents, POLLIN);
  EXPECT_EQ(wait(), 1);
  consume(first.front());
  EXPECT_EQ(wait(), 0);
  EXPECT_EQ(descriptors.front().revents, 0);
}

TEST_F(ReadinessTest, InterestChangesAndReorderingUseCurrentSlots) {
  descriptors.front().events = POLLOUT;
  EXPECT_EQ(wait(), 1);
  EXPECT_EQ(descriptors.front().revents, POLLOUT);
  descriptors.front().events = POLLIN;
  EXPECT_EQ(wait(), 0);
  std::swap(descriptors.front(), descriptors.back());
  std::swap(identities.front(), identities.back());
  signal(first.back());
  EXPECT_EQ(wait(), 1);
  EXPECT_EQ(descriptors.front().revents, 0);
  EXPECT_EQ(descriptors.back().revents, POLLIN);
}

TEST_F(ReadinessTest, RemovedWatchDoesNotReportAndCanBeReadmitted) {
  EXPECT_EQ(wait(), 0);
  descriptors.front().fd = -1;
  signal(first.back());
  EXPECT_EQ(wait(), 0);
  descriptors.front().fd = first.front();
  EXPECT_EQ(wait(), 1);
  EXPECT_EQ(descriptors.front().revents, POLLIN);
}

// The old open file remains alive through dup after its original number is reused.
TEST_F(ReadinessTest, ReusedNumberDoesNotInheritAnOldOpenFileWatch) {
  EXPECT_EQ(wait(), 0);
  duplicate = ::dup(first.front());
  ASSERT_GE(duplicate, 0);
  ASSERT_EQ(::dup2(second.front(), first.front()), first.front());
  identities.front().generation = 2;
  signal(first.back());
  EXPECT_EQ(wait(), 0);
  signal(second.back());
  EXPECT_EQ(wait(), 2);
  EXPECT_EQ(descriptors.front().revents, POLLIN);
  consume(first.front());
  EXPECT_EQ(wait(), 0);
}

TEST_F(ReadinessTest, HangupWithoutReadInterestMatchesPoll) {
  descriptors.front().events = 0;
  EXPECT_EQ(wait(), 0);
  close_descriptor(first.back());
  // Native poll differs across platforms when no read interest is registered.
  expect_matches_poll();
}

TEST_F(ReadinessTest, InvalidDescriptorFallsBackToPoll) {
  EXPECT_EQ(wait(), 0);
  close_descriptor(first.front());
  identities.front().generation = 2;
  EXPECT_EQ(wait(), 1);
  EXPECT_EQ(descriptors.front().revents, POLLNVAL);
}

TEST_F(ReadinessTest, DuplicateTransitionPreservesPollCountAndInterests) {
  EXPECT_EQ(wait(), 0);
  descriptors.back().fd = first.front();
  identities.back() = identities.front();
  signal(first.back());
  // Darwin reports only one ready entry for a duplicate fd; Linux reports both.
  expect_matches_poll();
  descriptors.back().events = 0;
  expect_matches_poll();
}

TEST_F(ReadinessTest, UncachedLifetimeAndUnsupportedInterestPreservePollBehavior) {
  EXPECT_EQ(wait(), 0);
  identities.front() = {};
  signal(first.back());
  EXPECT_EQ(wait(), 1);
  consume(first.front());
  identities.front() = {.domain = 1, .owner = 0, .generation = 1};
  descriptors.front().events = POLLRDNORM;
  signal(first.back());
  EXPECT_EQ(wait(), 1);
  EXPECT_NE(descriptors.front().revents & POLLRDNORM, 0);
}

TEST_F(ReadinessTest, RegularFilesFallBackToPoll) {
  const std::unique_ptr<std::FILE, decltype(&std::fclose)> file(std::tmpfile(), &std::fclose);
  ASSERT_NE(file, nullptr);
  descriptors.front().fd = ::fileno(file.get());
  EXPECT_EQ(wait(), 1);
  EXPECT_EQ(descriptors.front().revents, POLLIN);
}

// GoogleTest assertion macros inflate the branch count in this linear signal-lifetime test.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(ReadinessTest, InterruptedWaitReturnsEintr) {
  struct sigaction previous{};
  struct sigaction action{};
  action.sa_handler = +[](int) {};
  // macOS exposes sigemptyset as a macro.
  static_cast<void>(sigemptyset(&action.sa_mask));
  ASSERT_EQ(::sigaction(SIGALRM, &action, &previous), 0);
  static_cast<void>(::alarm(1));
  const auto result = readiness.wait(descriptors, identities, 2000);
  const int error = errno;
  static_cast<void>(::alarm(0));
  EXPECT_EQ(::sigaction(SIGALRM, &previous, nullptr), 0);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(error, EINTR);
}

TEST_F(ReadinessTest, EmptyAndOversizedSetsUsePollSemantics) {
  EXPECT_EQ(readiness.wait({}, {}, 0), 0);
  Readiness small{1};
  signal(first.back());
  EXPECT_EQ(small.wait(descriptors, identities, 0), 1);
}

} // namespace
} // namespace frame::platform
