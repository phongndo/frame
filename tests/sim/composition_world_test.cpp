#include "environment.hpp"
#include "random.hpp"
#include "trace.hpp"

#include "core/layout.hpp"
#include "frame/terminal/terminal.hpp"
#include "render/pane_composition.hpp"
#include "render/status_line.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frame::test::sim {
namespace {

constexpr std::size_t composition_panes_max = 6;
constexpr std::size_t composition_operations_default = 96;
constexpr std::size_t composition_frame_bytes_max = std::size_t{512} * 1'024U;

[[nodiscard]] auto create_terminal(const std::uint16_t columns, const std::uint16_t rows)
    -> vt::Terminal {
  vt::TerminalOptions options;
  options.size = {.columns = columns, .rows = rows};
  options.scrollback_lines_max = 128;
  auto terminal = vt::Terminal::create(options);
  if (!terminal.has_value()) {
    std::abort();
  }
  return std::move(*terminal);
}

void write_text(vt::Terminal& terminal, const std::string_view text) {
  terminal.write(std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto screen_text(vt::Terminal& terminal) -> std::optional<std::string> {
  std::array<std::byte, std::size_t{128} * 1'024U> bytes{};
  const auto size =
      terminal.format_visible_tail(vt::ScreenFormat::plain, terminal.size().rows, bytes, false);
  if (!size.has_value()) {
    return std::nullopt;
  }
  // Bounded formatter bytes are copied only for deterministic comparison and diagnostics.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return std::string(reinterpret_cast<const char*>(bytes.data()), *size);
}

class CompositionWorld final {
public:
  CompositionWorld() : layout_(PaneId::from_parts(0, 1)), outer_(create_terminal(40, 12)) {
    generations_.front() = 1;
    auto& initial = terminals_.front().emplace(create_terminal(viewport_.columns, content_rows()));
    write_text(initial, "P0");
    synchronize_geometry();
  }

  [[nodiscard]] auto apply(Random& random, const std::size_t operation_index)
      -> std::optional<std::string> {
    switch (random.index(8)) {
    case 0:
      last_operation_ = "write";
      write(random);
      break;
    case 1:
      last_operation_ = "split";
      split(random);
      break;
    case 2:
      last_operation_ = "remove";
      remove(random);
      break;
    case 3:
      last_operation_ = "swap";
      swap_panes(random);
      break;
    case 4:
      last_operation_ = "resize-viewport";
      resize_viewport(random);
      break;
    case 5:
      last_operation_ = "focus";
      focus(random);
      break;
    case 6:
      last_operation_ = "write";
      write(random);
      break;
    case 7:
      last_operation_ = "toggle-status";
      toggle_status();
      break;
    default:
      return std::string{"unknown composition operation"};
    }
    if (!layout_.valid()) {
      return "operation " + std::to_string(operation_index) + " (" + std::string(last_operation_) +
             "): layout became invalid";
    }
    if (const auto error = compose_and_compare(); error.has_value()) {
      return "operation " + std::to_string(operation_index) + " (" + std::string(last_operation_) +
             "): " + *error;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto state_hash() -> std::uint64_t {
    constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    auto hash = offset;
    const auto text = screen_text(outer_).value_or("<format-error>");
    for (const auto character : text) {
      hash = (hash ^ static_cast<std::uint8_t>(character)) * prime;
    }
    hash = (hash ^ viewport_.columns) * prime;
    hash = (hash ^ viewport_.rows) * prime;
    hash = (hash ^ focused_.slot()) * prime;
    hash = (hash ^ focused_.generation()) * prime;
    return hash;
  }

private:
  [[nodiscard]] auto content_rows() const noexcept -> std::uint16_t {
    return static_cast<std::uint16_t>(viewport_.rows - (status_enabled_ ? 1U : 0U));
  }

  [[nodiscard]] auto content_viewport() const noexcept -> PaneRectangle {
    return {.columns = viewport_.columns, .rows = content_rows()};
  }

  [[nodiscard]] auto live_panes() const -> std::vector<PaneId> {
    std::vector<PaneId> panes;
    panes.reserve(composition_panes_max);
    for (std::size_t slot = 0; slot < terminals_.size(); ++slot) {
      if (terminals_.at(slot).has_value()) {
        panes.emplace_back(
            PaneId::from_parts(static_cast<std::uint32_t>(slot), generations_.at(slot)));
      }
    }
    return panes;
  }

  [[nodiscard]] auto random_pane(Random& random) const -> PaneId {
    const auto panes = live_panes();
    return panes.at(random.index(panes.size()));
  }

  [[nodiscard]] auto terminal_for(const PaneId pane) -> vt::Terminal* {
    if (!pane.is_valid() || pane.slot() >= terminals_.size() ||
        generations_.at(pane.slot()) != pane.generation()) {
      return nullptr;
    }
    auto& terminal = terminals_.at(pane.slot());
    return terminal.has_value() ? &*terminal : nullptr;
  }

  void write(Random& random) {
    auto* const terminal = terminal_for(random_pane(random));
    if (terminal == nullptr) {
      integrity_failed_ = true;
      return;
    }
    constexpr std::array payloads{
        std::string_view{"x"},
        std::string_view{"\r\nline"},
        std::string_view{"\x1B[1;3"
                         "1mR\x1B[0m"},
        std::string_view{"e\xCC\x81"},
        std::string_view{"abcdefghijklmnopqrstuvwxyz"},
    };
    write_text(*terminal, payloads.at(random.index(payloads.size())));
  }

  void split(Random& random) {
    const auto panes = live_panes();
    if (panes.size() == terminals_.size()) {
      return;
    }
    const auto free = static_cast<std::size_t>(std::distance(
        terminals_.begin(), std::ranges::find_if(terminals_, [](const auto& terminal) {
          return !terminal.has_value();
        })));
    auto& generation = generations_.at(free);
    generation = generation == std::numeric_limits<std::uint32_t>::max() ? 1U : generation + 1U;
    const auto added = PaneId::from_parts(static_cast<std::uint32_t>(free), generation);
    const auto source = panes.at(random.index(panes.size()));
    const auto axis = random.boolean() ? core::SplitAxis::left_right : core::SplitAxis::top_bottom;
    auto candidate = layout_;
    if (!candidate.split(source, added, axis) ||
        !candidate.project(content_viewport()).has_value()) {
      return;
    }
    layout_ = candidate;
    auto& terminal = terminals_.at(free).emplace(create_terminal(1, 1));
    const std::array marker{'P', static_cast<char>('0' + static_cast<int>(free))};
    write_text(terminal, std::string_view(marker.data(), marker.size()));
    focused_ = added;
    geometry_changed_ = true;
    synchronize_geometry();
  }

  void remove(Random& random) {
    const auto panes = live_panes();
    if (panes.size() <= 1) {
      return;
    }
    const auto removed = panes.at(random.index(panes.size()));
    const auto next = layout_.remove(removed);
    if (!next.has_value()) {
      integrity_failed_ = true;
      return;
    }
    terminals_.at(removed.slot()).reset();
    if (focused_ == removed) {
      focused_ = *next;
    }
    geometry_changed_ = true;
    synchronize_geometry();
  }

  void swap_panes(Random& random) {
    const auto panes = live_panes();
    if (panes.size() < 2) {
      return;
    }
    const auto first_index = random.index(panes.size());
    auto second_index = random.index(panes.size() - 1U);
    if (second_index >= first_index) {
      ++second_index;
    }
    if (layout_.swap(panes.at(first_index), panes.at(second_index))) {
      geometry_changed_ = true;
      synchronize_geometry();
    }
  }

  void resize_viewport(Random& random) {
    const render::Viewport requested{
        .columns = random.between(12, 72),
        .rows = random.between(4, 20),
    };
    const auto requested_content_rows =
        static_cast<std::uint16_t>(requested.rows - (status_enabled_ ? 1U : 0U));
    if (!layout_.project({.columns = requested.columns, .rows = requested_content_rows})
             .has_value()) {
      return;
    }
    viewport_ = requested;
    const vt::TerminalSize outer_size{.columns = viewport_.columns, .rows = viewport_.rows};
    if (!outer_.resize(outer_size).has_value()) {
      integrity_failed_ = true;
      return;
    }
    geometry_changed_ = true;
    synchronize_geometry();
  }

  void focus(Random& random) { focused_ = random_pane(random); }

  void toggle_status() {
    const auto requested_rows = static_cast<std::uint16_t>(
        viewport_.rows - (status_enabled_ ? std::uint16_t{0} : std::uint16_t{1}));
    if (!layout_.project({.columns = viewport_.columns, .rows = requested_rows}).has_value()) {
      return;
    }
    status_enabled_ = !status_enabled_;
    geometry_changed_ = true;
    synchronize_geometry();
  }

  void synchronize_geometry() {
    const auto projection = layout_.project(content_viewport());
    if (!projection.has_value()) {
      integrity_failed_ = true;
      return;
    }
    for (const auto pane : live_panes()) {
      const auto rectangle = projection->rectangle(pane);
      auto* const terminal = terminal_for(pane);
      if (!rectangle.has_value() || terminal == nullptr ||
          !terminal->resize({.columns = rectangle->columns, .rows = rectangle->rows}).has_value()) {
        integrity_failed_ = true;
        return;
      }
    }
  }

  [[nodiscard]] auto surfaces()
      -> std::optional<std::array<render::PaneSurface, composition_panes_max>> {
    const auto projection = layout_.project(content_viewport());
    if (!projection.has_value()) {
      return std::nullopt;
    }
    std::array<render::PaneSurface, composition_panes_max> result{};
    std::size_t count = 0;
    for (const auto pane : live_panes()) {
      const auto rectangle = projection->rectangle(pane);
      auto* const terminal = terminal_for(pane);
      if (!rectangle.has_value() || terminal == nullptr) {
        return std::nullopt;
      }
      result.at(count) = {
          .terminal = terminal,
          .rectangle = *rectangle,
          .focused = pane == focused_,
          .border_right = static_cast<std::uint32_t>(rectangle->column) + rectangle->columns <
                          viewport_.columns,
          .border_bottom =
              static_cast<std::uint32_t>(rectangle->row) + rectangle->rows < content_rows(),
      };
      ++count;
    }
    surface_count_ = count;
    return result;
  }

  [[nodiscard]] auto status_line() const noexcept -> render::StatusLine {
    if (!status_enabled_) {
      return {};
    }
    return {
        .session_name = "sim",
        .tabs = status_tabs_,
        .prompt_target = render::StatusPromptTarget::none,
        .prompt_feedback = render::StatusPromptFeedback::none,
        .prompt_value = {},
        .input_context = " TEST ",
        .prompt_cursor = 0,
        .dirty = geometry_changed_,
    };
  }

  [[nodiscard]] auto compose_and_compare() -> std::optional<std::string> {
    if (integrity_failed_) {
      return std::string{"composition world integrity failed before rendering"};
    }
    const auto resolved = surfaces();
    if (!resolved.has_value()) {
      return std::string{"failed to resolve generated pane surfaces"};
    }
    const auto pane_span = std::span(*resolved).first(surface_count_);
    const auto incremental = render::compose_frame(pane_span, viewport_, incremental_frame_,
                                                   geometry_changed_, status_line());
    if (!incremental.has_value()) {
      return "incremental composition failed with error " +
             std::to_string(static_cast<unsigned>(incremental.error()));
    }
    outer_.write(std::span(incremental_frame_).first(incremental->bytes));

    for (const auto pane : live_panes()) {
      terminal_for(pane)->invalidate_ansi_render_state();
    }
    const auto reference =
        render::compose_frame(pane_span, viewport_, reference_frame_, true, status_line());
    if (!reference.has_value()) {
      return std::string{"full reference composition failed"};
    }
    auto expected = create_terminal(viewport_.columns, viewport_.rows);
    expected.write(std::span(reference_frame_).first(reference->bytes));
    const auto actual_text = screen_text(outer_);
    const auto expected_text = screen_text(expected);
    if (!actual_text.has_value() || !expected_text.has_value()) {
      return std::string{"failed to format composed outer terminals"};
    }
    if (*actual_text != *expected_text) {
      return "incremental/full multi-pane projections diverged: incremental=[" + *actual_text +
             "] full=[" + *expected_text + "]";
    }
    geometry_changed_ = false;
    return std::nullopt;
  }

  core::PaneLayout layout_;
  std::array<std::optional<vt::Terminal>, composition_panes_max> terminals_{};
  std::array<std::uint32_t, composition_panes_max> generations_{};
  vt::Terminal outer_;
  render::Viewport viewport_{.columns = 40, .rows = 12};
  PaneId focused_{PaneId::from_parts(0, 1)};
  std::array<render::StatusTab, 1> status_tabs_{
      render::StatusTab{.number = 1, .title = "world", .active = true}};
  std::array<std::byte, composition_frame_bytes_max> incremental_frame_{};
  std::array<std::byte, composition_frame_bytes_max> reference_frame_{};
  std::size_t surface_count_{0};
  bool status_enabled_{false};
  bool geometry_changed_{true};
  bool integrity_failed_{false};
  std::string_view last_operation_{"initial"};
};

[[nodiscard]] auto run_composition_world(const std::uint64_t seed, const std::size_t operations,
                                         std::uint64_t* const hash = nullptr)
    -> testing::AssertionResult {
  Random random(seed);
  auto world = std::make_unique<CompositionWorld>();
  for (std::size_t index = 0; index < operations; ++index) {
    if (const auto error = world->apply(random, index); error.has_value()) {
      return testing::AssertionFailure()
             << *error << "\nreplay: FRAME_COMPOSITION_SIM_SEED=" << seed
             << " FRAME_COMPOSITION_SIM_OPERATIONS=" << operations << " ./test sim";
    }
  }
  if (hash != nullptr) {
    *hash = world->state_hash();
  }
  return testing::AssertionSuccess();
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(CompositionSimulationTest, GeneratedMultiPaneIncrementalProjectionMatchesFullReplay) {
  constexpr std::array seeds{0ULL, 1ULL, 0xC0FFEEULL, 0x51A7E123ULL};
  std::uint64_t selected_seed = 0;
  std::uint64_t selected_operations = composition_operations_default;
  const bool configured = std::getenv("FRAME_COMPOSITION_SIM_SEED") != nullptr;
  ASSERT_TRUE(environment_u64("FRAME_COMPOSITION_SIM_SEED", selected_seed));
  ASSERT_TRUE(environment_u64("FRAME_COMPOSITION_SIM_OPERATIONS", selected_operations));
  ASSERT_GT(selected_operations, 0U);
  ASSERT_LE(selected_operations, trace_operations_max);
  if (configured) {
    ASSERT_TRUE(run_composition_world(selected_seed, selected_operations));
    return;
  }
  for (const auto seed : seeds) {
    ASSERT_TRUE(run_composition_world(seed, selected_operations));
  }
}

TEST(CompositionSimulationTest, SameSeedReachesTheSameOuterTerminalState) {
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  ASSERT_TRUE(run_composition_world(0xC011A805EULL, 64, &first));
  ASSERT_TRUE(run_composition_world(0xC011A805EULL, 64, &second));
  EXPECT_EQ(first, second);
}

} // namespace
} // namespace frame::test::sim
