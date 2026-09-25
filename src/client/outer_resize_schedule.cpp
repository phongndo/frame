#include "client/outer_resize_schedule.hpp"

#include <algorithm>

namespace lemma::client {

void OuterResizeSchedule::observe(const TimePoint now) noexcept {
  if (deadline_.has_value()) {
    return;
  }
  deadline_ = last_sent_.has_value() ? std::max(now, *last_sent_ + commit_interval) : now;
}

void OuterResizeSchedule::commit(const TimePoint now, const bool sent) noexcept {
  deadline_.reset();
  if (sent) {
    last_sent_ = now;
  }
}

} // namespace lemma::client
