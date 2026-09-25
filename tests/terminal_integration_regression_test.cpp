#include "core/presentation_gate.hpp"
#include "core/terminal_resize.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "render/pane_composition.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lemma {
namespace {

[[nodiscard]] auto make_terminal(const vt::TerminalOptions& options = {}) -> vt::Terminal {
  auto terminal = vt::Terminal::create(options);
  EXPECT_TRUE(terminal.has_value());
  return std::move(*terminal);
}

void write_terminal(vt::Terminal& terminal, const std::string_view text) {
  terminal.write(std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto output_text(const std::span<const std::byte> bytes) -> std::string_view {
  // Rendered bytes are borrowed as text only for matcher assertions.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Reduced from deterministic seed 0 after generation exposed a Ghostty PageList viewport-pin
// panic. Row shrink, wrapped output, a historical viewport, and later row growth must not let
// dependency-owned pin state abort the server.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalResizeRegressionTest, ResizesHistoricalViewportAfterRowShrinkAndWrappedOutput) {
  vt::TerminalOptions options;
  options.size = {.columns = 24, .rows = 6};
  options.scrollback_lines_max = 256;
  auto terminal = make_terminal(options);
  write_terminal(terminal, "\r\nline two\r\nline three");
  ASSERT_TRUE(terminal.resize({.columns = 24, .rows = 4}).has_value());
  write_terminal(terminal, "history-abcdefghijklmnopqrstuvwxyz-0123456789\r\n");
  terminal.scroll_viewport(vt::ViewportScroll::top);
  terminal.scroll_viewport(vt::ViewportScroll::delta, 1);
  const auto before = terminal.viewport_state();
  ASSERT_TRUE(before.has_value());
  ASSERT_FALSE(before->follows_output);

  ASSERT_TRUE(terminal.resize({.columns = 22, .rows = 9}).has_value());
  const auto after = terminal.viewport_state();
  ASSERT_TRUE(after.has_value());
  EXPECT_FALSE(after->follows_output);
  EXPECT_EQ(after->offset, before->offset);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalResizeRegressionTest, ResizesAlternateScreenWithHistoricalPrimaryViewport) {
  vt::TerminalOptions options;
  options.size = {.columns = 24, .rows = 6};
  options.scrollback_lines_max = 256;
  auto terminal = make_terminal(options);
  write_terminal(terminal, "\r\nline two\r\nline three");
  ASSERT_TRUE(terminal.resize({.columns = 24, .rows = 4}).has_value());
  write_terminal(terminal, "history-abcdefghijklmnopqrstuvwxyz-0123456789\r\n");
  terminal.scroll_viewport(vt::ViewportScroll::top);
  terminal.scroll_viewport(vt::ViewportScroll::delta, 1);
  const auto primary_before = terminal.viewport_state();
  ASSERT_TRUE(primary_before.has_value());
  ASSERT_FALSE(primary_before->follows_output);

  write_terminal(terminal, "\x1B[?1049h\x1B[2Jalternate");
  auto alternate = terminal.inspection();
  ASSERT_TRUE(alternate.has_value());
  ASSERT_EQ(alternate->active_screen, vt::ActiveScreen::alternate);
  ASSERT_TRUE(terminal.resize({.columns = 22, .rows = 9}).has_value());
  alternate = terminal.inspection();
  ASSERT_TRUE(alternate.has_value());
  ASSERT_EQ(alternate->active_screen, vt::ActiveScreen::alternate);
  write_terminal(terminal, "\x1B[?1049lprimary");

  const auto primary_after = terminal.viewport_state();
  ASSERT_TRUE(primary_after.has_value());
  EXPECT_FALSE(primary_after->follows_output);
  EXPECT_EQ(primary_after->offset, primary_before->offset);
}

// Every split point is exercised; assertion macros inflate the nested-loop complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalResizeRegressionTest, PreservesFragmentedChildSequences) {
  constexpr std::array<std::string_view, 4> sequences{
      "\x1B[31mX",
      "\x1B]2;child title\x1B\\",
      "\xE7\x95\x8C",
      "\x1B]2;child title\a",
  };
  for (const auto sequence : sequences) {
    for (std::size_t split = 1; split < sequence.size(); ++split) {
      SCOPED_TRACE(testing::Message() << "sequence=" << sequence << " split=" << split);
      auto expected = make_terminal();
      auto actual = make_terminal();
      write_terminal(expected, "\x1B[?1049h");
      write_terminal(actual, "\x1B[?1049h");
      ASSERT_TRUE(expected.resize({.columns = 81, .rows = 25}).has_value());
      write_terminal(expected, sequence);
      write_terminal(actual, sequence.substr(0, split));
      ASSERT_TRUE(actual.resize({.columns = 81, .rows = 25}).has_value());
      write_terminal(actual, sequence.substr(split));
      std::array<std::byte, 32768> expected_output{};
      std::array<std::byte, 32768> actual_output{};
      const auto expected_size = expected.format_screen(vt::ScreenFormat::vt, expected_output);
      const auto actual_size = actual.format_screen(vt::ScreenFormat::vt, actual_output);
      ASSERT_TRUE(expected_size.has_value());
      ASSERT_TRUE(actual_size.has_value());
      EXPECT_EQ(output_text(std::span(actual_output).first(*actual_size)),
                output_text(std::span(expected_output).first(*expected_size)));
      EXPECT_EQ(actual.title(), expected.title());
      EXPECT_EQ(actual.take_effects().title_changes, expected.take_effects().title_changes);
      EXPECT_FALSE(actual.integrity_failed());
    }
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalRenderRegressionTest, ComposedReflowPreservesCombiningMarkAfterWideGrapheme) {
  vt::TerminalOptions options;
  options.size = {.columns = 24, .rows = 6};
  auto source = make_terminal(options);
  auto projected = make_terminal(options);
  write_terminal(source, "A\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"
                         "e\xCC\x81\xE7\x95\x8C"
                         "Z released released");
  ASSERT_TRUE(source.resize({.columns = 6, .rows = 4}).has_value());
  ASSERT_TRUE(projected.resize({.columns = 6, .rows = 4}).has_value());
  source.scroll_viewport(vt::ViewportScroll::top);

  const render::PaneSurface surface{
      .terminal = &source,
      .rectangle = {.columns = 6, .rows = 4},
      .focused = true,
  };
  std::array<std::byte, std::size_t{16} * 1'024U> frame{};
  const auto composed =
      render::compose_frame(std::span(&surface, 1), {.columns = 6, .rows = 4}, frame, true);
  ASSERT_TRUE(composed.has_value());
  projected.write(std::span(frame).first(composed->bytes));

  std::array<std::byte, 1'024> source_text{};
  std::array<std::byte, 1'024> projected_text{};
  const auto source_size = source.format_visible_tail(vt::ScreenFormat::plain, 4, source_text);
  const auto projected_size =
      projected.format_visible_tail(vt::ScreenFormat::plain, 4, projected_text);
  ASSERT_TRUE(source_size.has_value());
  ASSERT_TRUE(projected_size.has_value());
  // Formatted buffers are copied only into diagnostics on failure.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string source_diagnostic(reinterpret_cast<const char*>(source_text.data()),
                                      *source_size);
  const std::string projected_diagnostic(reinterpret_cast<const char*>(projected_text.data()),
                                         *projected_size);
  // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
  EXPECT_TRUE(std::ranges::equal(std::span(source_text).first(*source_size),
                                 std::span(projected_text).first(*projected_size)))
      << "source=[" << source_diagnostic << "] projected=[" << projected_diagnostic << "]";
}

TEST(TerminalRenderRegressionTest, AnsiProjectionPreservesIndexedExplicitAndDefaultColors) {
  auto theme = vt::default_theme();
  theme.foreground = {.red = 10, .green = 20, .blue = 30};
  theme.background = {.red = 40, .green = 50, .blue = 60};
  theme.cursor = {.red = 70, .green = 80, .blue = 90};
  theme.palette.at(1) = {.red = 101, .green = 102, .blue = 103};
  vt::TerminalOptions options;
  options.size = {.columns = 8, .rows = 2};
  options.theme = theme;
  auto terminal = make_terminal(options);
  write_terminal(terminal, "\x1B[31mR\x1B[0;48;2;7;8;9m \x1B[0mD");

  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  const auto ansi = output_text(std::span(output).first(rendered->bytes));
  EXPECT_THAT(ansi, testing::HasSubstr("38;5;1"));
  EXPECT_THAT(ansi, testing::HasSubstr("48;2;7;8;9"));
  EXPECT_THAT(ansi, testing::Not(testing::HasSubstr("38;2;10;20;30")));
  EXPECT_THAT(ansi, testing::Not(testing::HasSubstr("48;2;40;50;60")));
  EXPECT_THAT(ansi, testing::HasSubstr("\x1B]112\x1B\\"));
}

TEST(TerminalRenderRegressionTest, RepaintsRepeatedGlyphRunsWhenOnlyTheirStylesChange) {
  vt::TerminalOptions options;
  options.size = {.columns = 16, .rows = 2};
  auto terminal = make_terminal(options);
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  write_terminal(terminal, "\x1B[31mxxxx\x1B[32mxxxx\x1B[31mxxxx");
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());

  write_terminal(terminal, "\x1B[H\x1B[32mxxxx\x1B[31mxxxx\x1B[32mxxxx");
  const auto changed = terminal.render_ansi(output);
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(changed->rows, 1U);
  const auto ansi = output_text(std::span(output).first(changed->bytes));
  EXPECT_THAT(ansi, testing::HasSubstr("\x1B[0;38;5;2mxxxx\x1B[0;38;5;1mxxxx\x1B[0;38;5;2mxxxx"));

  const auto unchanged = terminal.render_ansi(output);
  ASSERT_TRUE(unchanged.has_value());
  EXPECT_EQ(unchanged->rows, 0U);
}

TEST(TerminalRenderRegressionTest, KeepsErasedBackgroundsDistinctFromAdjacentTextRuns) {
  vt::TerminalOptions options;
  options.size = {.columns = 12, .rows = 2};
  auto terminal = make_terminal(options);
  write_terminal(terminal, "\x1B[0mAB\x1B[41m\x1B[2X\x1B[2C\x1B[0mCD"
                           "\x1B[44m\x1B[2X\x1B[2C\x1B[0mEF");
  std::array<std::byte, 8192> output{};
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  EXPECT_THAT(output_text(std::span(output).first(rendered->bytes)),
              testing::HasSubstr("\x1B[0mAB\x1B[0;48;5;1m  \x1B[0mCD"
                                 "\x1B[0;48;5;4m  \x1B[0mEF"));
}

TEST(TerminalRenderRegressionTest, ProjectsUnqueriedExtendedPaletteEntriesAsRgb) {
  auto theme = vt::default_theme();
  theme.palette.at(200) = {.red = 10, .green = 20, .blue = 30};
  vt::TerminalOptions options;
  options.size = {.columns = 4, .rows = 2};
  options.theme = theme;
  auto terminal = make_terminal(options);
  write_terminal(terminal, "\x1B[38;5;200mX");

  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  const auto ansi = output_text(std::span(output).first(rendered->bytes));
  EXPECT_THAT(ansi, testing::HasSubstr("38;2;10;20;30"));
  EXPECT_THAT(ansi, testing::Not(testing::HasSubstr("38;5;200")));
}

TEST(TerminalRenderRegressionTest, CleanCellFrameStillProjectsChangedCursorColor) {
  auto terminal = make_terminal();
  write_terminal(terminal, "content");
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());

  write_terminal(terminal, "\x1B]12;rgb:0a/0b/0c\x1B\\");
  const auto rendered = terminal.render_ansi(output);

  ASSERT_TRUE(rendered.has_value());
  EXPECT_THAT(output_text(std::span(output).first(rendered->bytes)),
              testing::HasSubstr("\x1B]12;#0a0b0c\x1B\\"));
}

TEST(TerminalRenderRegressionTest, EraseLineTailPreservesSessionBackground) {
  auto theme = vt::default_theme();
  theme.foreground = {.red = 10, .green = 20, .blue = 30};
  theme.background = {.red = 40, .green = 50, .blue = 60};
  vt::TerminalOptions options;
  options.size = {.columns = 8, .rows = 2};
  options.theme = theme;
  auto terminal = make_terminal(options);
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto blank = terminal.render_ansi(output, true);
  ASSERT_TRUE(blank.has_value());
  EXPECT_THAT(output_text(std::span(output).first(blank->bytes)),
              testing::HasSubstr("\x1B[0m\x1B[K"));

  write_terminal(terminal, "text");
  const auto tail = terminal.render_ansi(output, true);
  ASSERT_TRUE(tail.has_value());
  EXPECT_THAT(output_text(std::span(output).first(tail->bytes)),
              testing::HasSubstr("\x1B[0m\x1B[K"));
}

TEST(TerminalRenderRegressionTest, PaletteRedrawDoesNotImitateTerminalScroll) {
  auto theme = vt::default_theme();
  theme.palette.at(1) = {.red = 1, .green = 2, .blue = 3};
  vt::TerminalOptions options;
  options.size = {.columns = 6, .rows = 4};
  options.theme = theme;
  auto terminal = make_terminal(options);
  write_terminal(terminal, "\x1B[31mAAAA\x1B[2;1HAAAA\x1B[3;1HAAAA\x1B[4;1HAAAA");
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());

  write_terminal(terminal, "\x1B]4;1;rgb:0a/14/1e\x1B\\");
  const auto recolored = terminal.render_ansi(output);
  ASSERT_TRUE(recolored.has_value());
  EXPECT_EQ(recolored->scrolled_rows, 0);
  EXPECT_EQ(recolored->rows, options.size.rows);
  const auto ansi = output_text(std::span(output).first(recolored->bytes));
  EXPECT_THAT(ansi, testing::HasSubstr("38;2;10;20;30"));
  EXPECT_THAT(ansi, testing::Not(testing::HasSubstr("\x1B[1S")));
  EXPECT_THAT(ansi, testing::Not(testing::HasSubstr("\x1B[1T")));
}

TEST(TerminalRenderRegressionTest, AnsiProjectionIsolatesPaneColorOverrides) {
  auto theme = vt::default_theme();
  theme.foreground = {.red = 10, .green = 20, .blue = 30};
  theme.background = {.red = 40, .green = 50, .blue = 60};
  theme.palette.at(1) = {.red = 70, .green = 80, .blue = 90};
  vt::TerminalOptions options;
  options.size = {.columns = 8, .rows = 2};
  options.theme = theme;
  auto terminal = make_terminal(options);
  write_terminal(terminal, "\x1B[31mP\x1B[38;2;70;80;90mT\x1B[0mD"
                           "\x1B]4;1;rgb:01/02/03\x1B\\\x1B[31mO"
                           "\x1B]10;rgb:04/05/06\x1B\\\x1B]11;rgb:07/08/09\x1B\\"
                           "\x1B]12;rgb:0a/0b/0c\x1B\\\x1B[0mX");

  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto overridden = terminal.render_ansi(output, true);
  ASSERT_TRUE(overridden.has_value());
  const auto ansi = output_text(std::span(output).first(overridden->bytes));
  EXPECT_THAT(ansi, testing::HasSubstr("38;2;70;80;90"));
  EXPECT_THAT(ansi, testing::HasSubstr("38;2;1;2;3"));
  EXPECT_THAT(ansi, testing::HasSubstr("38;2;4;5;6"));
  EXPECT_THAT(ansi, testing::HasSubstr("48;2;7;8;9"));
  EXPECT_THAT(ansi, testing::HasSubstr("\x1B]12;#0a0b0c\x1B\\"));

  write_terminal(terminal, "\x1B]104;1\x1B\\\x1B]110\x1B\\\x1B]111\x1B\\\x1B]112\x1B\\\x1B[31mR");
  const auto reset = terminal.render_ansi(output, true);
  ASSERT_TRUE(reset.has_value());
  const auto reset_ansi = output_text(std::span(output).first(reset->bytes));
  EXPECT_THAT(reset_ansi, testing::HasSubstr("38;5;1"));
  EXPECT_THAT(reset_ansi, testing::Not(testing::HasSubstr("38;2;4;5;6")));
  EXPECT_THAT(reset_ansi, testing::Not(testing::HasSubstr("48;2;7;8;9")));
  EXPECT_THAT(reset_ansi, testing::HasSubstr("\x1B]112\x1B\\"));
}

TEST(TerminalRenderRegressionTest, ThemeReplacementPreservesApplicationOverrides) {
  auto theme = vt::default_theme();
  vt::TerminalOptions options;
  options.size = {.columns = 4, .rows = 2};
  options.theme = theme;
  auto terminal = make_terminal(options);
  write_terminal(terminal, "\x1B]4;1;rgb:01/02/03\x1B\\\x1B[31mX");
  theme.foreground = {.red = 10, .green = 20, .blue = 30};
  theme.palette.at(1) = {.red = 40, .green = 50, .blue = 60};

  ASSERT_TRUE(terminal.set_theme(theme).has_value());

  EXPECT_EQ(terminal.theme(), theme);
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  EXPECT_THAT(output_text(std::span(output).first(rendered->bytes)),
              testing::HasSubstr("38;2;1;2;3"));
}

TEST(SynchronizedOutputBoundaryTest, HeldPaneDoesNotBlockLivePane) {
  vt::TerminalOptions options;
  options.size = {.columns = 6, .rows = 2};
  auto held = make_terminal(options);
  auto live = make_terminal(options);
  std::array<std::byte, std::size_t{32} * 1'024U> output{};
  std::array panes{
      render::PaneSurface{
          .terminal = &held, .rectangle = {.columns = 6, .rows = 2}, .focused = true},
      render::PaneSurface{.terminal = &live, .rectangle = {.column = 6, .columns = 6, .rows = 2}},
  };
  ASSERT_TRUE(render::compose_frame(panes, {.columns = 12, .rows = 2}, output, true).has_value());

  write_terminal(held, "\x1B[?2026hhidden");
  write_terminal(live, "visible");
  core::PresentationGate gate;
  ASSERT_TRUE(held.synchronized_output().value_or(false));
  static_cast<void>(gate.observe(true, true, core::PresentationGate::TimePoint{}));
  panes.front().presentation_suppressed = gate.presentation_suppressed();

  const auto rendered = render::compose_frame(panes, {.columns = 12, .rows = 2}, output, false);
  ASSERT_TRUE(rendered.has_value());
  const auto ansi = output_text(std::span(output).first(rendered->bytes));
  EXPECT_THAT(ansi, testing::HasSubstr("visibl"));
  EXPECT_THAT(ansi, testing::Not(testing::HasSubstr("hidden")));
  const auto retained = held.update_render_state();
  ASSERT_TRUE(retained.has_value());
  EXPECT_NE(retained->dirty, vt::DirtyState::clean);

  const auto forced = render::compose_frame(panes, {.columns = 12, .rows = 2}, output, true);
  ASSERT_TRUE(forced.has_value());
  EXPECT_FALSE(forced->full);
  EXPECT_THAT(output_text(std::span(output).first(forced->bytes)),
              testing::Not(testing::HasSubstr("\x1B[2J")));
}

TEST(SynchronizedOutputBoundaryTest, ReleasedFocusedPaneRestoresCanonicalInputModes) {
  vt::TerminalOptions pane_options;
  pane_options.size = {.columns = 6, .rows = 2};
  auto held = make_terminal(pane_options);
  auto live = make_terminal(pane_options);
  vt::TerminalOptions outer_options;
  outer_options.size = {.columns = 12, .rows = 2};
  auto outer = make_terminal(outer_options);
  // The attached client enables outer focus reports for its lifetime; composition must keep them.
  write_terminal(outer, "\x1B[?1004h");
  std::array<std::byte, std::size_t{32} * 1'024U> output{};
  std::array panes{
      render::PaneSurface{
          .terminal = &held, .rectangle = {.columns = 6, .rows = 2}, .focused = true},
      render::PaneSurface{.terminal = &live, .rectangle = {.column = 6, .columns = 6, .rows = 2}},
  };
  write_terminal(held, "\x1B[?1h\x1B[?1004h\x1B[?2004h");
  const auto initial = render::compose_frame(panes, {.columns = 12, .rows = 2}, output, true);
  ASSERT_TRUE(initial.has_value());
  outer.write(std::span(output).first(initial->bytes));

  write_terminal(held, "\x1B[?2026hhidden");
  write_terminal(live, "visible");
  panes.front().presentation_suppressed = true;
  const auto suppressed = render::compose_frame(panes, {.columns = 12, .rows = 2}, output, false,
                                                {}, initial->outer_modes);
  ASSERT_TRUE(suppressed.has_value());
  outer.write(std::span(output).first(suppressed->bytes));

  write_terminal(held, "\x1B[?2026l");
  panes.front().presentation_suppressed = false;
  const auto released = render::compose_frame(panes, {.columns = 12, .rows = 2}, output, false, {},
                                              suppressed->outer_modes);
  ASSERT_TRUE(released.has_value());
  outer.write(std::span(output).first(released->bytes));

  std::array<std::byte, 64> encoded{};
  vt::KeyEvent key_event{};
  key_event.action = vt::KeyAction::press;
  key_event.key = vt::Key::arrow_up;
  const auto key = outer.encode_key(key_event, encoded);
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(output_text(std::span(encoded).first(*key)), "\x1BOA");

  std::array paste_input{std::byte{'x'}};
  const auto paste = outer.encode_paste(paste_input, encoded);
  ASSERT_TRUE(paste.has_value());
  EXPECT_EQ(output_text(std::span(encoded).first(*paste)), "\x1B[200~x\x1B[201~");

  const auto focus = outer.encode_focus(vt::FocusEvent::gained, encoded);
  ASSERT_TRUE(focus.has_value());
  EXPECT_EQ(output_text(std::span(encoded).first(*focus)), "\x1B[I");
}

TEST(SynchronizedOutputBoundaryTest, PresentationWatchdogDoesNotMutateCanonicalMode) {
  auto terminal = make_terminal();
  write_terminal(terminal, "\x1B[?2026hheld");
  ASSERT_TRUE(terminal.synchronized_output().value_or(false));

  core::PresentationGate gate;
  constexpr auto origin = core::PresentationGate::TimePoint{};
  const auto held = gate.observe(true, true, origin);
  EXPECT_TRUE(held.presentation_deferred);
  const auto released =
      gate.release_if_expired(origin + limits::synchronized_output_presentation_timeout);

  EXPECT_TRUE(released.urgent_render);
  EXPECT_TRUE(released.force_full);
  EXPECT_EQ(gate.suppression(), core::PresentationSuppression::watchdog_released);
  EXPECT_EQ(gate.watchdog_releases(), 1U);
  EXPECT_TRUE(terminal.synchronized_output().value_or(false));
}

TEST(TerminalRenderRegressionTest, AbortedFrameRepairsPhysicalShadow) {
  vt::TerminalOptions options;
  options.size = {.columns = 12, .rows = 2};
  auto terminal = make_terminal(options);
  const render::PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 12, .rows = 2},
      .focused = true,
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(render::compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 2}, output, true)
                  .has_value());
  write_terminal(terminal, "repair");

  std::array<std::byte, 48> exhausted{};
  const auto aborted =
      render::compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 2}, exhausted, false);
  ASSERT_FALSE(aborted.has_value());
  EXPECT_EQ(aborted.error(), render::CompositionError::output_exhausted);

  const auto repaired =
      render::compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 2}, output, false);
  ASSERT_TRUE(repaired.has_value());
  EXPECT_TRUE(repaired->full);
  EXPECT_THAT(output_text(std::span(output).first(repaired->bytes)), testing::HasSubstr("repair"));
}

struct ResizeObservation final {
  vt::Terminal* terminal{nullptr};
  std::array<vt::TerminalSize, 2> requested{};
  std::array<vt::TerminalSize, 2> terminal_sizes{};
  std::size_t calls{0};
  std::size_t accepted_calls{0};
};

[[nodiscard]] auto observe_pty_resize(void* const context, const vt::TerminalSize& size) noexcept
    -> bool {
  auto& observation = *static_cast<ResizeObservation*>(context);
  if (observation.calls >= observation.requested.size()) {
    return false;
  }
  std::span(observation.requested).subspan(observation.calls, 1).front() = size;
  std::span(observation.terminal_sizes).subspan(observation.calls, 1).front() =
      observation.terminal->size();
  ++observation.calls;
  return observation.calls <= observation.accepted_calls;
}

TEST(TerminalResizeTransactionTest, PreservesCellPixelsAcrossAppliedLayoutResize) {
  vt::TerminalOptions options;
  options.size = {
      .columns = 80,
      .rows = 24,
      .cell_width_px = 9,
      .cell_height_px = 18,
  };
  auto terminal = make_terminal(options);
  const vt::TerminalSize requested{
      .columns = 40,
      .rows = 12,
      .cell_width_px = options.size.cell_width_px,
      .cell_height_px = options.size.cell_height_px,
  };
  ResizeObservation observation{.terminal = &terminal, .accepted_calls = 1};

  const auto status =
      core::resize_terminal_transaction(terminal, requested, &observe_pty_resize, &observation);

  EXPECT_EQ(status, core::TerminalResizeStatus::applied);
  ASSERT_EQ(observation.calls, 1U);
  EXPECT_EQ(observation.requested.front(), requested);
  EXPECT_EQ(terminal.size(), requested);
}

TEST(TerminalResizeTransactionTest, ReportsPtyGeometryBeforeGhosttyMutation) {
  auto terminal = make_terminal();
  const auto original = terminal.size();
  const vt::TerminalSize requested{.columns = 120, .rows = 40};
  ResizeObservation observation{.terminal = &terminal};

  const auto status =
      core::resize_terminal_transaction(terminal, requested, &observe_pty_resize, &observation);

  EXPECT_EQ(status, core::TerminalResizeStatus::rejected);
  ASSERT_EQ(observation.calls, 1U);
  EXPECT_EQ(observation.requested.front(), requested);
  EXPECT_EQ(observation.terminal_sizes.front(), original)
      << "the child PTY must own requested geometry before Ghostty can use it";
  EXPECT_EQ(terminal.size(), original);
}

TEST(TerminalResizeTransactionTest, RestoresPtyWhenGhosttyRejectsRequest) {
  auto terminal = make_terminal();
  const auto original = terminal.size();
  const vt::TerminalSize invalid{.columns = 0, .rows = 1};
  ResizeObservation observation{.terminal = &terminal, .accepted_calls = 2};

  const auto status =
      core::resize_terminal_transaction(terminal, invalid, &observe_pty_resize, &observation);

  EXPECT_EQ(status, core::TerminalResizeStatus::rolled_back);
  ASSERT_EQ(observation.calls, 2U);
  EXPECT_EQ(observation.requested.front(), invalid);
  EXPECT_EQ(observation.requested.back(), original);
  EXPECT_EQ(observation.terminal_sizes.front(), original);
  EXPECT_EQ(observation.terminal_sizes.back(), original);
  EXPECT_EQ(terminal.size(), original);
}

TEST(TerminalResizeTransactionTest, FailsClosedWhenPtyRollbackFails) {
  auto terminal = make_terminal();
  const auto original = terminal.size();
  const vt::TerminalSize invalid{.columns = 0, .rows = 1};
  ResizeObservation observation{.terminal = &terminal, .accepted_calls = 1};

  const auto status =
      core::resize_terminal_transaction(terminal, invalid, &observe_pty_resize, &observation);

  EXPECT_EQ(status, core::TerminalResizeStatus::consistency_lost);
  ASSERT_EQ(observation.calls, 2U);
  EXPECT_EQ(observation.requested.front(), invalid);
  EXPECT_EQ(observation.requested.back(), original);
  EXPECT_EQ(terminal.size(), original);
}

// Remaining disabled tests are executable M0 specifications for later milestones. Enabling any one
// requires replacing FAIL() with the real characterization.

// NOLINTNEXTLINE(readability-function-cognitive-complexity)

} // namespace
} // namespace lemma
