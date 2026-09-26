#include "core/frame_scheduler.hpp"
#include "lemma/id.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace lemma::core {

[[nodiscard]] auto latency_sensitive_input(const std::size_t bytes) noexcept -> bool {
  return bytes > 0 && bytes <= interactive_input_bytes_max;
}

void InteractiveDamageLatch::await_write(const std::size_t queued_bytes_before,
                                         const std::size_t queued_bytes_after) noexcept {
  if (bytes_until_armed_ == 0 && queued_bytes_after > queued_bytes_before) {
    bytes_until_armed_ = queued_bytes_before + 1U;
  }
}

void InteractiveDamageLatch::prepare_write(const std::size_t bytes,
                                           const PtyReadableOutput readable,
                                           void* const context) noexcept {
  // A pending latch keeps its backlog: output readable now may already include the response to
  // the input that armed it.
  const bool arms_idle_latch = !pending_ && bytes_until_armed_ > 0 && bytes >= bytes_until_armed_;
  sampled_output_ = arms_idle_latch ? readable(context) : 0;
}

void InteractiveDamageLatch::record_write(const std::size_t bytes) noexcept {
  const auto sampled = sampled_output_;
  sampled_output_ = 0;
  if (bytes_until_armed_ == 0) {
    return;
  }
  if (bytes < bytes_until_armed_) {
    bytes_until_armed_ -= bytes;
    return;
  }
  bytes_until_armed_ = 0;
  if (!pending_) {
    output_backlog_ = sampled;
    pending_ = true;
  }
}

[[nodiscard]] auto InteractiveDamageLatch::take_response(const std::size_t drained_bytes,
                                                         const bool visible_damage) noexcept
    -> bool {
  if (!pending_) {
    return false;
  }
  if (output_backlog_ > 0) {
    const auto backlog = std::min(drained_bytes, output_backlog_);
    output_backlog_ -= backlog;
    if (drained_bytes == backlog) {
      return false;
    }
  }
  if (!visible_damage) {
    return false;
  }
  pending_ = false;
  return true;
}

void InteractiveDamageLatch::reset() noexcept {
  bytes_until_armed_ = 0;
  sampled_output_ = 0;
  output_backlog_ = 0;
  pending_ = false;
}

void FrameScheduler::request(const FrameUrgency urgency, const bool force_full, const TimePoint now,
                             const FrameSinkState sink, const PaneId source) noexcept {
  if (sink == FrameSinkState::unavailable) {
    return;
  }
  auto candidate = urgency == FrameUrgency::burst ? burst_deadline(now) : now;
  if (urgency == FrameUrgency::interactive && source.is_valid()) {
    // The input latch can be consumed by unrelated output already in this PTY. Allow one
    // immediate following response without accelerating sibling Panes or the ongoing stream.
    // Keep burst history, and do not arm a timer unless more damage actually arrives.
    interactive_followup_ =
        InteractiveFollowup{.source = source, .deadline = now + interactive_followup_window};
  } else if (urgency == FrameUrgency::burst && interactive_followup_.has_value()) {
    if (now > interactive_followup_->deadline) {
      interactive_followup_.reset();
    } else if (source == interactive_followup_->source) {
      candidate = now;
      interactive_followup_.reset();
    }
  }
  if (!pending_ || candidate < deadline_) {
    deadline_ = candidate;
  }
  if (!pending_ || urgency > urgency_) {
    urgency_ = urgency;
  }
  pending_ = true;
  // Composition has already advanced retained physical state for the in-flight frame. Canonical
  // damage that arrives behind it collapses into one complete repair frame after it drains.
  force_full_ = force_full_ || force_full || sink == FrameSinkState::blocked;
}

[[nodiscard]] auto FrameScheduler::deadline(const FrameSinkState sink) const noexcept
    -> std::optional<TimePoint> {
  if (!pending_ || sink != FrameSinkState::ready) {
    return std::nullopt;
  }
  return deadline_;
}

[[nodiscard]] auto FrameScheduler::due(const TimePoint now,
                                       const FrameSinkState sink) const noexcept -> bool {
  const auto pending_deadline = deadline(sink);
  return pending_deadline.has_value() && now >= *pending_deadline;
}

[[nodiscard]] auto FrameScheduler::pending() const noexcept -> bool { return pending_; }

[[nodiscard]] auto FrameScheduler::force_full() const noexcept -> bool { return force_full_; }

[[nodiscard]] auto FrameScheduler::urgency() const noexcept -> FrameUrgency { return urgency_; }

void FrameScheduler::complete() noexcept { clear_pending(); }

void FrameScheduler::cancel() noexcept { reset(); }

[[nodiscard]] auto FrameScheduler::burst_deadline(const TimePoint now) noexcept -> TimePoint {
  if (!tracking_burst_ || now - last_burst_request_at_ > burst_continuity_window) {
    burst_started_at_ = now;
    tracking_burst_ = true;
  }
  last_burst_request_at_ = now;
  const bool sustained = now - burst_started_at_ >= sustained_burst_threshold;
  return now + (sustained ? sustained_burst_delay : burst_delay);
}

void FrameScheduler::clear_pending() noexcept {
  pending_ = false;
  force_full_ = false;
  urgency_ = FrameUrgency::burst;
  deadline_ = {};
}

void FrameScheduler::reset() noexcept {
  clear_pending();
  burst_started_at_ = {};
  last_burst_request_at_ = {};
  interactive_followup_.reset();
  tracking_burst_ = false;
}

} // namespace lemma::core
