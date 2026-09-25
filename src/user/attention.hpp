#ifndef LEMMA_USER_ATTENTION_HPP
#define LEMMA_USER_ATTENTION_HPP

#include "api/json.hpp"
#include "lemma/limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lemma::user {

// Core's per-Session Pane bound (core::panes_per_session_max); a unit test asserts they agree.
inline constexpr std::size_t attention_panes_max =
    static_cast<std::size_t>(limits::panes_hard_max / limits::sessions_hard_max);
// The longest marker: "100%=x!".
inline constexpr std::size_t attention_marker_bytes_max = 7;

enum class Progress : std::uint8_t {
  none,
  normal,
  error,
  indeterminate,
  paused,
};

// The subset of a public Pane signal record that statusline markers use.
struct PaneSignals final {
  std::uint64_t generation{0};
  std::uint64_t bells{0};
  std::uint64_t notifications{0};
  std::uint64_t commands{0};
  std::optional<std::int64_t> exit_code{}; // NOLINT(readability-redundant-member-init)
  Progress progress{Progress::none};
  std::optional<std::uint8_t> percent{}; // NOLINT(readability-redundant-member-init)
};

// Decodes the `signals` object of `pane.signal`, `pane.list`, or `pane.inspect`.
[[nodiscard]] auto decode_signals(const api::JsonValue& signals) -> PaneSignals;

struct PaneMember final {
  std::string_view pane;
  std::string_view tab;
  PaneSignals signals;
};

struct Marker final {
  std::array<char, attention_marker_bytes_max> text{};
  std::size_t size{0};

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {text.data(), size}; }
  [[nodiscard]] auto operator==(const Marker& other) const noexcept -> bool {
    return view() == other.view();
  }
};

// Per-Session statusline attention: which inactive Tabs have unseen bells or notifications,
// something failed (progress in error, or a failed command since the last visit), or progress in
// flight. Pane->Tab membership comes only from
// complete `pane.list` projections; signal records never create Panes.
class TabAttention final {
public:
  // Attention for a Session observed from its creation: no listed Pane counts as already seen.
  [[nodiscard]] static auto since_creation() -> TabAttention {
    TabAttention attention;
    attention.listed_ = true;
    return attention;
  }

  // Replaces membership with a complete listing. Unless the Session was observed from creation,
  // Panes in the first listing are seen as listed. Panes that appear later, including while
  // detached, count every signal since their creation.
  void list(std::span<const PaneMember> panes);
  // Applies a newer signal record. Returns false for a Pane absent from the last listing.
  [[nodiscard]] auto signal(std::string_view pane, const PaneSignals& signals) -> bool;
  // Records the active Tab; its Panes stay seen while it remains active.
  void visit(std::string_view tab);
  // No Tab is visible until the next visit, as while the Session is detached.
  void detach() noexcept { active_.clear(); }
  [[nodiscard]] auto marker(std::string_view tab) const noexcept -> Marker;

private:
  struct Entry final {
    std::string pane;
    std::string tab;
    PaneSignals latest;
    std::uint64_t seen_bells{0};
    std::uint64_t seen_notifications{0};
    std::uint64_t seen_commands{0};
  };

  static void see(Entry& entry) noexcept;

  std::vector<Entry> panes_;
  std::string active_;
  bool listed_{false};
};

} // namespace lemma::user

#endif
