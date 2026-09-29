#ifndef FRAME_CORE_LAYOUT_HPP
#define FRAME_CORE_LAYOUT_HPP

#include "frame/geometry.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace frame::core {

inline constexpr std::size_t pane_layout_panes_max =
    static_cast<std::size_t>(limits::panes_hard_max / limits::sessions_hard_max);
inline constexpr std::size_t pane_layout_nodes_max = (pane_layout_panes_max * 2U) - 1U;

static_assert(pane_layout_panes_max > 0);
static_assert(pane_layout_nodes_max <=
              static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max()));

enum class SplitAxis : std::uint8_t {
  left_right,
  top_bottom,
};

enum class ResizeDirection : std::uint8_t {
  left,
  right,
  up,
  down,
};

enum class LayoutResizeStatus : std::uint8_t {
  applied,
  no_effect,
  unavailable,
  invalid,
};

// Stable generational representatives identify one structural branch without exposing node storage.
// A topology mutation that removes either representative invalidates the handle rather than
// retargeting a captured drag to another divider.
struct LayoutDivider final {
  PaneId first;
  PaneId second;
  SplitAxis axis{SplitAxis::left_right};

  friend constexpr auto operator==(const LayoutDivider&, const LayoutDivider&) noexcept
      -> bool = default;
};

struct LayoutSnapshotNode final {
  PaneId pane;
  std::int16_t first{-1};
  std::int16_t second{-1};
  std::uint16_t ratio{0};
  bool leaf{true};
  SplitAxis axis{SplitAxis::left_right};
};

struct LayoutSnapshot final {
  std::array<LayoutSnapshotNode, pane_layout_nodes_max> nodes{};
  std::size_t size{0};
};

struct LayoutProjection final {
  std::array<PaneRectangle, pane_layout_panes_max> rectangles{};
  std::array<PaneId, pane_layout_panes_max> panes{};
  std::array<bool, pane_layout_panes_max> included{};
  std::size_t pane_count{0};

  [[nodiscard]] auto rectangle(PaneId pane) const noexcept -> std::optional<PaneRectangle>;

  friend constexpr auto operator==(const LayoutProjection&, const LayoutProjection&) noexcept
      -> bool = default;
};

// Fixed-capacity semantic split topology. Nodes are private so every mutation preserves one rooted
// full binary tree and every stored split ratio is valid by construction.
class PaneLayout final {
public:
  explicit PaneLayout(PaneId first_pane) noexcept;

  [[nodiscard]] auto contains(PaneId pane) const noexcept -> bool;
  [[nodiscard]] auto pane_count() const noexcept -> std::size_t;
  [[nodiscard]] auto project(PaneRectangle viewport) const noexcept
      -> std::optional<LayoutProjection>;
  // Dense preorder projection for bounded public introspection. Child indices refer only to this
  // snapshot and never expose private PaneLayout storage slots.
  [[nodiscard]] auto snapshot() const noexcept -> std::optional<LayoutSnapshot>;
  [[nodiscard]] auto divider_at(PaneRectangle viewport, std::uint16_t column,
                                std::uint16_t row) const noexcept -> std::optional<LayoutDivider>;
  [[nodiscard]] auto divider_rectangle(LayoutDivider divider, PaneRectangle viewport) const noexcept
      -> std::optional<PaneRectangle>;

  [[nodiscard]] auto split(PaneId source, PaneId added, SplitAxis axis) noexcept -> bool;
  [[nodiscard]] auto swap(PaneId first, PaneId second) noexcept -> bool;
  [[nodiscard]] auto remove(PaneId pane) noexcept -> std::optional<PaneId>;
  [[nodiscard]] auto resize(PaneId pane, ResizeDirection direction, PaneRectangle viewport,
                            std::uint16_t amount = 1) noexcept -> LayoutResizeStatus;
  [[nodiscard]] auto resize_divider(LayoutDivider divider, std::uint16_t coordinate,
                                    PaneRectangle viewport) noexcept -> LayoutResizeStatus;

  [[nodiscard]] auto valid() const noexcept -> bool;

  friend constexpr auto operator==(const PaneLayout&, const PaneLayout&) noexcept -> bool = default;

private:
  class SplitRatio final {
  public:
    constexpr SplitRatio() noexcept = default;

    [[nodiscard]] static auto from_extents(std::uint16_t first, std::uint16_t second) noexcept
        -> SplitRatio;
    [[nodiscard]] auto first_extent(std::uint16_t available) const noexcept -> std::uint16_t;
    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
      return first_share_ > 0 && first_share_ < scale;
    }
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint16_t { return first_share_; }

    friend constexpr auto operator==(const SplitRatio&, const SplitRatio&) noexcept
        -> bool = default;

  private:
    static constexpr std::uint32_t scale = 65'535;
    explicit constexpr SplitRatio(const std::uint16_t first_share) noexcept
        : first_share_(first_share) {}

    // The first child receives the odd cell for the default equal split.
    std::uint16_t first_share_{32'768};
  };

  static_assert(sizeof(SplitRatio) == sizeof(std::uint16_t));

  struct Node final {
    PaneId pane;
    std::int16_t parent{-1};
    std::int16_t first{-1};
    std::int16_t second{-1};
    SplitRatio ratio;
    bool active{false};
    bool leaf{true};
    SplitAxis axis{SplitAxis::left_right};

    friend constexpr auto operator==(const Node&, const Node&) noexcept -> bool = default;
  };

  struct MinimumExtent final {
    std::uint16_t columns{0};
    std::uint16_t rows{0};
  };

  enum class ResizeValueKind : std::uint8_t {
    extent_delta,
    divider_coordinate,
  };

  using MinimumExtents = std::array<MinimumExtent, pane_layout_nodes_max>;
  using NodeRectangles = std::array<PaneRectangle, pane_layout_nodes_max>;

  [[nodiscard]] auto node_for_pane(PaneId pane) const noexcept -> std::optional<std::size_t>;
  [[nodiscard]] auto node_for_divider(LayoutDivider divider) const noexcept
      -> std::optional<std::size_t>;
  [[nodiscard]] auto first_leaf(std::size_t node_index) const noexcept -> PaneId;
  [[nodiscard]] auto compute_minimum(std::size_t node_index, std::size_t depth,
                                     MinimumExtents& extents) const noexcept -> bool;
  [[nodiscard]] auto project_node(std::size_t node_index, PaneRectangle rectangle,
                                  std::size_t depth, const MinimumExtents& extents,
                                  LayoutProjection& projection,
                                  NodeRectangles* node_rectangles) const noexcept -> bool;
  [[nodiscard]] auto project_internal(PaneRectangle viewport, LayoutProjection& projection,
                                      NodeRectangles* node_rectangles,
                                      MinimumExtents* minimum_extents) const noexcept -> bool;
  [[nodiscard]] auto resize_node(std::size_t node_index, PaneRectangle viewport, std::int32_t value,
                                 ResizeValueKind value_kind) noexcept -> LayoutResizeStatus;
  [[nodiscard]] auto
  validate_node(std::size_t node_index, std::int16_t parent, std::size_t depth,
                std::array<bool, pane_layout_nodes_max>& visited,
                std::array<bool, pane_layout_panes_max>& pane_slots) const noexcept -> bool;

  std::array<Node, pane_layout_nodes_max> nodes_{};
};

static_assert(std::is_trivially_copyable_v<PaneLayout>);

} // namespace frame::core

#endif // FRAME_CORE_LAYOUT_HPP
