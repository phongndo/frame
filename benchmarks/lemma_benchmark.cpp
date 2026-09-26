#include "core/layout.hpp"
#include "extension/protocol.hpp"
#include "extension/runtime.hpp"
#include "input/input_router.hpp"
#include "lemma/command.hpp"
#include "lemma/geometry.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "platform/pty.hpp"
#include "protocol/attachment.hpp"
#include "render/grid.hpp"
#include "render/pane_composition.hpp"
#include "render/scene.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <util.h>
#else
#include <pty.h>
#endif

namespace lemma {
namespace {

struct CommandBenchmarkContext final {
  std::uint64_t calls{0};
};

[[nodiscard]] auto execute_benchmark_command(void* const context,
                                             const Command& /*command*/) noexcept -> CommandResult {
  ++static_cast<CommandBenchmarkContext*>(context)->calls;
  return {.status = CommandStatus::applied};
}

void benchmark_command_dispatch(benchmark::State& state) {
  CommandBenchmarkContext context;
  const CommandDispatcher dispatcher(&execute_benchmark_command, &context);
  const Command command{.kind = CommandKind::focus_next, .origin = CommandOrigin::keymap};
  for ([[maybe_unused]] const auto iteration : state) {
    auto status = dispatcher.dispatch(command).status;
    benchmark::DoNotOptimize(status);
  }
  benchmark::DoNotOptimize(context.calls);
}

void benchmark_input_router_unbound_run(benchmark::State& state) {
  input::InputRouter router(input::default_input_map());
  constexpr std::array bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'}};
  for ([[maybe_unused]] const auto iteration : state) {
    auto checkpoint = router.legacy_route_requires_checkpoint();
    auto routed = router.route_legacy(bytes, bytes.size());
    benchmark::DoNotOptimize(checkpoint);
    benchmark::DoNotOptimize(routed.consumed);
  }
}

void benchmark_input_router_context_command(benchmark::State& state) {
  input::InputRouter router(input::default_input_map());
  constexpr std::array enter{std::byte{0x02}, std::byte{'m'}};
  static_cast<void>(router.route_legacy(enter, enter.size()));
  static_cast<void>(router.route_legacy(std::span(enter).subspan(1), 1));
  constexpr std::array resize{std::byte{'h'}};
  for ([[maybe_unused]] const auto iteration : state) {
    auto checkpoint = router.legacy_route_requires_checkpoint();
    auto routed = router.route_legacy(resize, 1);
    benchmark::DoNotOptimize(checkpoint);
    benchmark::DoNotOptimize(routed.consumed);
  }
}

void benchmark_input_router_typed_forward_cycle(benchmark::State& state) {
  input::InputRouter router(input::default_input_map());
  constexpr input::KeyEvent press{.action = input::KeyAction::press,
                                  .key = input::PhysicalKey::h,
                                  .modifiers = 0,
                                  .unshifted_codepoint = 'h',
                                  .text = {}};
  constexpr input::KeyEvent release{.action = input::KeyAction::release,
                                    .key = input::PhysicalKey::h,
                                    .modifiers = 0,
                                    .unshifted_codepoint = 'h',
                                    .text = {}};
  for ([[maybe_unused]] const auto iteration : state) {
    auto press_result = router.route_key(press).effect.index();
    auto release_result = router.route_key(release).effect.index();
    benchmark::DoNotOptimize(press_result);
    benchmark::DoNotOptimize(release_result);
  }
}

void benchmark_input_router_typed_context_repeat(benchmark::State& state) {
  input::InputRouter router(input::default_input_map());
  constexpr std::array enter{std::byte{0x02}, std::byte{'m'}};
  static_cast<void>(router.route_legacy(enter, enter.size()));
  static_cast<void>(router.route_legacy(std::span(enter).subspan(1), 1));
  constexpr input::KeyEvent press{.action = input::KeyAction::press,
                                  .key = input::PhysicalKey::h,
                                  .modifiers = 0,
                                  .unshifted_codepoint = 'h',
                                  .text = {}};
  constexpr input::KeyEvent repeat{.action = input::KeyAction::repeat,
                                   .key = input::PhysicalKey::h,
                                   .modifiers = 0,
                                   .unshifted_codepoint = 'h',
                                   .text = {}};
  static_cast<void>(router.route_key(press));
  for ([[maybe_unused]] const auto iteration : state) {
    auto routed = router.route_key(repeat).effect.index();
    benchmark::DoNotOptimize(routed);
  }
}

[[nodiscard]] auto benchmark_layout(const std::size_t requested_panes) -> core::PaneLayout {
  core::PaneLayout layout(PaneId::from_parts(0, 1));
  std::size_t panes = 1;
  std::size_t level = 0;
  while (panes < requested_panes) {
    const auto level_panes = panes;
    for (std::size_t source = 0; source < level_panes && panes < requested_panes; ++source) {
      const auto axis = level % 2U == 0 ? core::SplitAxis::left_right : core::SplitAxis::top_bottom;
      const auto source_id = PaneId::from_parts(static_cast<std::uint32_t>(source), 1);
      const auto added_id = PaneId::from_parts(static_cast<std::uint32_t>(panes), 1);
      if (!layout.split(source_id, added_id, axis)) {
        break;
      }
      ++panes;
    }
    ++level;
  }
  return layout;
}

[[nodiscard]] auto benchmark_worst_depth_layout() -> core::PaneLayout {
  core::PaneLayout layout(PaneId::from_parts(0, 1));
  for (std::uint32_t slot = 1; slot < core::pane_layout_panes_max; ++slot) {
    if (!layout.split(PaneId::from_parts(0, 1), PaneId::from_parts(slot, 1),
                      core::SplitAxis::left_right)) {
      break;
    }
  }
  return layout;
}

void benchmark_layout_projection(benchmark::State& state) {
  const auto panes = static_cast<std::size_t>(state.range(0));
  const auto layout = benchmark_layout(panes);
  if (layout.pane_count() != panes) {
    state.SkipWithError("failed to build benchmark layout");
    return;
  }
  constexpr PaneRectangle viewport{.columns = 240, .rows = 80};
  for ([[maybe_unused]] const auto iteration : state) {
    const auto projection = layout.project(viewport);
    if (!projection.has_value()) {
      state.SkipWithError("failed to project benchmark layout");
      return;
    }
    auto projected_panes = projection->pane_count;
    benchmark::DoNotOptimize(projected_panes);
  }
}

void benchmark_layout_resize_candidate(benchmark::State& state) {
  const auto layout = benchmark_layout(64);
  constexpr PaneRectangle viewport{.columns = 240, .rows = 80};
  for ([[maybe_unused]] const auto iteration : state) {
    auto candidate = layout;
    const auto status =
        candidate.resize(PaneId::from_parts(0, 1), core::ResizeDirection::right, viewport);
    benchmark::DoNotOptimize(candidate);
    if (status != core::LayoutResizeStatus::applied) {
      state.SkipWithError("failed to resize benchmark layout");
      return;
    }
  }
}

void benchmark_layout_swap_candidate(benchmark::State& state) {
  const auto layout = benchmark_layout(64);
  for ([[maybe_unused]] const auto iteration : state) {
    auto candidate = layout;
    const bool swapped = candidate.swap(PaneId::from_parts(0, 1), PaneId::from_parts(63, 1));
    benchmark::DoNotOptimize(candidate);
    if (!swapped) {
      state.SkipWithError("failed to swap benchmark layout");
      return;
    }
  }
}

void benchmark_layout_divider_hit(benchmark::State& state) {
  const auto layout = benchmark_layout(64);
  constexpr PaneRectangle viewport{.columns = 240, .rows = 80};
  for ([[maybe_unused]] const auto iteration : state) {
    const auto divider = layout.divider_at(viewport, 120, 0);
    if (!divider.has_value()) {
      state.SkipWithError("failed to hit benchmark divider");
      return;
    }
    auto observed = *divider;
    benchmark::DoNotOptimize(observed);
  }
}

void benchmark_layout_divider_resize_candidate(benchmark::State& state) {
  const auto layout = benchmark_layout(64);
  constexpr PaneRectangle viewport{.columns = 240, .rows = 80};
  const auto divider = layout.divider_at(viewport, 120, 0);
  if (!divider.has_value()) {
    state.SkipWithError("failed to hit benchmark divider");
    return;
  }
  for ([[maybe_unused]] const auto iteration : state) {
    auto candidate = layout;
    const auto status = candidate.resize_divider(*divider, 121, viewport);
    auto rectangle = candidate.divider_rectangle(*divider, viewport);
    benchmark::DoNotOptimize(candidate);
    benchmark::DoNotOptimize(rectangle);
    if (status != core::LayoutResizeStatus::applied || !rectangle.has_value()) {
      state.SkipWithError("failed to project resized benchmark divider");
      return;
    }
  }
}

void handle_benchmark_winch(int /*signal*/) noexcept {}

// Process setup and teardown are untimed; the loop measures only foreground-child TIOCSWINSZ.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void benchmark_live_divider_pty_resize(benchmark::State& state) {
  int descriptor = -1;
  const auto child = ::forkpty(&descriptor, nullptr, nullptr, nullptr);
  if (child < 0) {
    state.SkipWithError("failed to create benchmark PTY peer");
    return;
  }
  if (child == 0) {
    struct sigaction action{};
    action.sa_handler = &handle_benchmark_winch;
    static_cast<void>(sigemptyset(&action.sa_mask));
    if (::sigaction(SIGWINCH, &action, nullptr) != 0 || ::write(STDOUT_FILENO, "R", 1) != 1) {
      ::_exit(1);
    }
    while (true) {
      static_cast<void>(::pause());
    }
  }

  char ready = 0;
  ssize_t ready_bytes = 0;
  while (true) {
    ready_bytes = ::read(descriptor, &ready, 1);
    if (ready_bytes >= 0 || errno != EINTR) {
      break;
    }
  }
  if (ready_bytes != 1 || ready != 'R') {
    state.SkipWithError("benchmark PTY peer did not become ready");
  } else {
    bool expanded = false;
    for ([[maybe_unused]] const auto iteration : state) {
      expanded = !expanded;
      if (!platform::resize_pty(descriptor, expanded ? std::uint16_t{40} : std::uint16_t{39},
                                std::uint16_t{23}, 0, 0)) {
        state.SkipWithError("TIOCSWINSZ failed");
        break;
      }
    }
    state.SetItemsProcessed(state.iterations());
  }

  static_cast<void>(::kill(child, SIGKILL));
  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  static_cast<void>(::close(descriptor));
}

// Measures the live path through semantic resize, Ghostty reflow, and full pane composition. PTY
// ioctls, child scheduling, client transport, and outer-terminal rendering remain excluded.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void benchmark_live_divider_resize(benchmark::State& state) {
  const auto pane_count = static_cast<std::size_t>(state.range(0));
  auto layout = benchmark_layout(pane_count);
  constexpr PaneRectangle layout_viewport{.columns = 240, .rows = 80};
  constexpr render::Viewport render_viewport{.columns = 240, .rows = 80};
  auto projection = layout.project(layout_viewport);
  const auto divider = layout.divider_at(layout_viewport, 120, 0);
  if (!projection.has_value() || !divider.has_value() || layout.pane_count() != pane_count) {
    state.SkipWithError("failed to build live-resize layout");
    return;
  }

  std::vector<vt::Terminal> terminals;
  terminals.reserve(pane_count);
  std::vector<render::PaneSurface> panes;
  panes.reserve(pane_count);
  for (std::size_t slot = 0; slot < pane_count; ++slot) {
    const auto id = PaneId::from_parts(static_cast<std::uint32_t>(slot), 1);
    const auto rectangle = projection->rectangle(id);
    if (!rectangle.has_value()) {
      state.SkipWithError("live-resize projection omitted pane");
      return;
    }
    vt::TerminalOptions options;
    options.size = {.columns = rectangle->columns, .rows = rectangle->rows};
    auto terminal = vt::Terminal::create(options);
    if (!terminal.has_value()) {
      state.SkipWithError("failed to create live-resize terminal");
      return;
    }
    terminals.emplace_back(std::move(*terminal));
    panes.push_back({
        .terminal = &terminals.back(),
        .rectangle = *rectangle,
        .focused = slot == 0,
    });
  }
  std::vector<std::byte> frame(std::size_t{1} * 1'024U * 1'024U);
  if (!render::compose_frame(panes, render_viewport, frame, true).has_value()) {
    state.SkipWithError("failed to compose initial live-resize frame");
    return;
  }

  bool expanded = false;
  std::uint64_t output_bytes = 0;
  for ([[maybe_unused]] const auto iteration : state) {
    auto candidate = layout;
    const auto coordinate = expanded ? std::uint16_t{120} : std::uint16_t{121};
    expanded = !expanded;
    if (candidate.resize_divider(*divider, coordinate, layout_viewport) !=
        core::LayoutResizeStatus::applied) {
      state.SkipWithError("failed to resize live candidate");
      return;
    }
    projection = candidate.project(layout_viewport);
    if (!projection.has_value()) {
      state.SkipWithError("failed to project live candidate");
      return;
    }
    for (std::size_t slot = 0; slot < pane_count; ++slot) {
      const auto id = PaneId::from_parts(static_cast<std::uint32_t>(slot), 1);
      const auto rectangle = projection->rectangle(id);
      if (!rectangle.has_value()) {
        state.SkipWithError("resized projection omitted pane");
        return;
      }
      auto& terminal = std::span(terminals).subspan(slot, 1).front();
      const vt::TerminalSize size{.columns = rectangle->columns, .rows = rectangle->rows};
      if (terminal.size() != size && !terminal.resize(size).has_value()) {
        state.SkipWithError("failed to resize live terminal");
        return;
      }
      std::span(panes).subspan(slot, 1).front().rectangle = *rectangle;
    }
    const auto rendered = render::compose_frame(panes, render_viewport, frame, true);
    if (!rendered.has_value()) {
      state.SkipWithError("failed to compose live-resize frame");
      return;
    }
    output_bytes += rendered->bytes;
    layout = candidate;
    benchmark::DoNotOptimize(layout);
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_layout_projection_worst_depth(benchmark::State& state) {
  const auto layout = benchmark_worst_depth_layout();
  constexpr PaneRectangle viewport{.columns = 500, .rows = 200};
  if (layout.pane_count() != core::pane_layout_panes_max) {
    state.SkipWithError("failed to build worst-depth layout");
    return;
  }
  for ([[maybe_unused]] const auto iteration : state) {
    const auto projection = layout.project(viewport);
    if (!projection.has_value()) {
      state.SkipWithError("failed to project worst-depth layout");
      return;
    }
    auto projected_panes = projection->pane_count;
    benchmark::DoNotOptimize(projected_panes);
  }
}

void benchmark_layout_divider_hit_worst_depth(benchmark::State& state) {
  const auto layout = benchmark_worst_depth_layout();
  constexpr PaneRectangle viewport{.columns = 500, .rows = 200};
  constexpr auto deepest_second =
      PaneId::from_parts(static_cast<std::uint32_t>(core::pane_layout_panes_max - 1U), 1);
  std::optional<std::uint16_t> coordinate;
  for (std::uint16_t column = 0; column < viewport.columns; ++column) {
    const auto divider = layout.divider_at(viewport, column, 0);
    if (divider.has_value() && divider->second == deepest_second) {
      coordinate = column;
      break;
    }
  }
  if (!coordinate.has_value()) {
    state.SkipWithError("failed to find deepest benchmark divider");
    return;
  }
  for ([[maybe_unused]] const auto iteration : state) {
    const auto divider = layout.divider_at(viewport, *coordinate, 0);
    if (!divider.has_value() || divider->second != deepest_second) {
      state.SkipWithError("failed to hit deepest benchmark divider");
      return;
    }
    auto observed = *divider;
    benchmark::DoNotOptimize(observed);
  }
}

void benchmark_private_attach_input_codec(benchmark::State& state) {
  constexpr std::array payload{std::byte{'i'}, std::byte{'n'}, std::byte{'p'}, std::byte{'u'},
                               std::byte{'t'}};
  const auto header = protocol::encode_input_header(payload.size(), 2);
  protocol::ClientDecoder decoder;
  if (!decoder.prepare().has_value()) {
    state.SkipWithError("failed to prepare private attach decoder");
    return;
  }
  for ([[maybe_unused]] const auto iteration : state) {
    decoder.reset(2, false);
    auto destination = std::ranges::copy(header, decoder.writable_bytes().begin()).out;
    std::ranges::copy(payload, destination);
    if (!decoder.commit(header.size() + payload.size()).has_value()) {
      state.SkipWithError("failed to commit private attach frame");
      return;
    }
    const auto decoded = decoder.next();
    if (!decoded.has_value() || !decoded->has_value()) {
      state.SkipWithError("failed to decode private attach frame");
      return;
    }
    auto message = **decoded;
    benchmark::DoNotOptimize(message);
    decoder.consume();
  }
  state.counters["wire_overhead_bytes"] = static_cast<double>(protocol::attach_header_bytes);
}

void benchmark_terminal_small_writes(benchmark::State& state) {
  auto result = vt::Terminal::create({});
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  // Keep canonical state equivalent across iterations: this measures parsing a typical small
  // cursor-addressed update rather than eventual history growth.
  constexpr std::string_view input = "\x1B[1;1Hprompt> echo hello\x1B[K";
  const auto bytes = std::as_bytes(std::span(input.data(), input.size()));

  for ([[maybe_unused]] const auto iteration : state) {
    terminal.write(bytes);
    benchmark::ClobberMemory();
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes.size()));
}

void benchmark_terminal_large_writes(benchmark::State& state) {
  auto result = vt::Terminal::create({});
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  // The alternate screen has no scrollback growth, so every iteration parses the same sustained
  // full-screen scrolling workload without eventually changing allocator or pruning behavior.
  constexpr std::string_view alternate_screen = "\x1B[?1049h";
  terminal.write(std::as_bytes(std::span(alternate_screen.data(), alternate_screen.size())));
  std::array<std::byte, std::size_t{64} * 1'024U> input{};
  input.fill(std::byte{'x'});

  for ([[maybe_unused]] const auto iteration : state) {
    terminal.write(input);
    benchmark::ClobberMemory();
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(input.size()));
}

void benchmark_terminal_ansi_damage_frames(benchmark::State& state) {
  auto result = vt::Terminal::create({});
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  constexpr std::string_view input = "changed row with styled \x1B[1;32mcontent\x1B[0m\r\n";
  const auto bytes = std::as_bytes(std::span(input.data(), input.size()));
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  auto initial = terminal.render_ansi(frame, true);
  if (!initial.has_value()) {
    state.SkipWithError("failed to render initial frame");
    return;
  }
  std::uint64_t output_bytes = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    terminal.write(bytes);
    auto rendered = terminal.render_ansi(frame);
    benchmark::DoNotOptimize(rendered);
    if (!rendered.has_value()) {
      state.SkipWithError("failed to render ANSI damage");
      break;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_terminal_ansi_single_row(benchmark::State& state) {
  auto result = vt::Terminal::create({});
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  constexpr std::string_view first = "\x1B[10;1Hfirst styled \x1B[1;32mrow\x1B[0m";
  constexpr std::string_view second = "\x1B[10;1Hsecond styled \x1B[1;34mrow\x1B[0m";
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  auto initial = terminal.render_ansi(frame, true);
  if (!initial.has_value()) {
    state.SkipWithError("failed to render initial frame");
    return;
  }
  std::uint64_t output_bytes = 0;
  bool use_first = false;

  for ([[maybe_unused]] const auto iteration : state) {
    const auto input = use_first ? first : second;
    use_first = !use_first;
    terminal.write(std::as_bytes(std::span(input.data(), input.size())));
    auto rendered = terminal.render_ansi(frame);
    benchmark::DoNotOptimize(rendered);
    if (!rendered.has_value()) {
      state.SkipWithError("failed to render ANSI row");
      break;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_terminal_ansi_clean_frame(benchmark::State& state) {
  auto result = vt::Terminal::create({});
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  constexpr std::string_view contents = "unchanged styled \x1B[1;32mcontent\x1B[0m";
  terminal.write(std::as_bytes(std::span(contents.data(), contents.size())));
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  if (!terminal.render_ansi(frame, true).has_value()) {
    state.SkipWithError("failed to render initial frame");
    return;
  }
  std::uint64_t output_bytes = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    auto rendered = terminal.render_ansi(frame);
    benchmark::DoNotOptimize(rendered);
    if (!rendered.has_value() || rendered->rows != 0) {
      state.SkipWithError("failed to render clean ANSI frame");
      return;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_terminal_ansi_scroll_operations(benchmark::State& state) {
  vt::TerminalOptions options;
  options.size = {.columns = 80, .rows = 24};
  auto result = vt::Terminal::create(options);
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  constexpr std::string_view initial =
      "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\n10\r\n11\r\n12\r\n"
      "13\r\n14\r\n15\r\n16\r\n17\r\n18\r\n19\r\n20\r\n21\r\n22\r\n23\r\n24";
  terminal.write(std::as_bytes(std::span(initial.data(), initial.size())));
  if (!terminal.render_ansi(frame, true).has_value()) {
    state.SkipWithError("failed to render initial frame");
    return;
  }
  std::uint64_t output_bytes = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    constexpr std::string_view line = "\r\nnext scrolling row";
    terminal.write(std::as_bytes(std::span(line.data(), line.size())));
    const auto rendered = terminal.render_ansi(frame);
    if (!rendered.has_value() || rendered->scrolled_rows != 1) {
      state.SkipWithError("failed to encode scroll operation");
      break;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

// Streaming command output: every frame scrolls one full-width line of distinct text. Unlike
// benchmark_terminal_ansi_scroll_operations, rows are dense, so per-cell work is not hidden by
// repeated blank cells.
void benchmark_terminal_ansi_text_scroll_frames(benchmark::State& state) {
  vt::TerminalOptions options;
  options.size = {.columns = 80, .rows = 24};
  auto result = vt::Terminal::create(options);
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  constexpr std::string_view alphabet =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_=+;:,./?!@#$%^&*()[]{}";
  std::string line;
  std::size_t sequence = 0;
  const auto write_line = [&]() {
    line.assign("\r\n");
    for (std::size_t column = 0; column < 79; ++column) {
      line.push_back(alphabet.at((sequence + (column * 7U)) % alphabet.size()));
    }
    ++sequence;
    terminal.write(std::as_bytes(std::span(line.data(), line.size())));
  };
  for (std::size_t row = 0; row < 24; ++row) {
    write_line();
  }
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  if (!terminal.render_ansi(frame, true).has_value()) {
    state.SkipWithError("failed to render initial frame");
    return;
  }
  std::uint64_t output_bytes = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    write_line();
    const auto rendered = terminal.render_ansi(frame);
    if (!rendered.has_value() || rendered->scrolled_rows != 1) {
      state.SkipWithError("failed to encode text scroll");
      break;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

// A focused 80x23 pane above the native status row, as in the interactive-under-output workload:
// the pane does not fill the viewport, so its output cannot use a terminal scroll and each frame
// is a redraw of every row. Argument 0 repeats one line (the workload's background output);
// argument 1 writes distinct lines, which repaint the whole pane.
void benchmark_terminal_pane_output_frames(benchmark::State& state) {
  const bool distinct = state.range(0) != 0;
  vt::TerminalOptions options;
  options.size = {.columns = 80, .rows = 23};
  auto result = vt::Terminal::create(options);
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  constexpr std::string_view alphabet =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_=+;:,./?!@#$%^&*()[]{}";
  std::string line;
  std::size_t sequence = 0;
  const auto write_line = [&]() {
    line.clear();
    for (std::size_t column = 0; column < 79; ++column) {
      line.push_back(alphabet.at((sequence + (column * 7U)) % alphabet.size()));
    }
    line.append("\r\n");
    sequence += distinct ? 1U : 0U;
    terminal.write(std::as_bytes(std::span(line.data(), line.size())));
  };
  for (std::size_t row = 0; row < 23; ++row) {
    write_line();
  }
  const vt::PaneRenderOptions pane{.force_full = false, .focused = true};
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  if (!terminal.render_pane_ansi(frame, {.force_full = true, .focused = true}).has_value()) {
    state.SkipWithError("failed to render initial frame");
    return;
  }
  std::uint64_t output_bytes = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    write_line();
    const auto rendered = terminal.render_pane_ansi(frame, pane);
    if (!rendered.has_value()) {
      state.SkipWithError("failed to render pane output");
      break;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_terminal_viewport_wheel_frames(benchmark::State& state) {
  vt::TerminalOptions options;
  options.size = {.columns = 80, .rows = 23};
  auto result = vt::Terminal::create(options);
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  constexpr std::string_view line =
      "history-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789------\r\n";
  const auto bytes = std::as_bytes(std::span(line.data(), line.size()));
  for (std::size_t row = 0; row < 20'000; ++row) {
    terminal.write(bytes);
  }
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  if (!terminal.render_ansi(frame, true).has_value()) {
    state.SkipWithError("failed to render initial history frame");
    return;
  }
  terminal.scroll_viewport(vt::ViewportScroll::delta, -100);
  std::uint64_t output_bytes = 0;
  bool upward = true;

  for ([[maybe_unused]] const auto iteration : state) {
    terminal.scroll_viewport(vt::ViewportScroll::delta, upward ? -1 : 1);
    upward = !upward;
    auto rendered = terminal.render_ansi(frame, true);
    benchmark::DoNotOptimize(rendered);
    if (!rendered.has_value()) {
      state.SkipWithError("failed to render historical viewport");
      return;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_terminal_visible_capture(benchmark::State& state) {
  const auto rows = static_cast<std::uint16_t>(state.range(0));
  const auto columns = rows >= 200U ? std::uint16_t{500} : std::uint16_t{80};
  vt::TerminalOptions options;
  options.size = {.columns = columns, .rows = rows};
  auto result = vt::Terminal::create(options);
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  constexpr std::string_view contents = "top-row";
  terminal.write(std::as_bytes(std::span(contents.data(), contents.size())));
  std::array<std::byte, std::size_t{256} * 1'024U> output{};
  std::uint64_t output_bytes = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    const auto formatted =
        terminal.format_visible_tail(vt::ScreenFormat::plain, rows, output, false);
    if (!formatted.has_value()) {
      state.SkipWithError("failed to capture visible terminal");
      return;
    }
    output_bytes += *formatted;
  }
  state.counters["capture_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_terminal_multiple_panes(benchmark::State& state) {
  const auto pane_count = static_cast<std::size_t>(state.range(0));
  std::size_t grid_columns = 8;
  if (pane_count == 1) {
    grid_columns = 1;
  } else if (pane_count == 4) {
    grid_columns = 2;
  } else if (pane_count == 16) {
    grid_columns = 4;
  }
  const auto grid_rows = pane_count / grid_columns;
  constexpr std::uint16_t viewport_columns = 240;
  constexpr std::uint16_t viewport_rows = 80;
  const auto pane_columns = static_cast<std::uint16_t>(viewport_columns / grid_columns);
  const auto pane_rows = static_cast<std::uint16_t>(viewport_rows / grid_rows);
  std::vector<vt::Terminal> terminals;
  terminals.reserve(pane_count);
  vt::TerminalOptions options;
  options.size = {.columns = pane_columns, .rows = pane_rows};
  for (std::size_t pane = 0; pane < pane_count; ++pane) {
    auto result = vt::Terminal::create(options);
    if (!result.has_value()) {
      state.SkipWithError("failed to create terminal");
      return;
    }
    terminals.emplace_back(std::move(*result));
  }

  std::vector<render::PaneSurface> panes;
  panes.reserve(pane_count);
  for (std::size_t pane = 0; pane < pane_count; ++pane) {
    panes.push_back({
        .terminal = &std::span(terminals).subspan(pane, 1).front(),
        .rectangle =
            {
                .column = static_cast<std::uint16_t>((pane % grid_columns) * pane_columns),
                .row = static_cast<std::uint16_t>((pane / grid_columns) * pane_rows),
                .columns = pane_columns,
                .rows = pane_rows,
            },
        .focused = pane == 0,
    });
  }
  const render::Viewport viewport{.columns = viewport_columns, .rows = viewport_rows};
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  constexpr std::string_view first = "\x1B[1;1HA";
  constexpr std::string_view second = "\x1B[1;1HB";
  if (!render::compose_frame(panes, viewport, frame, true).has_value()) {
    state.SkipWithError("failed to compose initial frame");
    return;
  }
  std::uint64_t output_bytes = 0;
  bool use_first = false;

  for ([[maybe_unused]] const auto iteration : state) {
    const auto input = use_first ? first : second;
    use_first = !use_first;
    const auto bytes = std::as_bytes(std::span(input.data(), input.size()));
    for (auto& terminal : terminals) {
      terminal.write(bytes);
    }
    auto rendered = render::compose_frame(panes, viewport, frame, false);
    benchmark::DoNotOptimize(rendered);
    if (!rendered.has_value()) {
      state.SkipWithError("failed to compose panes");
      return;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_idle_extension_runtime(benchmark::State& state) {
  auto runtime = std::make_unique<extension::Runtime>();
  std::vector<int> writers;
  const auto peer_count = static_cast<std::size_t>(state.range(0));
  for (std::size_t index = 0; index < peer_count; ++index) {
    std::array<int, 2> descriptors{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0) {
      state.SkipWithError("failed to create idle extension peer");
      return;
    }
    const auto admitted = runtime->admit(
        extension::FramedPeer(descriptors.at(0)),
        extension::Hello{
            .name = "idle", .subscription = {}, .capabilities = extension::capability_proc},
        SessionId::from_parts(0, 1), AttachmentId::from_parts(0, 1), {});
    if (!admitted.has_value()) {
      static_cast<void>(::close(descriptors.at(1)));
      state.SkipWithError("failed to admit idle extension peer");
      return;
    }
    writers.push_back(descriptors.at(1));
  }
  std::array<extension::PeerView, limits::extension_sessions_hard_max> peers{};
  std::array<render::GridSurface, limits::extension_surfaces_hard_max> surfaces{};
  for ([[maybe_unused]] const auto iteration : state) {
    const auto active = runtime->peer_views(peers);
    const auto projected = runtime->collect_surfaces({}, {.columns = 120, .rows = 40}, surfaces);
    const auto viewport = runtime->pane_viewport({}, {.columns = 120, .rows = 40});
    const auto buffered = runtime->buffered_work();
    auto observed =
        active.size() + projected.size() + (viewport.has_value() ? 1U : 0U) + (buffered ? 1U : 0U);
    benchmark::DoNotOptimize(observed);
  }
  for (const auto writer : writers) {
    static_cast<void>(::close(writer));
  }
}

void benchmark_scene_grid_row_updates(benchmark::State& state) {
  constexpr render::Viewport viewport{.columns = 120, .rows = 40};
  vt::TerminalOptions options;
  options.size = {.columns = 100, .rows = viewport.rows};
  auto terminal_result = vt::Terminal::create(options);
  auto grid_result = render::Grid::create(20, viewport.rows);
  if (!terminal_result.has_value() || !grid_result.has_value()) {
    state.SkipWithError("failed to create Scene benchmark state");
    return;
  }
  auto terminal = std::move(*terminal_result);
  auto grid = std::move(*grid_result);
  render::GridPatch initial;
  initial.rows.push_back({.runs = {{.text = "surface", .column = 0}}, .row = 0});
  if (!grid.apply(std::move(initial)).has_value()) {
    state.SkipWithError("failed to populate benchmark Grid");
    return;
  }
  const std::array panes{render::PaneSurface{
      .terminal = &terminal,
      .rectangle = {.columns = options.size.columns, .rows = options.size.rows},
      .focused = true,
  }};
  const std::array grids{render::GridSurface{
      .grid = &grid,
      .rectangle = {.column = options.size.columns, .columns = 20, .rows = viewport.rows},
  }};
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  if (!render::compose_scene({.panes = panes, .grids = grids}, viewport, frame, true).has_value()) {
    state.SkipWithError("failed to compose initial Scene");
    return;
  }
  bool alternate = false;
  std::uint64_t output_bytes = 0;
  for ([[maybe_unused]] const auto iteration : state) {
    render::GridPatch patch;
    patch.rows.push_back(
        {.runs = {{.text = alternate ? "surface-a" : "surface-b", .column = 0}}, .row = 20});
    alternate = !alternate;
    auto applied = grid.apply(std::move(patch));
    auto rendered = render::compose_scene({.panes = panes, .grids = grids}, viewport, frame, false);
    benchmark::DoNotOptimize(applied);
    benchmark::DoNotOptimize(rendered);
    if (!applied.has_value() || !rendered.has_value()) {
      state.SkipWithError("failed to apply or compose Grid update");
      return;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

void benchmark_terminal_full_frames(benchmark::State& state) {
  auto result = vt::Terminal::create({});
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  std::string contents;
  contents.reserve(std::size_t{24} * 128U);
  for (std::size_t row = 1; row <= 24; ++row) {
    contents += "\x1B[" + std::to_string(row) + ";1H";
    contents += row % 2U == 0 ? "\x1B[1;38;5;4m" : "\x1B[38;2;10;20;30m";
    contents.append(79, static_cast<char>('a' + (row % 26U)));
    contents += "\x1B[0m";
  }
  terminal.write(std::as_bytes(std::span(contents.data(), contents.size())));
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  std::uint64_t output_bytes = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    auto rendered = terminal.render_ansi(frame, true);
    benchmark::DoNotOptimize(rendered);
    if (!rendered.has_value()) {
      state.SkipWithError("failed to render full frame");
      break;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

// Full 80x24 frames of `ls --hyperlink`-style rows: four 16-column names per row. Argument 1 links
// each name to its own file URI (96 links per frame); argument 0 is the same text without links.
void benchmark_terminal_hyperlink_frames(benchmark::State& state) {
  const bool links = state.range(0) != 0;
  auto result = vt::Terminal::create({});
  if (!result.has_value()) {
    state.SkipWithError("failed to create terminal");
    return;
  }
  auto terminal = std::move(result).value();
  std::string contents;
  for (std::size_t row = 1; row <= 24; ++row) {
    contents += "\x1B[" + std::to_string(row) + ";1H";
    for (std::size_t name = 0; name < 4; ++name) {
      const auto file = "file-" + std::to_string(row) + "-" + std::to_string(name) + ".txt";
      if (links) {
        contents += "\x1B]8;;file://build-host/home/user/project/src/" + file + "\x1B\\";
      }
      contents += "\x1B[1;34m" + file + "\x1B[0m";
      if (links) {
        contents += "\x1B]8;;\x1B\\";
      }
      contents.append(20U - file.size(), ' ');
    }
  }
  terminal.write(std::as_bytes(std::span(contents.data(), contents.size())));
  std::array<std::byte, std::size_t{256} * 1'024U> frame{};
  std::uint64_t output_bytes = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    auto rendered = terminal.render_ansi(frame, true);
    benchmark::DoNotOptimize(rendered);
    if (!rendered.has_value()) {
      state.SkipWithError("failed to render hyperlink frame");
      break;
    }
    output_bytes += rendered->bytes;
  }
  state.counters["frame_bytes"] =
      benchmark::Counter(static_cast<double>(output_bytes), benchmark::Counter::kAvgIterations);
}

BENCHMARK(benchmark_command_dispatch);
BENCHMARK(benchmark_input_router_unbound_run);
BENCHMARK(benchmark_input_router_context_command);
BENCHMARK(benchmark_input_router_typed_forward_cycle);
BENCHMARK(benchmark_input_router_typed_context_repeat);
BENCHMARK(benchmark_layout_projection)->Arg(1)->Arg(4)->Arg(16)->Arg(64);
BENCHMARK(benchmark_layout_resize_candidate);
BENCHMARK(benchmark_layout_swap_candidate);
BENCHMARK(benchmark_layout_divider_hit);
BENCHMARK(benchmark_layout_divider_resize_candidate);
BENCHMARK(benchmark_live_divider_pty_resize);
BENCHMARK(benchmark_live_divider_resize)->Arg(2)->Arg(4)->Arg(16)->Arg(64);
BENCHMARK(benchmark_layout_projection_worst_depth);
BENCHMARK(benchmark_layout_divider_hit_worst_depth);
BENCHMARK(benchmark_private_attach_input_codec);
BENCHMARK(benchmark_terminal_small_writes);
BENCHMARK(benchmark_terminal_large_writes);
BENCHMARK(benchmark_terminal_ansi_damage_frames);
BENCHMARK(benchmark_terminal_ansi_single_row);
BENCHMARK(benchmark_terminal_ansi_clean_frame);
BENCHMARK(benchmark_terminal_ansi_scroll_operations);
BENCHMARK(benchmark_terminal_ansi_text_scroll_frames);
BENCHMARK(benchmark_terminal_pane_output_frames)->Arg(0)->Arg(1);
BENCHMARK(benchmark_terminal_viewport_wheel_frames);
BENCHMARK(benchmark_terminal_visible_capture)->Arg(23)->Arg(200);
BENCHMARK(benchmark_terminal_multiple_panes)->Arg(1)->Arg(4)->Arg(16)->Arg(64);
BENCHMARK(benchmark_idle_extension_runtime)->Arg(0)->Arg(1)->Arg(32);
BENCHMARK(benchmark_scene_grid_row_updates);
BENCHMARK(benchmark_terminal_full_frames);
BENCHMARK(benchmark_terminal_hyperlink_frames)->Arg(0)->Arg(1);

} // namespace
} // namespace lemma

namespace {

#ifdef __APPLE__
[[nodiscard]] auto sysctl_string(const char* const name) -> std::optional<std::string> {
  std::size_t size = 0;
  constexpr std::size_t value_bytes_max = 4'096;
  if (::sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0 ||
      size > value_bytes_max) {
    return std::nullopt;
  }
  std::string value(size, '\0');
  if (::sysctlbyname(name, value.data(), &size, nullptr, 0) != 0 || size == 0) {
    return std::nullopt;
  }
  value.resize(size);
  while (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }
  return value.empty() ? std::nullopt : std::optional<std::string>{std::move(value)};
}

[[nodiscard]] auto sysctl_unsigned(const char* const name) -> std::optional<std::uint64_t> {
  std::uint64_t value = 0;
  std::size_t size = sizeof(value);
  if (::sysctlbyname(name, &value, &size, nullptr, 0) != 0 ||
      (size != sizeof(std::uint32_t) && size != sizeof(value))) {
    return std::nullopt;
  }
  return value;
}
#endif

void add_host_fingerprint_context() {
#ifdef __APPLE__
  const auto model = sysctl_string("hw.model");
  const auto cpu = sysctl_string("machdep.cpu.brand_string");
  const auto physical_cpus = sysctl_unsigned("hw.physicalcpu");
  const auto memory = sysctl_unsigned("hw.memsize");
  benchmark::AddCustomContext("host_model_identifier", model.value_or("unavailable"));
  benchmark::AddCustomContext("host_cpu_model", cpu.value_or("unavailable"));
  benchmark::AddCustomContext("host_physical_cpu_count", physical_cpus.has_value()
                                                             ? std::to_string(*physical_cpus)
                                                             : "unavailable");
  benchmark::AddCustomContext("host_memory_bytes",
                              memory.has_value() ? std::to_string(*memory) : "unavailable");
#else
  benchmark::AddCustomContext("host_model_identifier", "unavailable");
  benchmark::AddCustomContext("host_cpu_model", "unavailable");
  benchmark::AddCustomContext("host_physical_cpu_count", "unavailable");
  benchmark::AddCustomContext("host_memory_bytes", "unavailable");
#endif
}

} // namespace

int main(int argc, char** argv) {
  try {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
      return 1;
    }
    add_host_fingerprint_context();
    static_cast<void>(benchmark::RunSpecifiedBenchmarks());
    benchmark::Shutdown();
    return 0;
  } catch (...) {
    return 1;
  }
}
