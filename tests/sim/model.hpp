#ifndef FRAME_TESTS_SIM_MODEL_HPP
#define FRAME_TESTS_SIM_MODEL_HPP

#include "core/layout.hpp"
#include "core/session.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace frame::test::sim {

struct MinimumExtent final {
  std::uint16_t columns{1};
  std::uint16_t rows{1};
};

struct ModelDivider final {
  PaneId first;
  PaneId second;
  core::SplitAxis axis{core::SplitAxis::left_right};
};

[[nodiscard]] constexpr auto id_code(const auto id) noexcept -> std::uint64_t {
  return id.is_valid() ? (static_cast<std::uint64_t>(id.generation()) << 32U) | id.slot() : 0;
}

[[nodiscard]] constexpr auto hash_mix(std::uint64_t hash, const std::uint64_t value) noexcept
    -> std::uint64_t {
  hash ^= value;
  hash *= 1'099'511'628'211ULL;
  return hash;
}

// This model knows only topology. It intentionally does not duplicate PaneLayout's fixed-point
// ratios or projection algorithm.
class LayoutModel final {
public:
  explicit LayoutModel(const PaneId first) : root_(std::make_unique<Node>(first)) {}

  [[nodiscard]] auto contains(const PaneId pane) const noexcept -> bool {
    return find_leaf(root_.get(), pane) != nullptr;
  }

  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return size_; }

  [[nodiscard]] auto split(const PaneId source, const PaneId added, const core::SplitAxis axis)
      -> bool {
    if (!source.is_valid() || !added.is_valid() || source == added || contains(added)) {
      return false;
    }
    const auto depth = depth_of(root_.get(), source, 0);
    if (!depth.has_value() || *depth + 1U >= limits::layout_depth_hard_max) {
      return false;
    }
    auto* const leaf = find_leaf(root_.get(), source);
    if (leaf == nullptr) {
      return false;
    }
    leaf->axis = axis;
    leaf->first = std::make_unique<Node>(source);
    leaf->second = std::make_unique<Node>(added);
    leaf->pane = {};
    ++size_;
    return true;
  }

  [[nodiscard]] auto remove(const PaneId pane) -> std::optional<PaneId> {
    if (size_ <= 1U || !pane.is_valid()) {
      return std::nullopt;
    }
    PaneId focus;
    if (!remove_from(root_, pane, focus)) {
      return std::nullopt;
    }
    --size_;
    return focus;
  }

  [[nodiscard]] auto swap(const PaneId first, const PaneId second) noexcept -> bool {
    if (!first.is_valid() || !second.is_valid() || first == second) {
      return false;
    }
    auto* const first_leaf_node = find_leaf(root_.get(), first);
    auto* const second_leaf_node = find_leaf(root_.get(), second);
    if (first_leaf_node == nullptr || second_leaf_node == nullptr) {
      return false;
    }
    std::swap(first_leaf_node->pane, second_leaf_node->pane);
    return true;
  }

  [[nodiscard]] auto minimum() const noexcept -> MinimumExtent { return minimum_of(*root_); }

  [[nodiscard]] auto panes() const -> std::vector<PaneId> {
    std::vector<PaneId> result;
    result.reserve(size_);
    collect_panes(*root_, result);
    return result;
  }

  [[nodiscard]] auto dividers() const -> std::vector<ModelDivider> {
    std::vector<ModelDivider> result;
    result.reserve(size_ > 0 ? size_ - 1U : 0U);
    collect_dividers(*root_, result);
    return result;
  }

  [[nodiscard]] auto matches(const core::LayoutSnapshot& snapshot) const noexcept -> bool {
    if (snapshot.size != (size_ * 2U) - 1U || snapshot.size > snapshot.nodes.size()) {
      return false;
    }
    std::array<bool, core::pane_layout_nodes_max> visited{};
    if (!matches_node(*root_, snapshot, 0, visited)) {
      return false;
    }
    return std::count(visited.begin(), visited.end(), true) ==
           static_cast<std::ptrdiff_t>(snapshot.size);
  }

  [[nodiscard]] auto hash() const noexcept -> std::uint64_t {
    return hash_node(*root_, 14'695'981'039'346'656'037ULL);
  }

private:
  struct Node final {
    explicit Node(const PaneId assigned) noexcept : pane(assigned) {}

    [[nodiscard]] auto leaf() const noexcept -> bool {
      return first == nullptr && second == nullptr;
    }

    PaneId pane;
    core::SplitAxis axis{core::SplitAxis::left_right};
    std::unique_ptr<Node> first;
    std::unique_ptr<Node> second;
  };

  [[nodiscard]] static auto find_leaf(Node* const node, const PaneId pane) noexcept -> Node* {
    if (node == nullptr) {
      return nullptr;
    }
    if (node->leaf()) {
      return node->pane == pane ? node : nullptr;
    }
    if (auto* const found = find_leaf(node->first.get(), pane); found != nullptr) {
      return found;
    }
    return find_leaf(node->second.get(), pane);
  }

  [[nodiscard]] static auto depth_of(const Node* const node, const PaneId pane,
                                     const std::size_t depth) noexcept
      -> std::optional<std::size_t> {
    if (node == nullptr) {
      return std::nullopt;
    }
    if (node->leaf()) {
      return node->pane == pane ? std::optional<std::size_t>{depth} : std::nullopt;
    }
    if (const auto found = depth_of(node->first.get(), pane, depth + 1U); found.has_value()) {
      return found;
    }
    return depth_of(node->second.get(), pane, depth + 1U);
  }

  [[nodiscard]] static auto first_leaf(const Node& node) noexcept -> PaneId {
    return node.leaf() ? node.pane : first_leaf(*node.first);
  }

  [[nodiscard]] static auto remove_from(std::unique_ptr<Node>& node, const PaneId pane,
                                        PaneId& focus) -> bool {
    if (node == nullptr || node->leaf()) {
      return false;
    }
    if (node->first->leaf() && node->first->pane == pane) {
      node = std::move(node->second);
      focus = first_leaf(*node);
      return true;
    }
    if (node->second->leaf() && node->second->pane == pane) {
      node = std::move(node->first);
      focus = first_leaf(*node);
      return true;
    }
    return remove_from(node->first, pane, focus) || remove_from(node->second, pane, focus);
  }

  [[nodiscard]] static auto minimum_of(const Node& node) noexcept -> MinimumExtent {
    if (node.leaf()) {
      return {};
    }
    const auto first = minimum_of(*node.first);
    const auto second = minimum_of(*node.second);
    if (node.axis == core::SplitAxis::left_right) {
      return {.columns = static_cast<std::uint16_t>(first.columns + second.columns + 1U),
              .rows = std::max(first.rows, second.rows)};
    }
    return {.columns = std::max(first.columns, second.columns),
            .rows = static_cast<std::uint16_t>(first.rows + second.rows + 1U)};
  }

  static void collect_panes(const Node& node, std::vector<PaneId>& panes) {
    if (node.leaf()) {
      panes.push_back(node.pane);
      return;
    }
    collect_panes(*node.first, panes);
    collect_panes(*node.second, panes);
  }

  static void collect_dividers(const Node& node, std::vector<ModelDivider>& dividers) {
    if (node.leaf()) {
      return;
    }
    dividers.push_back(
        {.first = first_leaf(*node.first), .second = first_leaf(*node.second), .axis = node.axis});
    collect_dividers(*node.first, dividers);
    collect_dividers(*node.second, dividers);
  }

  [[nodiscard]] static auto
  matches_node(const Node& expected, const core::LayoutSnapshot& snapshot, const std::int16_t index,
               std::array<bool, core::pane_layout_nodes_max>& visited) noexcept -> bool {
    if (index < 0 || std::cmp_greater_equal(index, snapshot.size)) {
      return false;
    }
    const auto node_index = static_cast<std::size_t>(index);
    auto& node_visited = std::span(visited).subspan(node_index, 1).front();
    if (node_visited) {
      return false;
    }
    node_visited = true;
    const auto& actual = std::span(snapshot.nodes).subspan(node_index, 1).front();
    if (expected.leaf()) {
      return actual.leaf && actual.pane == expected.pane && actual.first < 0 && actual.second < 0;
    }
    return !actual.leaf && !actual.pane.is_valid() && actual.axis == expected.axis &&
           matches_node(*expected.first, snapshot, actual.first, visited) &&
           matches_node(*expected.second, snapshot, actual.second, visited);
  }

  [[nodiscard]] static auto hash_node(const Node& node, std::uint64_t hash) noexcept
      -> std::uint64_t {
    hash = hash_mix(hash, node.leaf() ? 1U : 2U);
    if (node.leaf()) {
      return hash_mix(hash, id_code(node.pane));
    }
    hash = hash_mix(hash, static_cast<std::uint64_t>(node.axis));
    hash = hash_node(*node.first, hash);
    return hash_node(*node.second, hash);
  }

  std::unique_ptr<Node> root_;
  std::size_t size_{1};
};

class TabOrderModel final {
public:
  TabOrderModel() { tabs_.reserve(core::tabs_per_session_max); }

  [[nodiscard]] auto append(const TabId tab) -> bool {
    if (!tab.is_valid() || tabs_.size() == core::tabs_per_session_max ||
        std::ranges::find(tabs_, tab) != tabs_.end()) {
      return false;
    }
    tabs_.push_back(tab);
    return true;
  }

  [[nodiscard]] auto erase(const TabId tab) -> bool {
    const auto found = std::ranges::find(tabs_, tab);
    if (found == tabs_.end()) {
      return false;
    }
    tabs_.erase(found);
    return true;
  }

  [[nodiscard]] auto place_before(const TabId moving, const std::optional<TabId> anchor) -> bool {
    const auto source = std::ranges::find(tabs_, moving);
    if (source == tabs_.end() || (anchor.has_value() && *anchor == moving)) {
      return false;
    }
    auto candidate = tabs_;
    candidate.erase(candidate.begin() + std::distance(tabs_.begin(), source));
    auto destination = candidate.end();
    if (anchor.has_value()) {
      destination = std::ranges::find(candidate, *anchor);
      if (destination == candidate.end()) {
        return false;
      }
    }
    candidate.insert(destination, moving);
    if (candidate == tabs_) {
      return false;
    }
    tabs_ = std::move(candidate);
    return true;
  }

  [[nodiscard]] auto position(const TabId tab) const noexcept -> std::optional<std::size_t> {
    const auto found = std::ranges::find(tabs_, tab);
    return found == tabs_.end()
               ? std::nullopt
               : std::optional<std::size_t>{static_cast<std::size_t>(found - tabs_.begin())};
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return tabs_.size(); }
  [[nodiscard]] auto at(const std::size_t index) const noexcept -> TabId {
    return std::span(tabs_).subspan(index, 1).front();
  }
  [[nodiscard]] auto tabs() const -> std::vector<TabId> { return tabs_; }

  [[nodiscard]] auto hash() const noexcept -> std::uint64_t {
    auto hash = 14'695'981'039'346'656'037ULL;
    for (const auto tab : tabs_) {
      hash = hash_mix(hash, id_code(tab));
    }
    return hash;
  }

private:
  std::vector<TabId> tabs_;
};

} // namespace frame::test::sim

#endif // FRAME_TESTS_SIM_MODEL_HPP
