#include "frame/base64.hpp"
#include "render/graphics.hpp"
#include "render/pane_composition.hpp"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace frame::render {
namespace {
void write(vt::Terminal& terminal, const std::string_view bytes) {
  terminal.write(std::as_bytes(std::span(bytes)));
}
auto terminal(const std::uint16_t columns = 20, const std::uint16_t rows = 10) -> vt::Terminal {
  vt::TerminalOptions options;
  options.size = {.columns = columns, .rows = rows};
  return vt::Terminal::create(options).value();
}
void transmit(vt::Terminal& terminal, const std::uint32_t width = 2, const std::uint32_t height = 1,
              const std::string_view parameters = "c=4,r=2") {
  const std::string pixels(static_cast<std::size_t>(width) * height * 4U, '\xff');
  for (std::size_t offset = 0; offset < pixels.size(); offset += 3072U) {
    const auto count = std::min(std::size_t{3072}, pixels.size() - offset);
    std::string packet = offset == 0 ? "\x1b_Ga=T,q=2,C=1,f=32,i=1," + std::string(parameters) +
                                           ",s=" + std::to_string(width) +
                                           ",v=" + std::to_string(height) + ",m="
                                     : "\x1b_Gq=2,m=";
    packet += offset + count == pixels.size() ? "0;" : "1;";
    packet += base64::encode(std::string_view(pixels).substr(offset, count));
    packet += "\x1b\\";
    write(terminal, packet);
  }
}
auto project(GraphicsProjection& graphics, vt::Terminal& outer, const Scene scene,
             const bool full = false) -> std::size_t {
  std::vector<std::byte> output(std::size_t{256} * 1024U);
  const auto size = outer.size();
  const auto frame = compose_scene(scene, {.columns = size.columns, .rows = size.rows}, output,
                                   full, {}, {}, {}, &graphics);
  EXPECT_TRUE(frame);
  if (!frame) {
    return 0;
  }
  outer.write(std::span(output).first(frame->bytes));
  return frame->bytes;
}
void settle(GraphicsProjection& graphics, vt::Terminal& outer, const Scene scene,
            const bool full = false) {
  project(graphics, outer, scene, full);
  for (int i = 0; graphics.pending() && i < 100; ++i) {
    project(graphics, outer, scene);
  }
  ASSERT_FALSE(graphics.pending());
}
// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(GraphicsTest, ReadyUploadsArePlacedInTheSameFrameWithoutBlankReplacement) {
  auto source = terminal();
  auto outer = terminal();
  GraphicsProjection graphics;
  const std::array panes{
      PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}, .focused = true}};
  const Scene scene{.panes = panes, .grids = {}};
  for (const std::string_view pixels : {"/wAA/w==", "AAD//w=="}) {
    write(source,
          "\x1b_Ga=T,q=2,C=1,f=32,i=1,p=1,s=1,v=1,c=4,r=2;" + std::string(pixels) + "\x1b\\");
    project(graphics, outer, scene);
    EXPECT_FALSE(graphics.pending());
    std::array<vt::GraphicPlacement, 8> placements{};
    ASSERT_EQ(outer.graphics(placements).value(), 1U);
    const auto expected = base64::decode(pixels, 4);
    if (!expected) {
      ADD_FAILURE() << "invalid pixel fixture";
      return;
    }
    EXPECT_TRUE(std::ranges::equal(placements.front().pixels, std::as_bytes(std::span(*expected))));
  }
}

TEST(GraphicsTest, CompletedUploadResumesPlacementWhenTheFrameHasNoRoomLeft) {
  auto source = terminal();
  auto outer = terminal();
  transmit(source, 32, 24);
  GraphicsProjection graphics;
  const std::array panes{
      PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}, .focused = true}};
  const Scene scene{.panes = panes, .grids = {}};
  std::array<std::byte, 4400> output{};
  const auto first = graphics.append(scene, 0, output).value();
  ASSERT_GT(first, 0U);
  outer.write(std::span(output).first(first));
  ASSERT_TRUE(graphics.pending());
  const auto second = graphics.append(scene, 0, output).value();
  ASSERT_GT(second, 0U);
  outer.write(std::span(output).first(second));
  EXPECT_FALSE(graphics.pending());
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  EXPECT_EQ(placements.front().pixels.size(), 3072U);
}

TEST(GraphicsTest, RetainedImagesAreNamespacedPositionedAndRecreatedAfterFullRedraw) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  write(source, "\x1b[2;3H");
  transmit(source);
  std::array<vt::GraphicPlacement, 8> placements{};
  const auto initial = source.graphics(placements);
  ASSERT_TRUE(initial);
  ASSERT_EQ(*initial, 1U);
  EXPECT_EQ(placements.at(0).column, 2);
  EXPECT_EQ(placements.at(0).row, 1);
  GraphicsProjection graphics;
  const std::array panes{
      PaneSurface{.terminal = &source,
                  .rectangle = {.column = 10, .row = 4, .columns = 20, .rows = 10},
                  .focused = true}};
  const Scene scene{.panes = panes, .grids = {}};
  settle(graphics, outer, scene, true);
  std::array<std::byte, 1024> unchanged{};
  EXPECT_EQ(graphics.append(scene, 0, unchanged).value(), 0U);
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  EXPECT_EQ(placements.at(0).column, 12);
  EXPECT_EQ(placements.at(0).row, 5);
  EXPECT_EQ(placements.at(0).pixel_width, 32U);
  EXPECT_EQ(placements.at(0).pixels.size(), 8U);
  settle(graphics, outer, scene, true);
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  write(source, "\x1b_Ga=d,d=A,q=2\x1b\\");
  settle(graphics, outer, scene);
  EXPECT_EQ(outer.graphics(placements).value(), 0U);
}
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(GraphicsTest, UploadsAreBoundedAndSurviveUnrelatedPtyOutput) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  transmit(source, 256, 256);
  GraphicsProjection graphics;
  const std::array panes{
      PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}, .focused = true}};
  const Scene scene{.panes = panes, .grids = {}};
  EXPECT_LT(project(graphics, outer, scene, true), 70U * 1024U);
  ASSERT_TRUE(graphics.pending());
  write(source, "progress");
  for (int i = 0; graphics.pending() && i < 100; ++i) {
    EXPECT_LT(project(graphics, outer, scene), 70U * 1024U);
  }
  ASSERT_FALSE(graphics.pending());
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  EXPECT_EQ(placements.at(0).pixels.size(), 256U * 256U * 4U);
}
TEST(GraphicsTest, PaneEdgesClipImagesRatherThanDrawingIntoNeighbors) {
  auto source = terminal(4, 2);
  auto outer = terminal(20, 10);
  write(source, "\x1b[2;3H");
  transmit(source);
  GraphicsProjection graphics;
  const std::array panes{PaneSurface{.terminal = &source,
                                     .rectangle = {.column = 5, .row = 3, .columns = 4, .rows = 2},
                                     .focused = true}};
  settle(graphics, outer, {.panes = panes, .grids = {}}, true);
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  EXPECT_EQ(placements.at(0).column, 7);
  EXPECT_EQ(placements.at(0).row, 4);
  EXPECT_LE(placements.at(0).pixel_width, 16U);
  EXPECT_LE(placements.at(0).pixel_height, 16U);
}
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(GraphicsTest, PlaceholderGeometryAndVirtualParentChainsStayNative) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  transmit(source, 16, 16, "U=1,p=77,c=2,r=2");
  constexpr std::string_view placeholder = "\xF4\x8E\xBB\xAE";
  write(source, "\x1b[1;1H\x1b[38;2;0;0;1;58;2;0;0;77m");
  write(source, std::string(placeholder) + "\xCC\x85\xCC\x85" + std::string(placeholder));
  write(source, "\x1b[2;1H");
  write(source,
        std::string(placeholder) + "\xCC\x8D\xCC\x85" + std::string(placeholder) + "\x1b[0m");
  write(source, "\x1b_Ga=t,q=2,f=32,i=2,s=1,v=1;/wAA/w==\x1b\\");
  write(source, "\x1b_Ga=p,q=2,C=1,i=2,p=1,P=1,Q=77,H=1,V=2,c=1,r=1\x1b\\");
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(source.graphics(placements).value(), 3U);
  GraphicsProjection graphics;
  const std::array panes{
      PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}}};
  const Scene scene{.panes = panes, .grids = {}};
  settle(graphics, outer, scene, true);
  ASSERT_EQ(outer.graphics(placements).value(), 3U);
  bool first = false;
  bool second = false;
  bool child = false;
  for (const auto& placed : std::span(placements).first(3)) {
    if (placed.column == 0 && placed.row == 0) {
      EXPECT_EQ(placed.offset_y, 8U);
      EXPECT_EQ(placed.pixel_width, 16U);
      EXPECT_EQ(placed.pixel_height, 8U);
      first = true;
    } else if (placed.column == 0 && placed.row == 1) {
      EXPECT_EQ(placed.pixel_height, 8U);
      second = true;
    } else if (placed.column == 1 && placed.row == 2) {
      child = true;
    }
  }
  EXPECT_TRUE(first && second && child);
  write(source, "\x1b[2J");
  settle(graphics, outer, scene, true);
  EXPECT_EQ(outer.graphics(placements).value(), 0U);
}

TEST(GraphicsTest, AnimationUsesNativePixelsAndCallerOwnedMonotonicDeadlines) {
  auto source = terminal();
  write(source, "\x1b_Ga=T,q=2,C=1,i=1,s=1,v=1,f=32;/wAA/w==\x1b\\");
  write(source, "\x1b_Ga=f,q=2,i=1,s=1,v=1,f=32,z=25;AAD//w==\x1b\\");
  write(source, "\x1b_Ga=a,q=2,i=1,r=1,z=25,s=3\x1b\\");
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(source.graphics(placements).value(), 1U);
  const auto before = placements.at(0).image_generation;
  EXPECT_EQ(placements.at(0).pixels.front(), std::byte{255});
  ASSERT_EQ(source.tick_graphics(1000).value(), std::optional<std::uint64_t>{25});
  ASSERT_EQ(source.tick_graphics(1024).value(), std::optional<std::uint64_t>{1});
  ASSERT_EQ(source.tick_graphics(1025).value(), std::optional<std::uint64_t>{25});
  ASSERT_EQ(source.graphics(placements).value(), 1U);
  EXPECT_NE(placements.at(0).image_generation, before);
  EXPECT_EQ(placements.at(0).pixels.front(), std::byte{0});
  EXPECT_EQ(placements.at(0).pixels.subspan(2, 1).front(), std::byte{255});
  write(source, "\x1b_Ga=a,q=2,i=1,s=1\x1b\\");
  EXPECT_FALSE(source.tick_graphics(1050).value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(GraphicsTest, SurfaceOcclusionAndSuppressedPresentationDoNotLeakImages) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  transmit(source, 16, 16, "c=8,r=4");
  auto grid = Grid::create(2, 2).value();
  const std::array grids{
      GridSurface{.grid = &grid, .rectangle = {.column = 2, .row = 1, .columns = 2, .rows = 2}}};
  std::array panes{PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}}};
  GraphicsProjection graphics;
  settle(graphics, outer, {.panes = panes, .grids = grids}, true);
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(outer.graphics(placements).value(), 4U);
  for (const auto& placed : std::span(placements).first(4)) {
    const auto left = (placed.column * 8) + static_cast<int>(placed.offset_x);
    const auto top = (placed.row * 16) + static_cast<int>(placed.offset_y);
    EXPECT_TRUE(left >= 32 || top >= 48 || left + static_cast<int>(placed.pixel_width) <= 16 ||
                top + static_cast<int>(placed.pixel_height) <= 16);
  }
  // While synchronized output is withheld, canonical mutations cannot replace the presented image.
  settle(graphics, outer, {.panes = panes, .grids = {}}, true);
  panes.at(0).presentation_suppressed = true;
  write(source, "\x1b_Ga=d,d=A,q=2\x1b\\");
  settle(graphics, outer, {.panes = panes, .grids = {}});
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  settle(graphics, outer, {.panes = panes, .grids = grids}, true);
  EXPECT_EQ(outer.graphics(placements).value(), 0U);
  panes.at(0).presentation_suppressed = false;
  settle(graphics, outer, {.panes = panes, .grids = {}}, true);
  EXPECT_EQ(outer.graphics(placements).value(), 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(GraphicsTest, TransparentSurfaceOnlyOccludesItsPaintedCells) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  transmit(source);
  auto grid = Grid::create(2, 2).value();
  const std::array grids{
      GridSurface{.grid = &grid, .rectangle = {.columns = 2, .rows = 2}, .opaque = false}};
  const std::array panes{
      PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}}};
  const Scene scene{.panes = panes, .grids = grids};
  GraphicsProjection graphics;
  settle(graphics, outer, scene, true);
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  ASSERT_TRUE(grid.apply(
      {.rows = {{.runs = {{.text = "X", .column = 1}}, .row = 0}}, .styles = {}, .cursor = {}}));
  settle(graphics, outer, scene);
  ASSERT_EQ(outer.graphics(placements).value(), 3U);
  for (const auto& placed : std::span(placements).first(3)) {
    const auto left = (placed.column * 8) + static_cast<int>(placed.offset_x);
    const auto top = (placed.row * 16) + static_cast<int>(placed.offset_y);
    EXPECT_TRUE(left >= 16 || top >= 16 || left + static_cast<int>(placed.pixel_width) <= 8);
  }
  const auto generation = grid.generation();
  const auto rejected = grid.apply(
      {.rows = {{.runs = {{.text = "\xF4\x8E\xBB\xAE"}}, .row = 0}}, .styles = {}, .cursor = {}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), GridError::invalid_run);
  EXPECT_EQ(grid.generation(), generation);
}
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(GraphicsTest, TextCoverageOnlyConsumesCapacityWhereImagesNeedClipping) {
  auto source = terminal(80, 24);
  auto outer = terminal(80, 24);
  auto grid = Grid::create(80, 24).value();
  GridPatch patch;
  for (std::uint16_t row = 0; row < 20; ++row) {
    GridRowPatch cells;
    cells.row = row;
    for (std::uint16_t column = 0; column < 14; ++column) {
      cells.runs.push_back({.text = "X", .column = static_cast<std::uint16_t>(column * 2)});
    }
    patch.rows.push_back(std::move(cells));
  }
  ASSERT_TRUE(grid.apply(std::move(patch)));
  std::array panes{PaneSurface{.terminal = &source, .rectangle = {.columns = 80, .rows = 24}}};
  const std::array grids{
      GridSurface{.grid = &grid, .rectangle = {.columns = 80, .rows = 24}, .opaque = false}};
  const Scene scene{.panes = panes, .grids = grids};
  GraphicsProjection graphics;
  settle(graphics, outer, scene, true);
  // The image lies inside the Surface's rectangle, but outside its 280 painted runs.
  write(source, "\x1b[24;80H");
  transmit(source, 1, 1, "c=1,r=1");
  settle(graphics, outer, {.panes = panes, .grids = {}}, true);
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  settle(graphics, outer, scene);
  EXPECT_EQ(outer.graphics(placements).value(), 1U);
  write(source, "\x1b_Ga=d,d=A,q=2\x1b\\");
  ASSERT_EQ(source.graphics(placements).value(), 0U);
  settle(graphics, outer, scene);
  EXPECT_EQ(outer.graphics(placements).value(), 0U);
  panes.at(0).presentation_suppressed = true;
  settle(graphics, outer, scene);
  EXPECT_EQ(outer.graphics(placements).value(), 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(GraphicsTest, ImageIdTieBreakingSurvivesReplacementAndOuterNamespacing) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  write(source, "\x1b_Ga=T,q=2,C=1,i=2,s=1,v=1,f=32;AAD//w==\x1b\\");
  write(source, "\x1b_Ga=T,q=2,C=1,i=1,s=1,v=1,f=32;/wAA/w==\x1b\\");
  GraphicsProjection graphics;
  const std::array panes{
      PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}}};
  const Scene scene{.panes = panes, .grids = {}};
  const auto ordered = [&] {
    std::array<vt::GraphicPlacement, 8> placements{};
    EXPECT_EQ(outer.graphics(placements).value(), 2U);
    const auto& first = placements.at(0);
    const auto& second = placements.at(1);
    const auto blue = [](const vt::GraphicPlacement& item) {
      return item.pixels.subspan(2, 1).front() == std::byte{255};
    };
    EXPECT_NE(blue(first), blue(second));
    EXPECT_TRUE(blue(first) ? first.z > second.z : second.z > first.z);
  };
  settle(graphics, outer, scene, true);
  ordered();
  write(source, "\x1b_Ga=T,q=2,C=1,i=1,s=1,v=1,f=32;AP8A/w==\x1b\\");
  settle(graphics, outer, scene);
  ordered();
}
TEST(GraphicsTest, FractionalCellScalingPreservesExactPixelBounds) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  write(source, "\x1b_Ga=T,q=2,C=1,i=1,s=1,v=1,f=32,c=3,X=3,Y=5;/wAA/w==\x1b\\");
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(source.graphics(placements).value(), 1U);
  const auto width = placements.at(0).pixel_width;
  const auto height = placements.at(0).pixel_height;
  GraphicsProjection graphics;
  const std::array panes{
      PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}}};
  settle(graphics, outer, {.panes = panes, .grids = {}}, true);
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  EXPECT_EQ(placements.at(0).pixel_width, width);
  EXPECT_EQ(placements.at(0).pixel_height, height);
  EXPECT_EQ(placements.at(0).offset_x, 3U);
  EXPECT_EQ(placements.at(0).offset_y, 5U);
  EXPECT_EQ(placements.at(0).pixels.front(), std::byte{255});
  EXPECT_EQ(placements.at(0).pixels.subspan(1, 1).front(), std::byte{0});
}
TEST(GraphicsTest, PngDecodeResizeAndFreshAttachmentKeepCanonicalOwnership) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  write(source, "\x1b_Ga=T,q=2,C=1,f=100,i=1,c=1,r=1;"
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/"
                "iZk9HQAAAABJRU5ErkJggg==\x1b\\");
  std::array panes{PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}}};
  GraphicsProjection graphics;
  settle(graphics, outer, {.panes = panes, .grids = {}}, true);
  std::array<vt::GraphicPlacement, 8> placements{};
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  EXPECT_EQ(placements.at(0).pixels.front(), std::byte{255});
  EXPECT_EQ(placements.at(0).pixel_width, 8U);
  ASSERT_TRUE(
      source.resize({.columns = 20, .rows = 10, .cell_width_px = 12, .cell_height_px = 24}));
  ASSERT_TRUE(outer.resize({.columns = 80, .rows = 24, .cell_width_px = 12, .cell_height_px = 24}));
  settle(graphics, outer, {.panes = panes, .grids = {}}, true);
  ASSERT_EQ(outer.graphics(placements).value(), 1U);
  EXPECT_EQ(placements.at(0).pixel_width, 12U);
  graphics.reset();
  auto fresh = terminal(80, 24);
  ASSERT_TRUE(fresh.resize(outer.size()));
  settle(graphics, fresh, {.panes = panes, .grids = {}}, true);
  ASSERT_EQ(fresh.graphics(placements).value(), 1U);
  EXPECT_EQ(placements.at(0).pixels.front(), std::byte{255});
  settle(graphics, fresh, {.panes = {}, .grids = {}}, true);
  EXPECT_EQ(fresh.graphics(placements).value(), 0U);
}

TEST(GraphicsTest, DeletedMultipartImageCannotLeaveAStalePlacement) {
  auto source = terminal();
  auto outer = terminal(80, 24);
  transmit(source, 256, 256);
  GraphicsProjection graphics;
  const std::array panes{
      PaneSurface{.terminal = &source, .rectangle = {.columns = 20, .rows = 10}, .focused = true}};
  const Scene scene{.panes = panes, .grids = {}};
  project(graphics, outer, scene, true);
  ASSERT_TRUE(graphics.pending());
  write(source, "\x1b_Ga=d,d=A,q=2\x1b\\");
  settle(graphics, outer, scene);
  std::array<vt::GraphicPlacement, 8> placements{};
  EXPECT_EQ(outer.graphics(placements).value(), 0U);
  transmit(source);
  settle(graphics, outer, scene);
  EXPECT_EQ(outer.graphics(placements).value(), 1U);
}
} // namespace
} // namespace frame::render
