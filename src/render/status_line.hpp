#ifndef LEMMA_RENDER_STATUS_LINE_HPP
#define LEMMA_RENDER_STATUS_LINE_HPP

#include "lemma/limits.hpp"
#include "render/scene.hpp"
#include "render/ui.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace lemma::render {

inline constexpr std::size_t status_tabs_max = 16;
inline constexpr std::size_t status_context_bytes_max = limits::search_query_bytes_max + 64U;
inline constexpr std::size_t status_attention_bytes_max = 12;

struct StatusTab final {
  std::uint16_t number{0};
  std::string_view title;
  bool active{false};
  // Printable ASCII drawn emphasized after an inactive Tab's title. The active Tab carries none.
  std::string_view attention{}; // NOLINT(readability-redundant-member-init)
};

enum class StatusPromptTarget : std::uint8_t {
  none,
  session,
  active_tab,
  command_line,
  copy_search_forward,
  copy_search_backward,
  message,
};

enum class StatusPromptFeedback : std::uint8_t {
  none,
  invalid,
  conflict,
};

struct StatusLine final {
  std::string_view session_name;
  std::span<const StatusTab> tabs;
  StatusPromptTarget prompt_target{StatusPromptTarget::none};
  StatusPromptFeedback prompt_feedback{StatusPromptFeedback::none};
  std::string_view prompt_value;
  std::string_view input_context;
  std::size_t prompt_cursor{0};
  bool dirty{false};

  [[nodiscard]] constexpr auto prompting() const noexcept -> bool {
    return prompt_target != StatusPromptTarget::none &&
           prompt_target != StatusPromptTarget::message;
  }
};

enum class StatusTargetKind : std::uint8_t {
  tab,
  create_tab,
};

struct StatusTarget final {
  StatusTargetKind kind{StatusTargetKind::tab};
  std::size_t tab_position{0};

  [[nodiscard]] constexpr auto operator==(const StatusTarget&) const noexcept -> bool = default;
};

// Returns the status control owning the zero-based outer-terminal column. Session cells,
// separators, overflow markers, spacing, prompts, and modal status rows are not targets.
// The hit test and status renderer share one bounded projection.
[[nodiscard]] auto status_target_at_column(StatusLine status, Viewport viewport,
                                           std::uint16_t column) noexcept
    -> std::optional<StatusTarget>;

// Pure first-party UI projection, also used by the external user-UI program. The retained Scene
// renderer itself does not need to call this projection for extension-owned status Surfaces.
[[nodiscard]] auto project_status_cells(StatusLine status, Viewport viewport,
                                        std::span<ui::Cell> cells, std::uint16_t& cursor) noexcept
    -> bool;

[[nodiscard]] auto valid_status(StatusLine status) noexcept -> bool;
[[nodiscard]] auto status_cursor_column(StatusLine status, Viewport viewport) noexcept
    -> std::uint16_t;

} // namespace lemma::render

#endif // LEMMA_RENDER_STATUS_LINE_HPP
