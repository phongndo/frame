#include "core/frame_scheduler.hpp"
#include "lemma/id.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace lemma::core {
namespace {

using namespace std::chrono_literals;

constexpr auto origin = FrameScheduler::TimePoint{};

TEST(FrameSchedulerTest, ClassifiesInputAndArmsOnlyAfterItsPtyWriteProgress) {
  EXPECT_FALSE(latency_sensitive_input(0));
  EXPECT_TRUE(latency_sensitive_input(interactive_input_bytes_max));
  EXPECT_FALSE(latency_sensitive_input(interactive_input_bytes_max + 1U));

  InteractiveDamageLatch latch;
  latch.await_write(3, 4);
  EXPECT_TRUE(latch.waiting_for_write());
  EXPECT_FALSE(latch.pending());
  EXPECT_FALSE(latch.consume());

  EXPECT_FALSE(latch.record_write(3));
  EXPECT_TRUE(latch.waiting_for_write());
  EXPECT_FALSE(latch.pending());
  EXPECT_FALSE(latch.consume());

  EXPECT_TRUE(latch.record_write(1));
  EXPECT_FALSE(latch.waiting_for_write());
  EXPECT_TRUE(latch.pending());
  EXPECT_TRUE(latch.consume());
  EXPECT_FALSE(latch.pending());
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(FrameSchedulerTest, OutputQueuedBeforeInputArmsCannotAnswerIt) {
  InteractiveDamageLatch latch;
  latch.await_write(0, 1);
  ASSERT_TRUE(latch.record_write(1));
  latch.record_output_backlog(81);

  // The backlog drains in pieces without answering the input; the latch stays armed for the
  // response behind it.
  EXPECT_FALSE(latch.record_output(50));
  EXPECT_FALSE(latch.record_output(31));
  EXPECT_TRUE(latch.pending());
  EXPECT_TRUE(latch.record_output(30));
  EXPECT_TRUE(latch.consume());

  // A read that reaches past the backlog carries output written after the input.
  latch.await_write(0, 1);
  ASSERT_TRUE(latch.record_write(1));
  latch.record_output_backlog(81);
  EXPECT_TRUE(latch.record_output(111));

  // Consuming or resetting clears the backlog; later output answers the next input directly.
  latch.record_output_backlog(81);
  EXPECT_TRUE(latch.consume());
  latch.await_write(0, 1);
  ASSERT_TRUE(latch.record_write(1));
  EXPECT_TRUE(latch.record_output(1));

  // A backlog is recorded only for an armed latch.
  latch.reset();
  latch.record_output_backlog(81);
  EXPECT_TRUE(latch.record_output(1));
}

TEST(FrameSchedulerTest, HigherUrgencyAdvancesButLaterRequestsNeverPostponeDeadline) {
  FrameScheduler scheduler;
  scheduler.request(FrameUrgency::burst, false, origin, FrameSinkState::ready);
  ASSERT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 2ms);

  scheduler.request(FrameUrgency::burst, false, origin + 1ms, FrameSinkState::ready);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 2ms);

  scheduler.request(FrameUrgency::interactive, false, origin + 1500us, FrameSinkState::ready);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 1500us);
  EXPECT_EQ(scheduler.urgency(), FrameUrgency::interactive);

  scheduler.request(FrameUrgency::state_change, false, origin + 1750us, FrameSinkState::ready);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 1500us);
  EXPECT_EQ(scheduler.urgency(), FrameUrgency::interactive);
}

TEST(FrameSchedulerTest, BurstContinuationGetsOneBoundedCoalescingDeadlinePerFrame) {
  FrameScheduler scheduler;
  scheduler.request(FrameUrgency::burst, false, origin, FrameSinkState::ready);
  EXPECT_FALSE(scheduler.due(origin + 1999us, FrameSinkState::ready));
  EXPECT_TRUE(scheduler.due(origin + 2ms, FrameSinkState::ready));

  scheduler.complete();
  EXPECT_FALSE(scheduler.pending());
  EXPECT_FALSE(scheduler.deadline(FrameSinkState::ready).has_value());

  scheduler.request(FrameUrgency::burst, false, origin + 3ms, FrameSinkState::ready);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 5ms);
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(FrameSchedulerTest, SustainedBurstUsesDisplayCadenceWithoutDelayingShortBursts) {
  FrameScheduler scheduler;
  for (auto elapsed : {0ms, 9ms, 18ms, 27ms, 36ms, 45ms}) {
    scheduler.request(FrameUrgency::burst, false, origin + elapsed, FrameSinkState::ready);
    EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + elapsed + 2ms);
    scheduler.complete();
  }

  scheduler.request(FrameUrgency::burst, false, origin + 54ms, FrameSinkState::ready);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 70ms);
  scheduler.request(FrameUrgency::interactive, false, origin + 55ms, FrameSinkState::ready);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 55ms);
  scheduler.complete();

  // A gap outside the continuity window starts a fresh low-latency burst.
  scheduler.request(FrameUrgency::burst, false, origin + 65ms, FrameSinkState::ready);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 67ms);
}

TEST(FrameSchedulerTest, BlockedOutputRetainsOnePendingRequestWithoutADeadlineWakeup) {
  FrameScheduler scheduler;
  scheduler.request(FrameUrgency::interactive, false, origin, FrameSinkState::blocked);

  EXPECT_TRUE(scheduler.pending());
  EXPECT_TRUE(scheduler.force_full());
  EXPECT_FALSE(scheduler.deadline(FrameSinkState::blocked).has_value());
  EXPECT_FALSE(scheduler.due(origin + 1s, FrameSinkState::blocked));
  EXPECT_TRUE(scheduler.due(origin + 1s, FrameSinkState::ready));
}

TEST(FrameSchedulerTest, ResizePromotesBurstAndRetainsFullRedrawRequirement) {
  FrameScheduler scheduler;
  scheduler.request(FrameUrgency::burst, false, origin, FrameSinkState::ready);
  scheduler.request(FrameUrgency::state_change, true, origin + 500us, FrameSinkState::ready);

  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 500us);
  EXPECT_EQ(scheduler.urgency(), FrameUrgency::state_change);
  EXPECT_TRUE(scheduler.force_full());
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(FrameSchedulerTest, DetachCancelsPendingDeadlineFullRedrawAndBurstHistory) {
  FrameScheduler scheduler;
  scheduler.request(FrameUrgency::state_change, true, origin, FrameSinkState::ready);
  scheduler.cancel();

  EXPECT_FALSE(scheduler.pending());
  EXPECT_FALSE(scheduler.force_full());
  EXPECT_FALSE(scheduler.deadline(FrameSinkState::ready).has_value());

  for (auto elapsed : {0ms, 9ms, 18ms, 27ms, 36ms, 45ms}) {
    scheduler.request(FrameUrgency::burst, false, origin + elapsed, FrameSinkState::ready);
    scheduler.complete();
  }
  scheduler.request(FrameUrgency::burst, false, origin + 54ms, FrameSinkState::ready);
  ASSERT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 70ms);

  scheduler.cancel();
  scheduler.request(FrameUrgency::burst, false, origin + 55ms, FrameSinkState::ready);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 57ms);
}

void prepare_sustained_burst(FrameScheduler& scheduler) {
  for (auto elapsed : {0ms, 9ms, 18ms, 27ms, 36ms, 45ms}) {
    scheduler.request(FrameUrgency::burst, false, origin + elapsed, FrameSinkState::ready);
    scheduler.complete();
  }
  scheduler.request(FrameUrgency::burst, false, origin + 54ms, FrameSinkState::ready);
}

// The real call sequence can spend the input latch on unrelated PTY output before its response.
TEST(FrameSchedulerTest, BackgroundDamageCannotDelayFollowingInputResponse) {
  const auto source = PaneId::from_parts(0, 1);
  FrameScheduler scheduler;
  prepare_sustained_burst(scheduler);
  InteractiveDamageLatch latch;
  latch.await_write(0, 1);
  ASSERT_TRUE(latch.record_write(1));
  ASSERT_TRUE(latch.consume());
  const auto first_damage = origin + 55ms;
  scheduler.request(FrameUrgency::interactive, false, first_damage, FrameSinkState::ready, source);
  ASSERT_TRUE(scheduler.due(first_damage, FrameSinkState::ready));
  scheduler.complete();

  ASSERT_FALSE(latch.consume());
  scheduler.request(FrameUrgency::burst, false, first_damage + 40us, FrameSinkState::ready, source);
  const auto followup = first_damage + 40us;
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), followup);
  EXPECT_TRUE(scheduler.due(followup, FrameSinkState::ready));
  scheduler.complete();

  // The single immediate follow-up does not restart the sustained stream's history or create an
  // idle timer.
  EXPECT_FALSE(scheduler.deadline(FrameSinkState::ready).has_value());
  scheduler.request(FrameUrgency::burst, false, followup, FrameSinkState::ready, source);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready),
            followup + FrameScheduler::sustained_burst_delay);
}

TEST(FrameSchedulerTest, InteractiveFollowupCoalescesDamageWithoutWakingBlockedOutput) {
  const auto source = PaneId::from_parts(0, 1);
  FrameScheduler scheduler;
  prepare_sustained_burst(scheduler);
  scheduler.request(FrameUrgency::interactive, false, origin + 55ms, FrameSinkState::ready, source);
  scheduler.complete();
  scheduler.request(FrameUrgency::burst, false, origin + 55040us, FrameSinkState::blocked, source);
  scheduler.request(FrameUrgency::burst, false, origin + 56ms, FrameSinkState::blocked, source);
  EXPECT_FALSE(scheduler.deadline(FrameSinkState::blocked).has_value());
  EXPECT_TRUE(scheduler.force_full());
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 55040us);
  EXPECT_TRUE(scheduler.due(origin + 58ms, FrameSinkState::ready));
}

TEST(FrameSchedulerTest, OutputAfterInteractiveRecoveryWindowKeepsDisplayCadence) {
  const auto source = PaneId::from_parts(0, 1);
  FrameScheduler scheduler;
  prepare_sustained_burst(scheduler);
  scheduler.request(FrameUrgency::interactive, false, origin + 55ms, FrameSinkState::ready, source);
  scheduler.complete();

  // A continuing stream must not get another short frame merely because input was just handled.
  scheduler.request(FrameUrgency::burst, false, origin + 56100us, FrameSinkState::ready, source);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 72100us);
}

TEST(FrameSchedulerTest, ExpiredAndCancelledInteractiveFollowupsDoNotAccelerateLaterOutput) {
  const auto source = PaneId::from_parts(0, 1);
  FrameScheduler scheduler;
  prepare_sustained_burst(scheduler);
  scheduler.request(FrameUrgency::interactive, false, origin + 55ms, FrameSinkState::ready, source);
  scheduler.complete();
  EXPECT_FALSE(scheduler.deadline(FrameSinkState::ready).has_value());
  scheduler.request(FrameUrgency::burst, false, origin + 58ms, FrameSinkState::ready, source);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 74ms);

  scheduler.request(FrameUrgency::interactive, false, origin + 59ms, FrameSinkState::ready, source);
  scheduler.complete();
  scheduler.cancel();
  scheduler.request(FrameUrgency::burst, false, origin + 59500us, FrameSinkState::ready, source);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 61500us);
}

TEST(FrameSchedulerTest, InteractiveRecoveryDoesNotAccelerateSiblingOrReplacementPane) {
  const auto source = PaneId::from_parts(0, 1);
  for (const auto other : {PaneId::from_parts(1, 1), PaneId::from_parts(0, 2)}) {
    FrameScheduler scheduler;
    prepare_sustained_burst(scheduler);
    scheduler.request(FrameUrgency::interactive, false, origin + 55ms, FrameSinkState::ready,
                      source);
    scheduler.complete();
    scheduler.request(FrameUrgency::burst, false, origin + 55040us, FrameSinkState::ready, other);
    EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 71040us);

    // Unrelated damage must not spend the source Pane's recovery opportunity either.
    scheduler.request(FrameUrgency::burst, false, origin + 55080us, FrameSinkState::ready, source);
    EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 55080us);
  }
}

TEST(FrameSchedulerTest, UnscopedInteractionDoesNotOpenPaneRecoveryWindow) {
  FrameScheduler scheduler;
  prepare_sustained_burst(scheduler);
  scheduler.request(FrameUrgency::interactive, false, origin + 55ms, FrameSinkState::ready);
  scheduler.complete();
  scheduler.request(FrameUrgency::burst, false, origin + 55040us, FrameSinkState::ready,
                    PaneId::from_parts(0, 1));
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 71040us);
}

TEST(FrameSchedulerTest, NoClientDoesNotCreatePendingWorkOrAnIdleTimer) {
  const auto source = PaneId::from_parts(0, 1);
  FrameScheduler scheduler;
  scheduler.request(FrameUrgency::interactive, true, origin, FrameSinkState::unavailable, source);

  EXPECT_FALSE(scheduler.pending());
  EXPECT_FALSE(scheduler.deadline(FrameSinkState::ready).has_value());
  scheduler.request(FrameUrgency::burst, false, origin + 40us, FrameSinkState::ready, source);
  EXPECT_EQ(scheduler.deadline(FrameSinkState::ready), origin + 2040us);
}

} // namespace
} // namespace lemma::core
