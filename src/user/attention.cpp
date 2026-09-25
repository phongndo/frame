#include "user/attention.hpp"

#include "api/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lemma::user {
namespace {

using api::JsonKind;
using api::JsonValue;

[[nodiscard]] auto object_member(const JsonValue& value, const std::string_view name)
    -> const JsonValue* {
  const auto* const found = api::json_member(value, name);
  return found != nullptr && found->kind == JsonKind::object ? found : nullptr;
}

[[nodiscard]] auto count(const JsonValue& value, const std::string_view name) -> std::uint64_t {
  const auto found = api::json_unsigned(value, name);
  if (!found.has_value()) {
    throw std::runtime_error("invalid pane signal record");
  }
  return *found;
}

[[nodiscard]] auto progress_state(const std::string_view state) -> Progress {
  constexpr std::array names{"normal", "error", "indeterminate", "paused"};
  const auto* const found = std::ranges::find(names, state);
  if (found == names.end()) {
    throw std::runtime_error("invalid pane progress state");
  }
  return static_cast<Progress>(1 + (found - names.begin()));
}

[[nodiscard]] constexpr auto progress_rank(const Progress progress) noexcept -> int {
  switch (progress) {
  case Progress::none:
    return 0;
  case Progress::normal:
  case Progress::indeterminate:
    return 1;
  case Progress::paused:
    return 2;
  case Progress::error:
    return 3;
  }
  return 0;
}

class MarkerWriter final {
public:
  void progress(const PaneSignals& signals) noexcept {
    if (signals.percent.has_value() && signals.progress != Progress::indeterminate) {
      std::array<char, 3> digits{};
      const auto result = std::to_chars(digits.begin(), digits.end(), *signals.percent);
      if (result.ec == std::errc{}) {
        for (const char digit : std::span(digits.begin(), result.ptr)) {
          put(digit);
        }
      }
    }
    put('%');
    if (signals.progress == Progress::paused) {
      put('=');
    }
  }

  void put(const char character) noexcept {
    if (marker_.size < marker_.text.size()) {
      std::span(marker_.text).subspan(marker_.size, 1).front() = character;
      ++marker_.size;
    }
  }

  [[nodiscard]] auto marker() const noexcept -> Marker { return marker_; }

private:
  Marker marker_;
};

} // namespace

auto decode_signals(const JsonValue& signals) -> PaneSignals {
  PaneSignals result{
      .generation = count(signals, "generation"),
      .bells = count(signals, "bells"),
      .notifications = count(signals, "notifications"),
      .commands = count(signals, "commands"),
      .exit_code = std::nullopt,
      .progress = Progress::none,
      .percent = std::nullopt,
  };
  if (const auto* const command = object_member(signals, "command"); command != nullptr) {
    if (const auto* const code = api::json_member(*command, "exit_code");
        code != nullptr && code->kind == JsonKind::number) {
      result.exit_code = code->number;
    }
  }
  if (const auto* const progress = object_member(signals, "progress"); progress != nullptr) {
    result.progress = progress_state(api::json_string(*progress, "state").value_or(""));
    if (const auto percent = api::json_unsigned(*progress, "percent");
        percent.has_value() && *percent <= 100U) {
      result.percent = static_cast<std::uint8_t>(*percent);
    }
  }
  return result;
}

void TabAttention::see(Entry& entry) noexcept {
  entry.seen_bells = entry.latest.bells;
  entry.seen_notifications = entry.latest.notifications;
  entry.seen_commands = entry.latest.commands;
}

void TabAttention::list(const std::span<const PaneMember> panes) {
  // A Session never lists more; clamping keeps a malformed listing from dropping the statusline.
  const auto listed = panes.first(std::min(panes.size(), attention_panes_max));
  std::vector<Entry> next;
  next.reserve(listed.size());
  for (const auto& member : listed) {
    const auto found = std::ranges::find(panes_, member.pane, &Entry::pane);
    auto& entry = found == panes_.end() ? next.emplace_back(Entry{.pane = std::string(member.pane),
                                                                  .tab = {},
                                                                  .latest = member.signals,
                                                                  .seen_bells = 0,
                                                                  .seen_notifications = 0,
                                                                  .seen_commands = 0})
                                        : next.emplace_back(std::move(*found));
    entry.tab = member.tab;
    // Events delivered while the listing was requested can be older than the listing.
    if (member.signals.generation >= entry.latest.generation) {
      entry.latest = member.signals;
    }
    if (!listed_ || entry.tab == active_) {
      see(entry);
    }
  }
  panes_ = std::move(next);
  listed_ = true;
}

auto TabAttention::signal(const std::string_view pane, const PaneSignals& signals) -> bool {
  const auto found = std::ranges::find(panes_, pane, &Entry::pane);
  if (found == panes_.end()) {
    return false;
  }
  if (signals.generation > found->latest.generation) {
    found->latest = signals;
    if (found->tab == active_) {
      see(*found);
    }
  }
  return true;
}

void TabAttention::visit(const std::string_view tab) {
  if (active_ == tab) {
    return;
  }
  active_ = tab;
  for (auto& entry : panes_) {
    if (entry.tab == active_) {
      see(entry);
    }
  }
}

auto TabAttention::marker(const std::string_view tab) const noexcept -> Marker {
  if (tab == active_) {
    return {};
  }
  const Entry* progress = nullptr;
  bool failed = false;
  bool alerted = false;
  for (const auto& entry : panes_) {
    if (entry.tab != tab) {
      continue;
    }
    const auto& latest = entry.latest;
    if (progress_rank(latest.progress) >
        (progress == nullptr ? 0 : progress_rank(progress->latest.progress))) {
      progress = &entry;
    }
    failed = failed || (latest.commands > entry.seen_commands && latest.exit_code.has_value() &&
                        *latest.exit_code != 0);
    alerted = alerted || latest.bells > entry.seen_bells ||
              latest.notifications > entry.seen_notifications;
  }
  MarkerWriter writer;
  if (progress != nullptr) {
    writer.progress(progress->latest);
    failed = failed || progress->latest.progress == Progress::error;
  }
  if (failed) {
    writer.put('x');
  }
  if (alerted) {
    writer.put('!');
  }
  return writer.marker();
}

} // namespace lemma::user
