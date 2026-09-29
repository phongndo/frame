#ifndef FRAME_CORE_PRESENTATION_GATE_HPP
#define FRAME_CORE_PRESENTATION_GATE_HPP

#include <chrono>
#include <cstdint>
#include <optional>

namespace frame::core {

enum class PresentationSuppression : std::uint8_t {
  inactive,
  held,
  watchdog_released,
};

struct PresentationGateUpdate final {
  bool visible_damage{false};
  bool presentation_deferred{false};
  bool urgent_render{false};
  bool force_full{false};
};

// Pane-local presentation policy for canonical synchronized-output mode. This gate never mutates
// Ghostty mode 2026: its watchdog releases presentation only, leaving child state authoritative.
class PresentationGate final {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  [[nodiscard]] auto observe(bool child_synchronized_output, bool damage, TimePoint now) noexcept
      -> PresentationGateUpdate;
  [[nodiscard]] auto release_if_expired(TimePoint now) noexcept -> PresentationGateUpdate;

  [[nodiscard]] auto suppression() const noexcept -> PresentationSuppression {
    return suppression_;
  }
  [[nodiscard]] auto presentation_suppressed() const noexcept -> bool {
    return suppression_ == PresentationSuppression::held;
  }
  [[nodiscard]] auto child_synchronized_output() const noexcept -> bool {
    return child_synchronized_output_;
  }
  [[nodiscard]] auto deadline() const noexcept -> std::optional<TimePoint>;
  [[nodiscard]] auto watchdog_releases() const noexcept -> std::uint64_t {
    return watchdog_releases_;
  }

private:
  TimePoint deadline_;
  PresentationSuppression suppression_{PresentationSuppression::inactive};
  std::uint64_t watchdog_releases_{0};
  bool child_synchronized_output_{false};
  bool pending_output_{false};
};

} // namespace frame::core

#endif // FRAME_CORE_PRESENTATION_GATE_HPP
