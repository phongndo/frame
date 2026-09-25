#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "render/grid.hpp"
#include "render/pane_composition.hpp"
#include "render/scene.hpp"
#include "render/status_line.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lemma::render {
namespace {

[[nodiscard]] auto make_terminal(const std::uint16_t columns, const std::uint16_t rows)
    -> vt::Terminal {
  vt::TerminalOptions options;
  options.size = {.columns = columns, .rows = rows};
  auto terminal = vt::Terminal::create(options);
  EXPECT_TRUE(terminal.has_value());
  return std::move(*terminal);
}

void write_text(vt::Terminal& terminal, const std::string_view text) {
  terminal.write(std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto as_text(const std::span<const std::byte> bytes) -> std::string_view {
  // The frame is an ANSI byte stream and std::string_view is only a non-owning test view.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void expect_grid_screen(vt::Terminal& terminal, const std::string_view expected) {
  auto reference = make_terminal(8, 1);
  write_text(reference, expected);
  std::array<std::byte, 1024> actual_bytes{};
  std::array<std::byte, 1024> expected_bytes{};
  const auto actual_size = terminal.format_screen(vt::ScreenFormat::plain, actual_bytes);
  const auto expected_size = reference.format_screen(vt::ScreenFormat::plain, expected_bytes);
  ASSERT_TRUE(actual_size.has_value());
  ASSERT_TRUE(expected_size.has_value());
  EXPECT_EQ(as_text(std::span(actual_bytes).first(*actual_size)),
            as_text(std::span(expected_bytes).first(*expected_size)));
}

[[nodiscard]] auto occurrences(const std::string_view text, const std::string_view needle)
    -> std::size_t {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

TEST(PaneCompositionTest, PlacesMultipleTerminalSurfacesInOneAtomicFrame) {
  auto left = make_terminal(5, 2);
  auto right = make_terminal(5, 2);
  write_text(left, "left");
  write_text(right, "right");
  const std::array panes{
      PaneSurface{.terminal = &left, .rectangle = {.column = 0, .row = 0, .columns = 5, .rows = 2}},
      PaneSurface{.terminal = &right,
                  .rectangle = {.column = 5, .row = 0, .columns = 5, .rows = 2},
                  .focused = true},
  };
  std::array<std::byte, std::size_t{64} * 1'024U> output{};

  const auto result = compose_frame(panes, {.columns = 10, .rows = 2}, output, true);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->panes, panes.size());
  EXPECT_EQ(result->rows, 4U);
  EXPECT_TRUE(result->full);
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;6H"));
  EXPECT_THAT(encoded, testing::HasSubstr("left"));
  EXPECT_THAT(encoded, testing::HasSubstr("right"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;10H\x1B[?25h"));
  EXPECT_EQ(occurrences(encoded, "\x1B[?2026h"), 1U);
  EXPECT_EQ(occurrences(encoded, "\x1B[?2026l"), 1U);
  EXPECT_EQ(occurrences(encoded, "\x1B[2J"), 1U);
}

TEST(PaneCompositionTest, ErasesLongBlankTailsWithoutTouchingNeighboringPanes) {
  auto left = make_terminal(80, 2);
  auto right = make_terminal(8, 2);
  auto outer = make_terminal(88, 2);
  write_text(left,
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopq");
  write_text(right, "neighbor");
  const std::array panes{
      PaneSurface{.terminal = &left, .rectangle = {.columns = 80, .rows = 2}, .focused = true},
      PaneSurface{.terminal = &right, .rectangle = {.column = 80, .columns = 8, .rows = 2}},
  };
  std::array<std::byte, 8192> output{};
  const auto initial = compose_frame(panes, {.columns = 88, .rows = 2}, output, true);
  ASSERT_TRUE(initial.has_value());
  outer.write(std::span(output).first(initial->bytes));

  write_text(left, "\x1B[H__LEMMA_OUTPUT_0000_AAAAAA__\x1B[K");
  const auto changed =
      compose_frame(panes, {.columns = 88, .rows = 2}, output, false, {}, initial->outer_modes);
  ASSERT_TRUE(changed.has_value());
  EXPECT_LT(changed->bytes, 120U);
  outer.write(std::span(output).first(changed->bytes));

  auto expected = make_terminal(88, 2);
  write_text(expected, "__LEMMA_OUTPUT_0000_AAAAAA__\x1B[1;81Hneighbor\x1B[2;1H ");
  std::array<std::byte, 1024> actual_text{};
  std::array<std::byte, 1024> expected_text{};
  const auto actual_size = outer.format_screen(vt::ScreenFormat::plain, actual_text);
  const auto expected_size = expected.format_screen(vt::ScreenFormat::plain, expected_text);
  ASSERT_TRUE(actual_size.has_value());
  ASSERT_TRUE(expected_size.has_value());
  EXPECT_EQ(as_text(std::span(actual_text).first(*actual_size)),
            as_text(std::span(expected_text).first(*expected_size)));
}

TEST(PaneCompositionTest, LeftAlignsMinimalTabStatusAbovePaneContent) {
  auto terminal = make_terminal(40, 2);
  write_text(terminal, "content");
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 40, .rows = 2},
      .focused = true,
  };
  const std::array tabs{
      StatusTab{.number = 1, .title = "zsh"},
      StatusTab{.number = 2, .title = "nvim", .active = true},
      StatusTab{.number = 3, .title = "logs"},
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 40, .rows = 3}, output, true,
                                    {.session_name = {},
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::none,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = {},
                                     .input_context = {},
                                     .prompt_cursor = 0,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->status);
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H\x1B[0m1:zsh"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[0;1m[ 2:nvim ]"));
  EXPECT_THAT(encoded, testing::HasSubstr("  +"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[2;1H"));
  EXPECT_THAT(encoded, testing::HasSubstr("content"));
}

TEST(PaneCompositionTest, StatusControlHitTestMatchesRenderedLabelsAndOverflow) {
  const std::array tabs{
      StatusTab{.number = 1, .title = "zsh"},
      StatusTab{.number = 2, .title = "nvim", .active = true},
      StatusTab{.number = 3, .title = "logs"},
  };
  const StatusLine status{.session_name = "lemma",
                          .tabs = tabs,
                          .prompt_target = StatusPromptTarget::none,
                          .prompt_feedback = StatusPromptFeedback::none,
                          .prompt_value = {},
                          .input_context = {},
                          .prompt_cursor = 0,
                          .dirty = false};

  EXPECT_EQ(status_target_at_column(status, {.columns = 40, .rows = 3}, 10),
            (StatusTarget{.kind = StatusTargetKind::tab, .tab_position = 0}));
  EXPECT_EQ(status_target_at_column(status, {.columns = 40, .rows = 3}, 17),
            (StatusTarget{.kind = StatusTargetKind::tab, .tab_position = 1}));
  EXPECT_EQ(status_target_at_column(status, {.columns = 40, .rows = 3}, 29),
            (StatusTarget{.kind = StatusTargetKind::tab, .tab_position = 2}));
  EXPECT_EQ(status_target_at_column(status, {.columns = 40, .rows = 3}, 37),
            (StatusTarget{.kind = StatusTargetKind::create_tab, .tab_position = 0}));
  EXPECT_EQ(status_target_at_column(status, {.columns = 40, .rows = 3}, 0), std::nullopt);
  EXPECT_EQ(status_target_at_column(status, {.columns = 40, .rows = 3}, 8), std::nullopt);
  EXPECT_EQ(status_target_at_column(status, {.columns = 40, .rows = 3}, 15), std::nullopt);

  const std::array overflow_tabs{
      StatusTab{.number = 1, .title = "shell"},
      StatusTab{.number = 2, .title = "api"},
      StatusTab{.number = 3, .title = "nvim", .active = true},
      StatusTab{.number = 4, .title = "tests"},
      StatusTab{.number = 5, .title = "logs"},
  };
  const StatusLine overflow{.session_name = {},
                            .tabs = overflow_tabs,
                            .prompt_target = StatusPromptTarget::none,
                            .prompt_feedback = StatusPromptFeedback::none,
                            .prompt_value = {},
                            .input_context = {},
                            .prompt_cursor = 0,
                            .dirty = false};
  EXPECT_EQ(status_target_at_column(overflow, {.columns = 18, .rows = 3}, 2),
            (StatusTarget{.kind = StatusTargetKind::tab, .tab_position = 2}));
  EXPECT_EQ(status_target_at_column(overflow, {.columns = 18, .rows = 3}, 11),
            (StatusTarget{.kind = StatusTargetKind::tab, .tab_position = 2}));
  EXPECT_EQ(status_target_at_column(overflow, {.columns = 18, .rows = 3}, 16),
            (StatusTarget{.kind = StatusTargetKind::create_tab, .tab_position = 0}));
  EXPECT_EQ(status_target_at_column(overflow, {.columns = 18, .rows = 3}, 0), std::nullopt);
  EXPECT_EQ(status_target_at_column(overflow, {.columns = 18, .rows = 3}, 12), std::nullopt);

  auto prompting = status;
  prompting.prompt_target = StatusPromptTarget::active_tab;
  prompting.prompt_value = "nvim";
  prompting.prompt_cursor = prompting.prompt_value.size();
  EXPECT_EQ(status_target_at_column(prompting, {.columns = 40, .rows = 3}, 17), std::nullopt);

  auto modal = status;
  modal.input_context = "RESIZE";
  EXPECT_EQ(status_target_at_column(modal, {.columns = 40, .rows = 3}, 17), std::nullopt);
}

TEST(PaneCompositionTest, ActiveInputContextReplacesStatusRowWithoutABadge) {
  auto terminal = make_terminal(40, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 40, .rows = 2},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "shell", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 40, .rows = 3}, output, true,
                                    {.session_name = "work",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::none,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = {},
                                     .input_context = "RESIZE",
                                     .prompt_cursor = 0,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H\x1B[0;1mRESIZE"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[0;7m")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("[ 1:shell ]")));
}

TEST(PaneCompositionTest, EditsActiveTabInlineAndOwnsTheVisibleCursor) {
  auto terminal = make_terminal(20, 2);
  write_text(terminal, "content");
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 20, .rows = 2},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 20, .rows = 3}, output, true,
                                    {.session_name = "work",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::active_tab,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = "abc",
                                     .input_context = {},
                                     .prompt_cursor = 1,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[0;1m work \x1B[0m | "));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[0;1m[ 1:\x1B[0;1;4mabc\x1B[0;1m ]"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;15H\x1B[2 q\x1B[?25h"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[0;2m")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("[ 1:zsh ]")));
}

TEST(PaneCompositionTest, CommandLineUsesTheWholeStatusRowWithoutAnnotations) {
  auto terminal = make_terminal(64, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 64, .rows = 2},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 64, .rows = 3}, output, true,
                                    {.session_name = "work",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::command_line,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = "pane split --right",
                                     .input_context = {},
                                     .prompt_cursor = 18,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr(":pane split --right"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[0;1;4m")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("Split the current pane")));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;20H\x1B[2 q\x1B[?25h"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("[ 1:zsh ]")));
}

TEST(PaneCompositionTest, CommandFailureReplacesTheStatusRowWithALeftAlignedMessage) {
  auto terminal = make_terminal(40, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 40, .rows = 2},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 40, .rows = 3}, output, true,
                                    {.session_name = "work",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::message,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = {},
                                     .input_context = "Error: Unknown command",
                                     .prompt_cursor = 0,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H\x1B[0;1mError: Unknown command"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("[ 1:zsh ]")));
}

TEST(PaneCompositionTest, MessageViewReplacesPaneContentWithAnOpaqueBoundedLog) {
  auto terminal = make_terminal(40, 3);
  write_text(terminal, "underlying pane content");
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 40, .rows = 3},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  const std::array lines{
      MessageViewLine{.text = "[2026-03-01 10:00:00] Information", .error = false},
      MessageViewLine{.text = "[2026-03-01 10:00:01] Error: Unknown command", .error = true},
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 40, .rows = 4}, output, true,
                                    {.session_name = "work",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::none,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = {},
                                     .input_context = "LOG",
                                     .prompt_cursor = 0,
                                     .dirty = true},
                                    std::nullopt, {.lines = lines, .active = true});

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->panes, 0U);
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("[2026-03-01 10:00:00] Information"));
  EXPECT_THAT(encoded, testing::HasSubstr("[2026-03-01 10:00:01] Error: Unknown "));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H\x1B[0;1mLOG"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[0;7m")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("[ 1:zsh ]")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("underlying pane content")));
}

TEST(PaneCompositionTest, LeavingStatusPromptRestoresFocusedPaneCursorBlinking) {
  auto terminal = make_terminal(20, 2);
  write_text(terminal, "\x1B[1 qcontent");
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 20, .rows = 2},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  auto outer = make_terminal(20, 3);

  const StatusLine normal_status{
      .session_name = "work",
      .tabs = tabs,
      .prompt_target = StatusPromptTarget::none,
      .prompt_feedback = StatusPromptFeedback::none,
      .prompt_value = {},
      .input_context = {},
      .prompt_cursor = 0,
      .dirty = true,
  };
  const auto initial =
      compose_frame(std::span(&pane, 1), {.columns = 20, .rows = 3}, output, true, normal_status);
  ASSERT_TRUE(initial.has_value());
  outer.write(std::span(output).first(initial->bytes));

  const StatusLine prompt_status{
      .session_name = "work",
      .tabs = tabs,
      .prompt_target = StatusPromptTarget::active_tab,
      .prompt_feedback = StatusPromptFeedback::none,
      .prompt_value = "zsh",
      .input_context = {},
      .prompt_cursor = 1,
      .dirty = true,
  };
  const auto prompt = compose_frame(std::span(&pane, 1), {.columns = 20, .rows = 3}, output, false,
                                    prompt_status, initial->outer_modes);
  ASSERT_TRUE(prompt.has_value());
  outer.write(std::span(output).first(prompt->bytes));

  const auto restored = compose_frame(std::span(&pane, 1), {.columns = 20, .rows = 3}, output,
                                      false, normal_status, prompt->outer_modes);
  ASSERT_TRUE(restored.has_value());
  outer.write(std::span(output).first(restored->bytes));

  outer.invalidate_ansi_render_state();
  const auto projected = outer.render_ansi(output, true);
  ASSERT_TRUE(projected.has_value());
  const auto encoded = as_text(std::span(output).first(projected->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1 q"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[2 q")));
}

TEST(PaneCompositionTest, NarrowTabEditorShowsOnlyItsBoundedField) {
  auto terminal = make_terminal(4, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 4, .rows = 2},
      .focused = true,
  };
  const std::array tabs{
      StatusTab{.number = 1, .title = "shell"},
      StatusTab{.number = 2, .title = "nvim", .active = true},
      StatusTab{.number = 3, .title = "logs"},
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 4, .rows = 3}, output, true,
                                    {.session_name = "work",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::active_tab,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = "abcdef",
                                     .input_context = {},
                                     .prompt_cursor = 6,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[0;1;4mdef\x1B[0;1m "));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;4H\x1B[2 q\x1B[?25h"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("…")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("[2:")));
}

TEST(PaneCompositionTest, RejectsControlBytesInEditableStatusValues) {
  auto terminal = make_terminal(20, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 20, .rows = 2},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  constexpr std::array prompt{'b', 'a', 'd', '\x1B', 'x'};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 20, .rows = 3}, output, true,
                                    {.session_name = "work",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::active_tab,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = std::string_view(prompt.data(), prompt.size()),
                                     .input_context = {},
                                     .prompt_cursor = prompt.size(),
                                     .dirty = true});

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), CompositionError::invalid_status);
}

TEST(PaneCompositionTest, EditsSessionInlineWithActiveTabContextAndConflictFeedback) {
  auto terminal = make_terminal(80, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 80, .rows = 2},
      .focused = true,
  };
  const std::array tabs{
      StatusTab{.number = 1, .title = "zsh", .active = true},
      StatusTab{.number = 2, .title = "nvim"},
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 80, .rows = 3}, output, true,
                                    {.session_name = "original",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::session,
                                     .prompt_feedback = StatusPromptFeedback::conflict,
                                     .prompt_value = "occupied",
                                     .input_context = {},
                                     .prompt_cursor = 8,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[0;1m \x1B[0;1;4moccupied\x1B[0;1m "));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[0m | \x1B[0;1m[ 1:zsh ]"));
  EXPECT_THAT(encoded, testing::HasSubstr("Session already exists"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;10H\x1B[2 q\x1B[?25h"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[0;2m")));
}

TEST(PaneCompositionTest, CopySearchUsesTheWholeStatusRowAndOwnsTheCursor) {
  auto terminal = make_terminal(12, 2);
  write_text(terminal, "underlay");
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 12, .rows = 2},
      .cursor_override_column = 1,
      .cursor_override_row = 1,
      .focused = true,
      .cursor_override = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 3}, output, true,
                                    {.session_name = {},
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::copy_search_forward,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = "ls",
                                     .input_context = {},
                                     .prompt_cursor = 2,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H\x1B[0;1m/ls"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;4H\x1B[2 q\x1B[?25h"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("[ 1:zsh ]")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[0;7m")));
}

TEST(PaneCompositionTest, DrawsBoldSessionBeforeLeftAlignedTabs) {
  auto terminal = make_terminal(40, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 40, .rows = 2},
      .focused = true,
  };
  const std::array tabs{
      StatusTab{.number = 1, .title = "zsh"},
      StatusTab{.number = 2, .title = "nvim", .active = true},
      StatusTab{.number = 3, .title = "logs"},
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 40, .rows = 3}, output, true,
                                    {.session_name = "lemma",
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::none,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = {},
                                     .input_context = {},
                                     .prompt_cursor = 0,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H\x1B[0;1m lemma \x1B[0m | 1:zsh"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[0;1m[ 2:nvim ]"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[0;2m")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[38")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[48")));
}

TEST(PaneCompositionTest, OffsetsPaneSeparatorsBelowTopStatus) {
  auto terminal = make_terminal(4, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 4, .rows = 2},
      .focused = true,
      .border_right = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 5, .rows = 3}, output, true,
                                    {.session_name = {},
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::none,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = {},
                                     .input_context = {},
                                     .prompt_cursor = 0,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[2;5H│"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[3;5H│"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B[1;5H│")));
}

TEST(PaneCompositionTest, RejectsPaneGeometryThatExceedsStatusReservedContent) {
  auto terminal = make_terminal(20, 3);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 20, .rows = 3},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 20, .rows = 3}, output, true,
                                    {.session_name = {},
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::none,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = {},
                                     .input_context = {},
                                     .prompt_cursor = 0,
                                     .dirty = true});

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), CompositionError::invalid_pane);
}

TEST(PaneCompositionTest, KeepsActiveTabVisibleWhenStatusOverflows) {
  auto terminal = make_terminal(18, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 18, .rows = 2},
      .focused = true,
  };
  const std::array tabs{
      StatusTab{.number = 1, .title = "shell"},
      StatusTab{.number = 2, .title = "api"},
      StatusTab{.number = 3, .title = "nvim", .active = true},
      StatusTab{.number = 4, .title = "tests"},
      StatusTab{.number = 5, .title = "logs"},
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 18, .rows = 3}, output, true,
                                    {.session_name = {},
                                     .tabs = tabs,
                                     .prompt_target = StatusPromptTarget::none,
                                     .prompt_feedback = StatusPromptFeedback::none,
                                     .prompt_value = {},
                                     .input_context = {},
                                     .prompt_cursor = 0,
                                     .dirty = true});

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("[ 3:nvim ]"));
  EXPECT_THAT(encoded, testing::HasSubstr("…"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("1:shell")));
}

TEST(PaneCompositionTest, OmitsCleanStatusFromIncrementalFrame) {
  auto terminal = make_terminal(12, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 12, .rows = 2},
      .focused = true,
  };
  const std::array tabs{StatusTab{.number = 1, .title = "zsh", .active = true}};
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const StatusLine dirty_status{.session_name = {},
                                .tabs = tabs,
                                .prompt_target = StatusPromptTarget::none,
                                .prompt_feedback = StatusPromptFeedback::none,
                                .prompt_value = {},
                                .input_context = {},
                                .prompt_cursor = 0,
                                .dirty = true};
  ASSERT_TRUE(
      compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 3}, output, true, dirty_status)
          .has_value());

  auto clean_status = dirty_status;
  clean_status.dirty = false;
  const auto result =
      compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 3}, output, false, clean_status);

  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->status);
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("[ 1:zsh ]")));
}

TEST(PaneCompositionTest, ProjectsOuterMouseModesOnlyWhenTheyChange) {
  auto terminal = make_terminal(12, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 12, .rows = 2},
      .focused = true,
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto initial = compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 2}, output, true);
  ASSERT_TRUE(initial.has_value());
  EXPECT_EQ(initial->outer_modes, OuterModeProjection::button_mouse);

  write_text(terminal, "ordinary damage");
  const auto retained = compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 2}, output,
                                      false, {}, initial->outer_modes);
  ASSERT_TRUE(retained.has_value());
  const auto retained_text = as_text(std::span(output).first(retained->bytes));
  EXPECT_THAT(retained_text, testing::Not(testing::HasSubstr("\x1B[?1002h")));
  EXPECT_THAT(retained_text, testing::Not(testing::HasSubstr("\x1B[?1003h")));
  EXPECT_THAT(retained_text, testing::Not(testing::HasSubstr("\x1B[?2004h")));
  EXPECT_THAT(retained_text, testing::Not(testing::HasSubstr("\x1B[?2004l")));
  EXPECT_THAT(retained_text, testing::Not(testing::HasSubstr("\x1B]112\x1B\\")));
  EXPECT_THAT(retained_text, testing::Not(testing::HasSubstr("\x1B[2 q")));

  write_text(terminal, "\x1B[?1003h\x1B[?2004h\x1B[5 qmode change");
  const auto changed = compose_frame(std::span(&pane, 1), {.columns = 12, .rows = 2}, output, false,
                                     {}, retained->outer_modes);
  ASSERT_TRUE(changed.has_value());
  EXPECT_EQ(changed->outer_modes, OuterModeProjection::any_mouse);
  const auto changed_text = as_text(std::span(output).first(changed->bytes));
  EXPECT_THAT(changed_text, testing::HasSubstr("\x1B[?1003h"));
  EXPECT_THAT(changed_text, testing::HasSubstr("\x1B[?2004h"));
  EXPECT_THAT(changed_text, testing::HasSubstr("\x1B[5 q"));
}

TEST(PaneCompositionTest, DrawsDeclaredPaneSeparators) {
  auto terminal = make_terminal(4, 2);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.column = 0, .row = 0, .columns = 4, .rows = 2},
      .focused = true,
      .border_right = true,
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 5, .rows = 2}, output, true);

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;5H│"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[2;5H│"));

  write_text(terminal, "changed");
  const auto incremental =
      compose_frame(std::span(&pane, 1), {.columns = 5, .rows = 2}, output, false);
  ASSERT_TRUE(incremental.has_value());
  const auto incremental_encoded = as_text(std::span(output).first(incremental->bytes));
  EXPECT_THAT(incremental_encoded, testing::Not(testing::HasSubstr("│")));
  EXPECT_THAT(incremental_encoded, testing::Not(testing::HasSubstr("\x1B[90m")));
}

TEST(PaneCompositionTest, ConnectsNestedSplitBordersAtJunctions) {
  auto left = make_terminal(4, 5);
  auto top_right = make_terminal(5, 2);
  auto bottom_right = make_terminal(5, 2);
  const std::array panes{
      PaneSurface{.terminal = &left,
                  .rectangle = {.column = 0, .row = 0, .columns = 4, .rows = 5},
                  .border_right = true},
      PaneSurface{.terminal = &top_right,
                  .rectangle = {.column = 5, .row = 0, .columns = 5, .rows = 2},
                  .border_bottom = true},
      PaneSurface{.terminal = &bottom_right,
                  .rectangle = {.column = 5, .row = 3, .columns = 5, .rows = 2},
                  .focused = true},
  };
  std::array<std::byte, std::size_t{32} * 1'024U> output{};

  const auto result = compose_frame(panes, {.columns = 10, .rows = 5}, output, true);

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[3;5H├"));
}

TEST(PaneCompositionTest, FillsFourPaneCrossJunction) {
  auto top_left = make_terminal(4, 3);
  auto top_right = make_terminal(4, 3);
  auto bottom_left = make_terminal(4, 3);
  auto bottom_right = make_terminal(4, 3);
  const std::array panes{
      PaneSurface{.terminal = &top_left,
                  .rectangle = {.column = 0, .row = 0, .columns = 4, .rows = 3},
                  .border_right = true,
                  .border_bottom = true},
      PaneSurface{.terminal = &top_right,
                  .rectangle = {.column = 5, .row = 0, .columns = 4, .rows = 3},
                  .border_bottom = true},
      PaneSurface{.terminal = &bottom_left,
                  .rectangle = {.column = 0, .row = 4, .columns = 4, .rows = 3},
                  .border_right = true},
      PaneSurface{.terminal = &bottom_right,
                  .rectangle = {.column = 5, .row = 4, .columns = 4, .rows = 3},
                  .focused = true},
  };
  std::array<std::byte, std::size_t{32} * 1'024U> output{};

  const auto result = compose_frame(panes, {.columns = 9, .rows = 7}, output, true);

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[4;5H┼"));
}

TEST(PaneCompositionTest, IncrementalFrameVisitsCleanPanesWithoutRepaintingTheirCells) {
  auto left = make_terminal(6, 2);
  auto right = make_terminal(6, 2);
  write_text(left, "left");
  write_text(right, "right");
  const std::array panes{
      PaneSurface{.terminal = &left,
                  .rectangle = {.column = 0, .row = 0, .columns = 6, .rows = 2},
                  .focused = true},
      PaneSurface{.terminal = &right,
                  .rectangle = {.column = 6, .row = 0, .columns = 6, .rows = 2}},
  };
  std::array<std::byte, std::size_t{64} * 1'024U> output{};
  ASSERT_TRUE(compose_frame(panes, {.columns = 12, .rows = 2}, output, true).has_value());

  write_text(left, "\x1B[2;1Hnew");
  const auto result = compose_frame(panes, {.columns = 12, .rows = 2}, output, false);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->rows, 1U);
  EXPECT_FALSE(result->full);
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("new"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("right")));
}

TEST(PaneCompositionTest, FocusedSurfaceAlwaysOwnsOuterTerminalModes) {
  auto left = make_terminal(4, 1);
  auto right = make_terminal(4, 1);
  write_text(left, "\x1B[?2004h");
  std::array panes{
      PaneSurface{.terminal = &left,
                  .rectangle = {.column = 0, .row = 0, .columns = 4, .rows = 1},
                  .focused = true},
      PaneSurface{.terminal = &right,
                  .rectangle = {.column = 4, .row = 0, .columns = 4, .rows = 1}},
  };
  std::array<std::byte, std::size_t{64} * 1'024U> output{};
  auto result = compose_frame(panes, {.columns = 8, .rows = 1}, output, true);
  ASSERT_TRUE(result.has_value());
  auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?2004h"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?1002h"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?1006h"));

  // Parse the actual composed frame rather than merely checking that enable sequences occur.
  // Mouse event/format modes are mutually exclusive, so later competing resets must not silently
  // cancel button tracking or SGR encoding.
  auto outer_terminal = make_terminal(8, 1);
  write_text(outer_terminal, encoded);
  const auto tracking = outer_terminal.mouse_tracking();
  ASSERT_TRUE(tracking.has_value());
  EXPECT_EQ(*tracking, (vt::MouseTrackingState{.enabled = true}));
  std::array<std::byte, 64> mouse_output{};
  const vt::MouseEvent mouse{
      .action = vt::MouseAction::press,
      .button = vt::MouseButton::left,
      .x = 1,
      .geometry = {.screen_width = 8, .screen_height = 1},
  };
  const auto mouse_result = outer_terminal.encode_mouse(mouse, mouse_output);
  ASSERT_TRUE(mouse_result.has_value());
  EXPECT_EQ(as_text(std::span(mouse_output).first(*mouse_result)), "\x1B[<0;2;1M");

  panes.front().focused = false;
  panes.back().focused = true;
  result = compose_frame(panes, {.columns = 8, .rows = 1}, output, false);
  ASSERT_TRUE(result.has_value());
  encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?2004l"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?1002h"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?1006h"));
}

TEST(PaneCompositionTest, PromotesOuterCaptureOnlyForUnbuttonedApplicationMotion) {
  auto terminal = make_terminal(8, 1);
  write_text(terminal, "\x1B[?1003h");
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 8, .rows = 1},
      .focused = true,
  };
  std::array<std::byte, std::size_t{16} * 1'024U> output{};

  const auto result = compose_frame(std::span(&pane, 1), {.columns = 8, .rows = 1}, output, true);

  ASSERT_TRUE(result.has_value());
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?1002l"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?1003h"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?1006h"));

  auto outer_terminal = make_terminal(8, 1);
  write_text(outer_terminal, encoded);
  const auto tracking = outer_terminal.mouse_tracking();
  ASSERT_TRUE(tracking.has_value());
  EXPECT_EQ(*tracking, (vt::MouseTrackingState{.enabled = true, .unbuttoned_motion = true}));
}

TEST(PaneCompositionTest, ClearsViewportWithoutPaneCoordinatesForSuspendedLayout) {
  std::array<std::byte, 1'024> output{};

  const auto result = compose_frame({}, {.columns = 1, .rows = 1}, output, true);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->panes, 0U);
  EXPECT_EQ(result->rows, 0U);
  EXPECT_TRUE(result->full);
  const auto encoded = as_text(std::span(output).first(result->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[2J\x1B[H"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr(";")));
}

TEST(PaneCompositionTest, RejectsInvalidGeometryAndFocusBeforeRendering) {
  auto terminal = make_terminal(5, 2);
  std::array<std::byte, 1'024> output{};
  const std::array outside{
      PaneSurface{.terminal = &terminal,
                  .rectangle = {.column = 6, .row = 0, .columns = 5, .rows = 2}},
  };
  auto result = compose_frame(outside, {.columns = 10, .rows = 2}, output, false);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), CompositionError::invalid_pane);

  const std::array duplicate_focus{
      PaneSurface{.terminal = &terminal,
                  .rectangle = {.column = 0, .row = 0, .columns = 5, .rows = 2},
                  .focused = true},
      PaneSurface{.terminal = &terminal,
                  .rectangle = {.column = 5, .row = 0, .columns = 5, .rows = 2},
                  .focused = true},
  };
  result = compose_frame(duplicate_focus, {.columns = 10, .rows = 2}, output, false);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), CompositionError::multiple_focused_panes);
}

TEST(PaneCompositionTest, RejectsOverlappingPaneRectanglesAndSeparatorsBeforeRendering) {
  auto wide_terminal = make_terminal(3, 1);
  auto narrow_terminal = make_terminal(2, 1);
  std::array<std::byte, 1'024> output{};
  const std::array overlapping_contents{
      PaneSurface{.terminal = &wide_terminal,
                  .rectangle = {.column = 0, .row = 0, .columns = 3, .rows = 1}},
      PaneSurface{.terminal = &wide_terminal,
                  .rectangle = {.column = 2, .row = 0, .columns = 3, .rows = 1}},
  };

  auto result = compose_frame(overlapping_contents, {.columns = 5, .rows = 1}, output, false);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), CompositionError::invalid_pane);
  EXPECT_EQ(output.front(), std::byte{});

  const std::array separator_overlaps_content{
      PaneSurface{.terminal = &narrow_terminal,
                  .rectangle = {.column = 0, .row = 0, .columns = 2, .rows = 1},
                  .border_right = true},
      PaneSurface{.terminal = &narrow_terminal,
                  .rectangle = {.column = 2, .row = 0, .columns = 2, .rows = 1}},
  };

  result = compose_frame(separator_overlaps_content, {.columns = 4, .rows = 1}, output, false);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), CompositionError::invalid_pane);
  EXPECT_EQ(output.front(), std::byte{});
}

TEST(GridTest, AppliesRowsTransactionallyAndPreservesStateAfterInvalidPatch) {
  auto created = Grid::create(10, 2);
  ASSERT_TRUE(created.has_value());
  auto grid = std::move(*created);
  GridPatch valid;
  valid.rows.push_back({.runs = {{.text = "valid", .column = 0, .style = 0}}, .row = 0});
  const auto applied = grid.apply(std::move(valid));
  ASSERT_TRUE(applied.has_value());
  EXPECT_EQ(applied->changed_rows, 1U);
  const auto generation = grid.generation();

  GridPatch invalid;
  invalid.rows.push_back({.runs = {{.text = "bad\x1b", .column = 0, .style = 0}}, .row = 0});
  const auto rejected = grid.apply(std::move(invalid));
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error(), GridError::invalid_run);
  EXPECT_EQ(grid.generation(), generation);

  std::array<std::byte, 4'096> output{};
  const auto rendered =
      grid.render_ansi(output, {.rectangle = {.columns = 10, .rows = 2}, .force_full = true});
  ASSERT_TRUE(rendered.has_value());
  EXPECT_THAT(as_text(std::span(output).first(rendered->bytes)), testing::HasSubstr("valid"));

  auto resized = grid.resized(12, 3);
  ASSERT_TRUE(resized.has_value());
  std::ranges::fill(output, std::byte{});
  const auto resized_rendered =
      resized->render_ansi(output, {.rectangle = {.columns = 12, .rows = 3}, .force_full = true});
  ASSERT_TRUE(resized_rendered.has_value());
  EXPECT_THAT(as_text(std::span(output).first(resized_rendered->bytes)),
              testing::HasSubstr("valid"));
}

TEST(GridTest, ReplacesRowsAndStyleTableInOneTransaction) {
  auto created = Grid::create(5, 1);
  ASSERT_TRUE(created.has_value());
  auto grid = std::move(*created);
  GridPatch initial;
  initial.styles = std::vector<GridStyle>(2);
  initial.rows.push_back({.runs = {{.text = "old", .column = 0, .style = 1}}, .row = 0});
  ASSERT_TRUE(grid.apply(std::move(initial)).has_value());

  GridPatch replacement;
  replacement.styles = std::vector<GridStyle>(1);
  replacement.rows.push_back({.runs = {{.text = "new", .column = 0, .style = 0}}, .row = 0});
  EXPECT_TRUE(grid.apply(std::move(replacement)).has_value());
}

TEST(PaneCompositionTest, ComposesRetainedGridBesidePaneAndArbitratesGridCursor) {
  auto terminal = make_terminal(5, 2);
  write_text(terminal, "pane");
  auto created = Grid::create(5, 2);
  ASSERT_TRUE(created.has_value());
  auto grid = std::move(*created);
  GridPatch patch;
  patch.rows.push_back({.runs = {{.text = "grid", .column = 0, .style = 0}}, .row = 0});
  patch.cursor = GridCursor{.column = 4, .row = 0, .visible = true};
  ASSERT_TRUE(grid.apply(std::move(patch)).has_value());
  const std::array panes{PaneSurface{
      .terminal = &terminal,
      .rectangle = {.column = 0, .row = 0, .columns = 5, .rows = 2},
  }};
  const std::array grids{GridSurface{.grid = &grid,
                                     .rectangle = {.column = 5, .row = 0, .columns = 5, .rows = 2},
                                     .focused = true}};
  std::array<std::byte, 8'192> output{};

  const auto composed =
      compose_scene({.panes = panes, .grids = grids}, {.columns = 10, .rows = 2}, output, true);

  ASSERT_TRUE(composed.has_value());
  const auto encoded = as_text(std::span(output).first(composed->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("pane"));
  EXPECT_THAT(encoded, testing::HasSubstr("grid"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[?25h"));

  write_text(terminal, "x");
  std::ranges::fill(output, std::byte{});
  const auto incremental =
      compose_scene({.panes = panes, .grids = grids}, {.columns = 10, .rows = 2}, output, false);
  ASSERT_TRUE(incremental.has_value());
  const auto incremental_encoded = as_text(std::span(output).first(incremental->bytes));
  EXPECT_THAT(incremental_encoded, testing::HasSubstr("\x1B[1;10H"));
  EXPECT_LT(incremental_encoded.find('x'), incremental_encoded.rfind("\x1B[1;10H"));
}

TEST(PaneCompositionTest, RepairsContentBelowTransparentGridDamage) {
  auto terminal = make_terminal(5, 1);
  write_text(terminal, "under");
  auto created = Grid::create(5, 1);
  ASSERT_TRUE(created.has_value());
  auto grid = std::move(*created);
  GridPatch initial;
  initial.rows.push_back({.runs = {{.text = "TOP", .column = 0}}, .row = 0});
  ASSERT_TRUE(grid.apply(std::move(initial)).has_value());
  const std::array panes{
      PaneSurface{.terminal = &terminal, .rectangle = {.columns = 5, .rows = 1}, .focused = true}};
  const std::array grids{
      GridSurface{.grid = &grid, .rectangle = {.columns = 5, .rows = 1}, .opaque = false}};
  std::array<std::byte, 8'192> output{};
  ASSERT_TRUE(
      compose_scene({.panes = panes, .grids = grids}, {.columns = 5, .rows = 1}, output, true)
          .has_value());

  GridPatch cleared;
  cleared.rows.push_back({.runs = {}, .row = 0});
  ASSERT_TRUE(grid.apply(std::move(cleared)).has_value());
  std::ranges::fill(output, std::byte{});
  const auto repaired =
      compose_scene({.panes = panes, .grids = grids}, {.columns = 5, .rows = 1}, output, false);
  ASSERT_TRUE(repaired.has_value());
  const auto encoded = as_text(std::span(output).first(repaired->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("under"));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("TOP")));
}

TEST(PaneCompositionTest, FocusedOccludedGridCannotRepaintUpperLayer) {
  auto lower = Grid::create(8, 1);
  auto upper = Grid::create(8, 1);
  ASSERT_TRUE(lower.has_value());
  ASSERT_TRUE(upper.has_value());
  GridPatch bottom;
  bottom.rows.push_back({.runs = {{.text = "lower111"}}, .row = 0});
  bottom.cursor = GridCursor{.visible = true};
  ASSERT_TRUE(lower->apply(std::move(bottom)).has_value());
  GridPatch top;
  top.rows.push_back({.runs = {{.text = "UPPER111"}}, .row = 0});
  ASSERT_TRUE(upper->apply(std::move(top)).has_value());
  const std::array grids{
      GridSurface{.grid = &*lower, .rectangle = {.columns = 8, .rows = 1}, .focused = true},
      GridSurface{.grid = &*upper, .rectangle = {.columns = 8, .rows = 1}}};
  auto oracle = make_terminal(8, 1);
  std::array<std::byte, 8192> output{};
  const auto full =
      compose_scene({.panes = {}, .grids = grids}, {.columns = 8, .rows = 1}, output, true);
  ASSERT_TRUE(full.has_value());
  oracle.write(std::span(output).first(full->bytes));
  expect_grid_screen(oracle, "UPPER111");
  ASSERT_TRUE(oracle.inspection().has_value());
  EXPECT_FALSE(oracle.inspection()->cursor_visible);
  EXPECT_TRUE(lower->damaged());

  GridPatch hidden;
  hidden.rows.push_back({.runs = {{.text = "lower222"}}, .row = 0});
  hidden.cursor = GridCursor{.column = 1, .visible = true};
  ASSERT_TRUE(lower->apply(std::move(hidden)).has_value());
  const auto incremental =
      compose_scene({.panes = {}, .grids = grids}, {.columns = 8, .rows = 1}, output, false);
  ASSERT_TRUE(incremental.has_value());
  oracle.write(std::span(output).first(incremental->bytes));
  expect_grid_screen(oracle, "UPPER111");
  EXPECT_FALSE(oracle.inspection()->cursor_visible);
  EXPECT_TRUE(lower->damaged());

  const auto exhausted = compose_scene({.panes = {}, .grids = grids}, {.columns = 8, .rows = 1},
                                       std::span(output).first(full->bytes - 1U), true);
  ASSERT_FALSE(exhausted.has_value());
  EXPECT_EQ(exhausted.error(), CompositionError::output_exhausted);
  const auto repaired =
      compose_scene({.panes = {}, .grids = grids}, {.columns = 8, .rows = 1}, output, false);
  ASSERT_TRUE(repaired.has_value());
  oracle.write(std::span(output).first(repaired->bytes));
  expect_grid_screen(oracle, "UPPER111");
  EXPECT_FALSE(oracle.inspection()->cursor_visible);

  const auto removed = compose_scene({.panes = {}, .grids = std::span(grids).first(1)},
                                     {.columns = 8, .rows = 1}, output, false);
  ASSERT_TRUE(removed.has_value());
  oracle.write(std::span(output).first(removed->bytes));
  expect_grid_screen(oracle, "lower222");
  EXPECT_TRUE(oracle.inspection()->cursor_visible);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(PaneCompositionTest, GridCursorRespectsPartialCoverageAndTransparentRuns) {
  auto lower = Grid::create(8, 1);
  auto upper = Grid::create(3, 1);
  ASSERT_TRUE(lower.has_value());
  ASSERT_TRUE(upper.has_value());
  GridPatch bottom;
  bottom.rows.push_back({.runs = {{.text = "abcdefgh"}}, .row = 0});
  bottom.cursor = GridCursor{.column = 4, .visible = true};
  ASSERT_TRUE(lower->apply(std::move(bottom)).has_value());
  GridPatch top;
  top.rows.push_back({.runs = {{.text = "TOP"}}, .row = 0});
  ASSERT_TRUE(upper->apply(std::move(top)).has_value());
  std::array grids{
      GridSurface{.grid = &*lower, .rectangle = {.columns = 8, .rows = 1}, .focused = true},
      GridSurface{.grid = &*upper, .rectangle = {.column = 3, .columns = 3, .rows = 1}}};
  auto oracle = make_terminal(8, 1);
  std::array<std::byte, 8192> output{};
  const auto full =
      compose_scene({.panes = {}, .grids = grids}, {.columns = 8, .rows = 1}, output, true);
  ASSERT_TRUE(full.has_value());
  oracle.write(std::span(output).first(full->bytes));
  expect_grid_screen(oracle, "abcTOPgh");
  EXPECT_FALSE(oracle.inspection()->cursor_visible);
  GridPatch cursor;
  cursor.cursor = GridCursor{.column = 1, .visible = true};
  ASSERT_TRUE(lower->apply(std::move(cursor)).has_value());
  const auto moved =
      compose_scene({.panes = {}, .grids = grids}, {.columns = 8, .rows = 1}, output, false);
  ASSERT_TRUE(moved.has_value());
  EXPECT_EQ(moved->rows, 0U);
  oracle.write(std::span(output).first(moved->bytes));
  expect_grid_screen(oracle, "abcTOPgh");
  EXPECT_TRUE(oracle.inspection()->cursor_visible);

  grids.back().opaque = false;
  for (const bool painted : {false, true}) {
    GridPatch coverage;
    coverage.rows.push_back(
        {.runs = painted ? std::vector<GridRun>{{.text = "TOP"}} : std::vector<GridRun>{},
         .row = 0});
    ASSERT_TRUE(upper->apply(std::move(coverage)).has_value());
    GridPatch position;
    position.cursor = GridCursor{.column = 4, .visible = true};
    ASSERT_TRUE(lower->apply(std::move(position)).has_value());
    const auto frame =
        compose_scene({.panes = {}, .grids = grids}, {.columns = 8, .rows = 1}, output, false);
    ASSERT_TRUE(frame.has_value());
    oracle.write(std::span(output).first(frame->bytes));
    expect_grid_screen(oracle, painted ? "abcTOPgh" : "abcdefgh");
    EXPECT_EQ(oracle.inspection()->cursor_visible, !painted);
  }
}

class PaneCursorOcclusionTest : public testing::Test {
public:
  void SetUp() override {
    GridPatch patch;
    patch.rows.push_back({.runs = {{.text = "TOP"}}, .row = 0});
    ASSERT_TRUE(grid.apply(std::move(patch)).has_value());
    grids.front().grid = &grid;
    write_text(child, "abcdefgh\r\nijklmnop\r\nqrstuvwx\x1b[2;5H\x1b[1 q"
                      "\x1b]12;#123456\x1b\\");
  }

  // GoogleTest assertions inflate the measured branch count.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void frame(const bool full, const bool visible, const std::uint16_t column,
             const std::uint16_t row, const std::string_view middle,
             const bool cursor_only = false) {
    const auto canonical = child.inspection();
    ASSERT_TRUE(canonical.has_value());
    const auto result = compose_scene({.panes = std::span(&pane, 1), .grids = visible_grids},
                                      {.columns = 12, .rows = 5}, output, full, {}, modes);
    ASSERT_TRUE(result.has_value());
    modes = result->outer_modes;
    if (cursor_only) {
      EXPECT_EQ(result->rows, 0U);
      EXPECT_FALSE(grid.damaged());
    }
    oracle.write(std::span(output).first(result->bytes));
    const auto physical = oracle.inspection();
    ASSERT_TRUE(physical.has_value());
    EXPECT_EQ(physical->cursor_visible, visible);
    if (visible) {
      EXPECT_EQ(physical->cursor_column, column);
      EXPECT_EQ(physical->cursor_row, row);
    }
    auto reference = make_terminal(12, 5);
    write_text(reference, "\x1b[2;3Habcdefgh\x1b[3;3H");
    write_text(reference, middle);
    write_text(reference, "\x1b[4;3Hqrstuvwx");
    std::array<std::byte, 1024> actual{};
    std::array<std::byte, 1024> expected{};
    const auto actual_size = oracle.format_screen(vt::ScreenFormat::plain, actual);
    const auto expected_size = reference.format_screen(vt::ScreenFormat::plain, expected);
    ASSERT_TRUE(actual_size.has_value());
    ASSERT_TRUE(expected_size.has_value());
    EXPECT_EQ(as_text(std::span(actual).first(*actual_size)),
              as_text(std::span(expected).first(*expected_size)));
    const auto after = child.inspection();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->cursor_visible, canonical->cursor_visible);
    EXPECT_EQ(after->cursor_column, canonical->cursor_column);
    EXPECT_EQ(after->cursor_row, canonical->cursor_row);
    EXPECT_TRUE(pane.focused);
    EXPECT_FALSE(grids.front().focused);
    EXPECT_EQ(result->outer_modes, OuterModeProjection::button_mouse);
  }

  vt::Terminal child{make_terminal(8, 3)};
  vt::Terminal oracle{make_terminal(12, 5)};
  Grid grid{[] {
    auto created = Grid::create(3, 1);
    EXPECT_TRUE(created.has_value());
    return std::move(*created);
  }()};
  PaneSurface pane{.terminal = &child,
                   .rectangle = {.column = 2, .row = 1, .columns = 8, .rows = 3},
                   .focused = true};
  std::array<GridSurface, 1> grids{
      GridSurface{.rectangle = {.column = 5, .row = 2, .columns = 3, .rows = 1}}};
  std::span<const GridSurface> visible_grids{grids};
  std::array<std::byte, 8192> output{};
  std::optional<OuterModeProjection> modes;
};

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(PaneCursorOcclusionTest, FocusedSurfaceBlockYieldsToPaneCursorShape) {
  write_text(child, "\x1b[6 q");
  GridPatch cursor;
  cursor.cursor = GridCursor{.column = 1, .visible = true};
  ASSERT_TRUE(grid.apply(std::move(cursor)).has_value());
  const auto present = [this](const bool surface_focused, const bool full) {
    pane.focused = !surface_focused;
    grids.front().focused = surface_focused;
    const auto composed = compose_scene({.panes = std::span(&pane, 1), .grids = grids},
                                        {.columns = 12, .rows = 5}, output, full, {}, modes);
    EXPECT_TRUE(composed.has_value());
    modes = composed.value_or(CompositionResult{}).outer_modes;
    oracle.write(std::span(output).first(composed.value_or(CompositionResult{}).bytes));
    oracle.invalidate_ansi_render_state();
    const auto projected = oracle.render_ansi(output, true);
    EXPECT_TRUE(projected.has_value());
    return std::string(as_text(std::span(output).first(projected.value_or({}).bytes)));
  };

  EXPECT_THAT(present(false, true), testing::HasSubstr("\x1b[6 q"));
  EXPECT_THAT(present(true, false), testing::HasSubstr("\x1b[2 q"));
  EXPECT_THAT(present(true, false), testing::HasSubstr("\x1b[2 q"));
  EXPECT_THAT(present(false, false), testing::HasSubstr("\x1b[6 q"));
}

TEST_F(PaneCursorOcclusionTest, OpaquePartialOverlayHidesCursorWithoutTakingFocus) {
  frame(true, false, 6, 2, "ijkTOPop");
  frame(false, false, 6, 2, "ijkTOPop", true);
}

TEST_F(PaneCursorOcclusionTest, TransparentPaintAndHoleUseRetainedCoverage) {
  grids.front().opaque = false;
  GridPatch patch;
  patch.rows.push_back({.runs = {{.text = "T"}, {.text = "P", .column = 2}}, .row = 0});
  ASSERT_TRUE(grid.apply(std::move(patch)).has_value());
  frame(true, true, 6, 2, "ijkTmPop");
  write_text(child, "\x1b[2;4H");
  frame(false, false, 5, 2, "ijkTmPop", true);
  write_text(child, "\x1b[2;5H");
  frame(false, true, 6, 2, "ijkTmPop", true);
  write_text(child, "\x1b[2;6H");
  frame(false, false, 7, 2, "ijkTmPop", true);
}

TEST_F(PaneCursorOcclusionTest, TransparentPaintClearingRestoresCursorIncrementally) {
  grids.front().opaque = false;
  frame(true, false, 6, 2, "ijkTOPop");
  GridPatch cleared;
  cleared.rows.push_back({.runs = {}, .row = 0});
  ASSERT_TRUE(grid.apply(std::move(cleared)).has_value());
  frame(false, true, 6, 2, "ijklmnop");
  frame(false, true, 6, 2, "ijklmnop", true);
}

TEST_F(PaneCursorOcclusionTest, CursorOnlyMovementEntersAndLeavesUnchangedOpaqueOverlay) {
  write_text(child, "\x1b[2;2H");
  frame(true, true, 3, 2, "ijkTOPop");
  write_text(child, "\x1b[2;5H");
  frame(false, false, 6, 2, "ijkTOPop", true);
  write_text(child, "\x1b[2;7H");
  frame(false, true, 8, 2, "ijkTOPop", true);
}

TEST_F(PaneCursorOcclusionTest, StructuralRepairRestoresCursorAndRemovesOverlayGhosts) {
  frame(true, false, 6, 2, "ijkTOPop");
  // Structural Scene changes request full repair through the existing Attachment policy.
  grids.front().rectangle.column = 2;
  frame(true, true, 6, 2, "TOPlmnop");
  grids.front().rectangle.column = 5;
  frame(true, false, 6, 2, "ijkTOPop");
  visible_grids = {};
  frame(true, true, 6, 2, "ijklmnop");
  frame(false, true, 6, 2, "ijklmnop", true);
  oracle.invalidate_ansi_render_state();
  const auto projected = oracle.render_ansi(output, true);
  ASSERT_TRUE(projected.has_value());
  const auto encoded = as_text(std::span(output).first(projected->bytes));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1b[1 q"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1b]12;#123456\x1b\\"));
}

TEST_F(PaneCursorOcclusionTest, CanonicallyHiddenCursorStaysHiddenAfterUncovering) {
  write_text(child, "\x1b[?25l");
  frame(true, false, 6, 2, "ijkTOPop");
  visible_grids = {};
  frame(true, false, 6, 2, "ijklmnop");
  frame(false, false, 6, 2, "ijklmnop", true);
}

TEST_F(PaneCursorOcclusionTest, OffViewportCursorStaysHiddenAfterUncovering) {
  write_text(child, "\x1b[3;1H\r\nlast\r\nline\r\nend");
  child.scroll_viewport(vt::ViewportScroll::top);
  const auto metadata = child.update_render_state();
  ASSERT_TRUE(metadata.has_value());
  ASSERT_FALSE(metadata->cursor_in_viewport);
  frame(true, false, 0, 0, "ijkTOPop");
  visible_grids = {};
  frame(true, false, 0, 0, "ijklmnop");
}

TEST_F(PaneCursorOcclusionTest, CopyModeOverrideRatherThanCanonicalCursorControlsCoverage) {
  pane.cursor_override = true;
  pane.cursor_override_column = 1;
  pane.cursor_override_row = 1;
  write_text(child, "\x1b[?25l");
  frame(true, true, 3, 2, "ijkTOPop");
  pane.cursor_override_column = 4;
  frame(false, false, 6, 2, "ijkTOPop", true);
  pane.cursor_override_column = 6;
  frame(false, true, 8, 2, "ijkTOPop", true);
  oracle.invalidate_ansi_render_state();
  const auto projected = oracle.render_ansi(output, true);
  ASSERT_TRUE(projected.has_value());
  EXPECT_THAT(as_text(std::span(output).first(projected->bytes)), testing::HasSubstr("\x1b[2 q"));
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(PaneCursorOcclusionTest, StatusOffsetAndNativePromptKeepCursorPrecedence) {
  const std::array tabs{StatusTab{.number = 1, .title = "sh", .active = true}};
  StatusLine status{.session_name = {},
                    .tabs = tabs,
                    .prompt_target = StatusPromptTarget::none,
                    .prompt_feedback = StatusPromptFeedback::none,
                    .prompt_value = {},
                    .input_context = {}};
  for (const bool full : {true, false}) {
    const auto covered = compose_scene({.panes = std::span(&pane, 1), .grids = grids},
                                       {.columns = 12, .rows = 5}, output, full, status, modes);
    ASSERT_TRUE(covered.has_value());
    modes = covered->outer_modes;
    oracle.write(std::span(output).first(covered->bytes));
    ASSERT_TRUE(oracle.inspection().has_value());
    EXPECT_FALSE(oracle.inspection()->cursor_visible);
    EXPECT_TRUE(pane.focused);
    EXPECT_FALSE(grids.front().focused);
  }
  status.prompt_target = StatusPromptTarget::command_line;
  status.prompt_value = "x";
  status.prompt_cursor = 1;
  const auto prompt = compose_scene({.panes = std::span(&pane, 1), .grids = grids},
                                    {.columns = 12, .rows = 5}, output, false, status, modes);
  ASSERT_TRUE(prompt.has_value());
  oracle.write(std::span(output).first(prompt->bytes));
  EXPECT_TRUE(oracle.inspection()->cursor_visible);
  EXPECT_EQ(oracle.inspection()->cursor_column, 2U);
  EXPECT_EQ(oracle.inspection()->cursor_row, 0U);

  status.prompt_target = StatusPromptTarget::none;
  status.prompt_value = {};
  status.prompt_cursor = 0;
  const auto restored =
      compose_scene({.panes = std::span(&pane, 1), .grids = {}}, {.columns = 12, .rows = 5}, output,
                    true, status, prompt->outer_modes);
  ASSERT_TRUE(restored.has_value());
  oracle.write(std::span(output).first(restored->bytes));
  EXPECT_TRUE(oracle.inspection()->cursor_visible);
  EXPECT_EQ(oracle.inspection()->cursor_column, 6U);
  EXPECT_EQ(oracle.inspection()->cursor_row, 3U);
  oracle.invalidate_ansi_render_state();
  const auto projected = oracle.render_ansi(output, true);
  ASSERT_TRUE(projected.has_value());
  EXPECT_THAT(as_text(std::span(output).first(projected->bytes)), testing::HasSubstr("\x1b[1 q"));
}

TEST_F(PaneCursorOcclusionTest, ExhaustionInvalidatesAndRepairsWithoutStaleSuppression) {
  frame(true, false, 6, 2, "ijkTOPop");
  const auto full = compose_scene({.panes = std::span(&pane, 1), .grids = grids},
                                  {.columns = 12, .rows = 5}, output, true);
  ASSERT_TRUE(full.has_value());
  const auto exhausted =
      compose_scene({.panes = std::span(&pane, 1), .grids = grids}, {.columns = 12, .rows = 5},
                    std::span(output).first(full->bytes - 1U), true);
  ASSERT_FALSE(exhausted.has_value());
  EXPECT_EQ(exhausted.error(), CompositionError::output_exhausted);
  EXPECT_TRUE(grid.damaged());
  frame(false, false, 6, 2, "ijkTOPop");
  write_text(child, "\x1b[2;2H");
  frame(false, true, 3, 2, "ijkTOPop", true);
}

TEST(PaneCompositionTest, EnforcesPaneAndOutputBounds) {
  auto terminal = make_terminal(1, 1);
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = 1, .rows = 1},
  };
  std::vector<PaneSurface> excessive(limits::panes_hard_max + 1U, pane);
  std::array<std::byte, 1> output{};

  auto result = compose_frame(excessive, {.columns = 1, .rows = 1}, output, false);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), CompositionError::too_many_panes);

  result = compose_frame(std::span(&pane, 1), {.columns = 1, .rows = 1}, output, true);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), CompositionError::output_exhausted);
}

} // namespace
} // namespace lemma::render
