#ifndef FRAME_RENDER_GRID_HPP
#define FRAME_RENDER_GRID_HPP

#include "frame/geometry.hpp"
#include "frame/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frame::render {

struct GridColor final {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};

  [[nodiscard]] constexpr auto operator==(const GridColor&) const noexcept -> bool = default;
};

struct GridStyle final {
  std::optional<GridColor> foreground;
  std::optional<GridColor> background;
  bool bold{false};
  bool faint{false};
  bool italic{false};
  bool underline{false};
  bool inverse{false};

  [[nodiscard]] constexpr auto operator==(const GridStyle&) const noexcept -> bool = default;
};

struct GridRun final {
  std::string text;
  std::uint16_t column{0};
  std::uint16_t columns{0};
  std::uint16_t style{0};
};

struct GridRowPatch final {
  std::vector<GridRun> runs;
  std::uint16_t row{0};
};

struct GridCursor final {
  std::uint16_t column{0};
  std::uint16_t row{0};
  bool visible{false};

  [[nodiscard]] constexpr auto operator==(const GridCursor&) const noexcept -> bool = default;
};

// A patch replaces each named row. Omitted rows retain their previous state. A supplied style
// table replaces the complete table; style zero always exists and represents terminal defaults.
struct GridPatch final {
  std::vector<GridRowPatch> rows;
  std::optional<std::vector<GridStyle>> styles;
  std::optional<GridCursor> cursor;
};

enum class GridError : std::uint8_t {
  invalid_dimensions,
  invalid_row,
  invalid_run,
  invalid_style,
  invalid_cursor,
  resource_limit,
  out_of_memory,
  output_exhausted,
};

struct GridApplyResult final {
  std::size_t retained_bytes{0};
  std::size_t changed_rows{0};
  std::uint64_t generation{0};
  bool cursor_changed{false};
  bool styles_changed{false};
};

struct GridRenderOptions final {
  PaneRectangle rectangle{};
  bool force_full{false};
  bool focused{false};
  bool project_cursor{false};
  bool opaque{true};
};

struct GridRenderResult final {
  std::size_t bytes{0};
  std::size_t rows{0};
  bool cursor_visible{false};
  bool full{false};
};

// Retained, native terminal-style presentation. Updates may allocate and are transactional;
// rendering is allocation-free and only visits damaged rows unless a repair is requested.
class Grid final {
public:
  [[nodiscard]] static auto create(std::uint16_t columns, std::uint16_t rows) noexcept
      -> std::expected<Grid, GridError>;

  Grid(const Grid&) = delete;
  auto operator=(const Grid&) -> Grid& = delete;
  Grid(Grid&&) noexcept = default;
  auto operator=(Grid&&) noexcept -> Grid& = default;
  ~Grid() = default;

  [[nodiscard]] auto
  apply(GridPatch patch,
        std::size_t retained_bytes_max = limits::surface_retained_bytes_max) noexcept
      -> std::expected<GridApplyResult, GridError>;
  [[nodiscard]] auto resized(std::uint16_t columns, std::uint16_t rows) const noexcept
      -> std::expected<Grid, GridError>;
  [[nodiscard]] auto render_ansi(std::span<std::byte> output, GridRenderOptions options) noexcept
      -> std::expected<GridRenderResult, GridError>;

  // Cursor projection never renders or acknowledges row content, including hidden damage.
  [[nodiscard]] auto render_cursor_ansi(std::span<std::byte> output, PaneRectangle rectangle,
                                        bool unobscured = true) noexcept
      -> std::expected<GridRenderResult, GridError>;
  [[nodiscard]] auto cursor() const noexcept -> GridCursor { return cursor_; }
  [[nodiscard]] auto paints_cell(std::uint16_t column, std::uint16_t row) const noexcept -> bool;
  // Borrowed row coverage for composition; invalidated by any Grid update.
  [[nodiscard]] auto row_runs(std::uint16_t row) const noexcept -> std::span<const GridRun>;

  void invalidate_render_state() noexcept;

  [[nodiscard]] auto columns() const noexcept -> std::uint16_t { return columns_; }
  [[nodiscard]] auto rows() const noexcept -> std::uint16_t { return rows_count_; }
  [[nodiscard]] auto generation() const noexcept -> std::uint64_t { return generation_; }
  [[nodiscard]] auto retained_bytes() const noexcept -> std::size_t { return retained_bytes_; }
  [[nodiscard]] auto damaged() const noexcept -> bool;

private:
  struct Row final {
    std::vector<GridRun> runs;
    std::uint64_t generation{1};
    std::uint64_t presented_generation{0};
  };

  Grid(std::uint16_t columns, std::uint16_t rows, std::vector<Row> storage,
       std::vector<GridStyle> styles) noexcept;

  std::vector<Row> rows_;
  std::vector<GridStyle> styles_;
  GridCursor cursor_;
  std::uint64_t generation_{1};
  std::uint64_t cursor_generation_{1};
  std::uint64_t presented_cursor_generation_{0};
  std::size_t retained_bytes_{0};
  std::uint16_t columns_{0};
  std::uint16_t rows_count_{0};
};

} // namespace frame::render

#endif // FRAME_RENDER_GRID_HPP
