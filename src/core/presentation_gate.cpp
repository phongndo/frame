#include "core/presentation_gate.hpp"

#include "frame/limits.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace frame::core {

[[nodiscard]] auto PresentationGate::observe(const bool child_synchronized_output,
                                             const bool damage, const TimePoint now) noexcept
    -> PresentationGateUpdate {
  child_synchronized_output_ = child_synchronized_output;
  if (!child_synchronized_output) {
    const bool released = suppression_ != PresentationSuppression::inactive;
    const bool pending = pending_output_ || damage;
    suppression_ = PresentationSuppression::inactive;
    deadline_ = {};
    pending_output_ = false;
    return {
        .visible_damage = pending,
        .urgent_render = released,
    };
  }

  if (suppression_ == PresentationSuppression::inactive) {
    suppression_ = PresentationSuppression::held;
    deadline_ = now + limits::synchronized_output_presentation_timeout;
  }
  if (suppression_ == PresentationSuppression::held) {
    pending_output_ = pending_output_ || damage;
    return {
        .presentation_deferred = damage,
    };
  }
  return {
      .visible_damage = damage,
  };
}

[[nodiscard]] auto PresentationGate::release_if_expired(const TimePoint now) noexcept
    -> PresentationGateUpdate {
  if (suppression_ != PresentationSuppression::held || now < deadline_) {
    return {};
  }
  suppression_ = PresentationSuppression::watchdog_released;
  deadline_ = {};
  const bool pending = pending_output_;
  pending_output_ = false;
  if (watchdog_releases_ < std::numeric_limits<std::uint64_t>::max()) {
    ++watchdog_releases_;
  }
  return {
      .visible_damage = pending,
      .urgent_render = true,
      .force_full = true,
  };
}

[[nodiscard]] auto PresentationGate::deadline() const noexcept -> std::optional<TimePoint> {
  return suppression_ == PresentationSuppression::held ? std::optional(deadline_) : std::nullopt;
}

} // namespace frame::core
