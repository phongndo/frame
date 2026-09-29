#include "core/command_line.hpp"

#include "frame/command.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string_view>

namespace frame::core {
namespace {
using namespace std::string_view_literals;

constexpr CommandLineContext context{
    .session = SessionId::from_parts(1, 2),
    .tab = TabId::from_parts(3, 4),
    .pane = PaneId::from_parts(5, 6),
};

struct ParseCase final {
  std::string_view text;
  api::CommandKind kind;
};

constexpr std::array parse_cases{
    ParseCase{.text = "session rename renamed", .kind = api::CommandKind::session_rename},
    ParseCase{.text = "session kill", .kind = api::CommandKind::session_kill},
    ParseCase{.text = "tab select 1", .kind = api::CommandKind::tab_select},
    ParseCase{.text = "tab move 2", .kind = api::CommandKind::tab_move},
    ParseCase{.text = "tab rename", .kind = api::CommandKind::tab_rename},
    ParseCase{.text = "tab kill", .kind = api::CommandKind::tab_kill},
    ParseCase{.text = "pane focus 7:8", .kind = api::CommandKind::pane_focus},
    ParseCase{.text = "pane swap 7:8", .kind = api::CommandKind::pane_swap},
    ParseCase{.text = "pane resize --down 2", .kind = api::CommandKind::pane_resize},
    ParseCase{.text = "pane zoom --on", .kind = api::CommandKind::pane_zoom},
    ParseCase{.text = "pane kill", .kind = api::CommandKind::pane_kill},
};

TEST(CommandLineTest, ParsesProcShapedCommandsWithImplicitCurrentTargets) {
  const auto split = parse_command_line("pane split --right", context);
  ASSERT_TRUE(split.has_value());
  EXPECT_EQ(split->kind, CommandLineActionKind::command);
  EXPECT_EQ(split->command.kind, api::CommandKind::pane_split);
  EXPECT_EQ(split->command.session.id, context.session);
  EXPECT_EQ(split->command.pane.id, context.pane);
  EXPECT_EQ(split->command.direction, api::Direction::right);

  const auto tab = parse_command_line("tab new --title 'unit tests'", context);
  ASSERT_TRUE(tab.has_value());
  EXPECT_EQ(tab->command.kind, api::CommandKind::tab_new);
  EXPECT_EQ(tab->command.title, "unit tests");

  const auto resize = parse_command_line("pane resize --left 5", context);
  ASSERT_TRUE(resize.has_value());
  EXPECT_EQ(resize->command.kind, api::CommandKind::pane_resize);
  EXPECT_EQ(resize->command.direction, api::Direction::left);
  EXPECT_EQ(resize->command.amount, 5);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(CommandLineTest, ParsesEveryCatalogMutationToTypedCommands) {
  for (const auto& parse_case : parse_cases) {
    const auto parsed = parse_command_line(parse_case.text, context);
    ASSERT_TRUE(parsed.has_value()) << parse_case.text;
    EXPECT_EQ(parsed->kind, CommandLineActionKind::command) << parse_case.text;
    EXPECT_EQ(parsed->command.kind, parse_case.kind) << parse_case.text;
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(CommandLineTest, ParsesSwitchAliasesWithoutTreatingThemAsNestedSessions) {
  for (const auto text :
       {"switch tests"sv, "attach tests"sv, "session switch tests"sv, "session attach tests"sv}) {
    const auto parsed = parse_command_line(text, context);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, CommandLineActionKind::switch_session);
    EXPECT_EQ(parsed->switch_session.name, "tests");
  }
}

TEST(CommandLineTest, ResolvesRegisteredCommandsUsingTheSameLiteralArgumentGrammar) {
  const std::array commands{
      extension::CommandDescriptor{.name = "project.open", .description = "Open"}};
  const auto parsed = parse_command_line("project.open 'two words' '' $HOME", context, commands);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->kind, CommandLineActionKind::hosted);
  EXPECT_EQ(parsed->hosted_command, "project.open");
  EXPECT_EQ(parsed->arguments, (std::vector<std::string>{"two words", "", "$HOME"}));
  EXPECT_EQ(parse_command_line("project.open", context).error(), CommandLineError::unknown_command);
  EXPECT_EQ(parse_command_line("project.open 'unfinished", context, commands).error(),
            CommandLineError::invalid_syntax);
  EXPECT_EQ(parse_command_line("pane split --right", context, commands)->kind,
            CommandLineActionKind::command);
}

TEST(CommandLineTest, RejectsUnknownMalformedAndUnclosedInput) {
  EXPECT_EQ(parse_command_line("frobnicate", context).error(), CommandLineError::unknown_command);
  EXPECT_EQ(parse_command_line("pane resize --left 0", context).error(),
            CommandLineError::invalid_syntax);
  EXPECT_EQ(parse_command_line("tab new --title 'unfinished", context).error(),
            CommandLineError::invalid_syntax);
  EXPECT_EQ(parse_command_line("detach now", context).error(), CommandLineError::invalid_syntax);
}

TEST(CommandLineTest, CompletesCatalogWordsAndDynamicSessionNames) {
  const auto root = command_line_completion_query("pan", 3);
  EXPECT_EQ(root.kind, CommandLineCompletionKind::root);
  const auto root_completion =
      complete_command_line(root.prefix, command_line_static_completions(root.kind));
  EXPECT_EQ(root_completion.replacement, "pane");
  EXPECT_TRUE(root_completion.append_space);

  const auto session = command_line_completion_query("switch te", 9);
  EXPECT_EQ(session.kind, CommandLineCompletionKind::session);
  const std::array candidates{"tests"sv, "work"sv};
  const auto session_completion = complete_command_line(session.prefix, candidates);
  EXPECT_EQ(session_completion.replacement, "tests");
  EXPECT_TRUE(session_completion.append_space);

  const auto operation = command_line_completion_query("pane sp", 7);
  EXPECT_EQ(operation.kind, CommandLineCompletionKind::pane_operation);
  const auto operation_completion =
      complete_command_line(operation.prefix, command_line_static_completions(operation.kind));
  EXPECT_EQ(operation_completion.replacement, "split");
}

} // namespace
} // namespace frame::core
