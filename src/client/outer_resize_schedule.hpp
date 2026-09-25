#ifndef LEMMA_CLIENT_OUTER_RESIZE_SCHEDULE_HPP
#define LEMMA_CLIENT_OUTER_RESIZE_SCHEDULE_HPP

#include <chrono>
#include <optional>

namespace lemma::client {

// Paces outer-terminal geometry commits. A change after a quiet interval commits immediately.
// Changes observed within one interval of the previously sent geometry coalesce into a single
// commit at the end of that interval, and later observations never postpone a pending deadline.
// A continuous window drag therefore sends at most one geometry per interval, and its final size
// follows the last observation by at most one interval.
class OuterResizeSchedule final {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  // One 60 Hz display frame, matching the daemon's sustained-output presentation cadence.
  // Intermediate geometry committed faster than the outer terminal can present it is invisible,
  // yet each commit costs every visible Pane a Ghostty reflow, a child SIGWINCH redraw, and a full
  // attachment frame.
  static constexpr auto commit_interval = std::chrono::milliseconds(16);

  void observe(TimePoint now) noexcept;
  // Resolves the pending observation. `sent` is false when the settled geometry matched the last
  // sent geometry, so the daemon performed no work and the interval is not consumed.
  void commit(TimePoint now, bool sent) noexcept;

  [[nodiscard]] auto pending() const noexcept -> bool { return deadline_.has_value(); }
  [[nodiscard]] auto deadline() const noexcept -> std::optional<TimePoint> { return deadline_; }
  [[nodiscard]] auto due(TimePoint now) const noexcept -> bool {
    return deadline_.has_value() && now >= *deadline_;
  }

private:
  std::optional<TimePoint> deadline_;
  std::optional<TimePoint> last_sent_;
};

} // namespace lemma::client

#endif // LEMMA_CLIENT_OUTER_RESIZE_SCHEDULE_HPP
