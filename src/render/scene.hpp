#ifndef LEMMA_RENDER_SCENE_HPP
#define LEMMA_RENDER_SCENE_HPP

#include "lemma/geometry.hpp"
#include "lemma/terminal/terminal.hpp"
#include "render/grid.hpp"

#include <cstdint>
#include <span>

namespace lemma::render {

struct Viewport final {
  std::uint16_t columns{0};
  std::uint16_t rows{0};
};

using PaneRectangle = lemma::PaneRectangle;

struct PaneSurface final {
  vt::Terminal* terminal{nullptr};
  PaneRectangle rectangle{};
  std::uint16_t cursor_override_column{0};
  std::uint16_t cursor_override_row{0};
  bool focused{false};
  bool cursor_override{false};
  bool presentation_suppressed{false};
  bool border_right{false};
  bool border_bottom{false};
  // Re-emit the Pane's OSC 8 hyperlinks to the outer terminal.
  bool hyperlinks{true};
};

struct GridSurface final {
  Grid* grid{nullptr};
  PaneRectangle rectangle{};
  bool focused{false};
  bool opaque{true};
};

// One resolved Attachment projection. Pane and Grid storage is borrowed for one composition call;
// retained content and authoritative geometry remain with their native owners.
struct Scene final {
  std::span<const PaneSurface> panes;
  // Grid order is back-to-front. Later opaque Grids may fully occlude earlier Scene content.
  std::span<const GridSurface> grids;
};

} // namespace lemma::render

#endif // LEMMA_RENDER_SCENE_HPP
