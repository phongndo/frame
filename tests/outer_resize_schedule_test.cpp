#include "client/outer_resize_schedule.hpp"

#include <chrono>
#include <cstddef>
#include <optional>

#include <gtest/gtest.h>

namespace lemma::client {
namespace {

using namespace std::chrono_literals;

constexpr auto origin = OuterResizeSchedule::TimePoint{} + 1s;
constexpr auto interval = OuterResizeSchedule::commit_interval;

TEST(OuterResizeScheduleTest, IsolatedResizeCommitsWithoutQuietDelay) {
  OuterResizeSchedule schedule;
  EXPECT_FALSE(schedule.pending());
  EXPECT_EQ(schedule.deadline(), std::nullopt);
  EXPECT_FALSE(schedule.due(origin));

  schedule.observe(origin);
  EXPECT_TRUE(schedule.pending());
  EXPECT_EQ(schedule.deadline(), std::optional{origin});
  EXPECT_TRUE(schedule.due(origin));
  schedule.commit(origin, true);
  EXPECT_FALSE(schedule.pending());

  // A later isolated resize is again immediate once the previous commit's interval has elapsed.
  schedule.observe(origin + interval);
  EXPECT_TRUE(schedule.due(origin + interval));
}

TEST(OuterResizeScheduleTest, ObservationsWithinIntervalCoalesceWithoutPostponement) {
  OuterResizeSchedule schedule;
  schedule.observe(origin);
  schedule.commit(origin, true);

  schedule.observe(origin + 1ms);
  EXPECT_EQ(schedule.deadline(), std::optional{origin + interval});
  EXPECT_FALSE(schedule.due(origin + interval - 1us));
  // Later samples join the pending commit rather than extending it.
  schedule.observe(origin + interval - 1ms);
  EXPECT_EQ(schedule.deadline(), std::optional{origin + interval});
  EXPECT_TRUE(schedule.due(origin + interval));
}

TEST(OuterResizeScheduleTest, UnchangedGeometryDoesNotConsumeInterval) {
  OuterResizeSchedule schedule;
  schedule.observe(origin);
  schedule.commit(origin, true);
  // A SIGWINCH whose settled size matches the last sent geometry sends nothing.
  schedule.observe(origin + interval);
  schedule.commit(origin + interval, false);
  schedule.observe(origin + interval + 1ms);
  EXPECT_TRUE(schedule.due(origin + interval + 1ms));
}

TEST(OuterResizeScheduleTest, ForcedCommitBeforeInputRestartsInterval) {
  OuterResizeSchedule schedule;
  schedule.observe(origin);
  schedule.commit(origin, true);
  schedule.observe(origin + 2ms);
  ASSERT_FALSE(schedule.due(origin + 3ms));
  // Input ordering sends pending geometry early; the next sample is paced from that send.
  schedule.commit(origin + 3ms, true);
  schedule.observe(origin + 4ms);
  EXPECT_EQ(schedule.deadline(), std::optional{origin + 3ms + interval});
}

struct DragResult final {
  std::size_t commits{0};
  OuterResizeSchedule::TimePoint last_observation;
  std::optional<OuterResizeSchedule::TimePoint> settle;
};

// A reactor that services every deadline exactly, driven by one SIGWINCH per step.
[[nodiscard]] auto simulate_drag(const OuterResizeSchedule::Clock::duration drag,
                                 const OuterResizeSchedule::Clock::duration step) -> DragResult {
  OuterResizeSchedule schedule;
  DragResult result;
  const auto service = [&](const OuterResizeSchedule::TimePoint at) {
    if (schedule.due(at)) {
      schedule.commit(at, true);
      ++result.commits;
    }
  };
  for (auto now = origin; now <= origin + drag; now += step) {
    service(now);
    schedule.observe(now);
    service(now);
    result.last_observation = now;
  }
  result.settle = schedule.deadline();
  if (result.settle.has_value()) {
    service(*result.settle);
  }
  return schedule.pending() ? DragResult{} : result;
}

TEST(OuterResizeScheduleTest, DragSendsBoundedCommitsAndSettlesPromptly) {
  constexpr auto drag = 500ms;
  const auto result = simulate_drag(drag, 1ms);
  ASSERT_TRUE(result.settle.has_value());
  EXPECT_LE(result.settle.value_or(origin) - result.last_observation, interval);
  EXPECT_GE(result.commits, 2U);
  EXPECT_LE(result.commits, static_cast<std::size_t>(drag / interval) + 2U);
}

} // namespace
} // namespace lemma::client
