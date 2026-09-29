#ifndef FRAME_RENDER_PANE_COMPOSITION_HPP
#define FRAME_RENDER_PANE_COMPOSITION_HPP

#include "frame/limits.hpp"
#include "render/scene.hpp"
#include "render/status_line.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace frame::render {
class GraphicsProjection;

inline constexpr std::size_t message_view_line_bytes_max = limits::status_message_bytes_max + 32U;

struct MessageViewLine final {
  std::string_view text;
  bool error{false};
};

struct MessageView final {
  std::span<const MessageViewLine> lines;
  bool active{false};
};

enum class OuterModeProjection : std::uint8_t {
  neutral,
  button_mouse,
  any_mouse,
};

enum class CompositionError : std::uint8_t {
  invalid_viewport,
  too_many_panes,
  invalid_pane,
  multiple_focused_panes,
  invalid_status,
  output_exhausted,
  terminal_error,
};

struct CompositionResult final {
  std::size_t bytes{0};
  std::size_t panes{0};
  std::size_t rows{0};
  OuterModeProjection outer_modes{OuterModeProjection::neutral};
  bool full{false};
  bool status{false};
};

// Composes one already-resolved Scene into a synchronized outer-terminal update. A visible native
// status line occupies the top row, and Scene coordinates are relative to the remaining content
// viewport. The focused projection owns cursor and outer input modes. Geometry changes require a
// full frame; retained Grid updates otherwise visit only damaged rows.
[[nodiscard]] auto
compose_scene(Scene scene, Viewport viewport, std::span<std::byte> output, bool force_full,
              StatusLine status = {},
              std::optional<OuterModeProjection> previous_outer_modes = std::nullopt,
              MessageView message_view = {}, GraphicsProjection* graphics = nullptr) noexcept
    -> std::expected<CompositionResult, CompositionError>;

// Pane-only compatibility interface retained for focused tests and callers while all production
// composition converges on Scene.
[[nodiscard]] auto
compose_frame(std::span<const PaneSurface> panes, Viewport viewport, std::span<std::byte> output,
              bool force_full, StatusLine status = {},
              std::optional<OuterModeProjection> previous_outer_modes = std::nullopt,
              MessageView message_view = {}) noexcept
    -> std::expected<CompositionResult, CompositionError>;

} // namespace frame::render

#endif // FRAME_RENDER_PANE_COMPOSITION_HPP
