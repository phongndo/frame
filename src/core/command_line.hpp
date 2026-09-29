#ifndef FRAME_CORE_COMMAND_LINE_HPP
#define FRAME_CORE_COMMAND_LINE_HPP

#include "api/command.hpp"
#include "extension/commands.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frame::core {

inline constexpr std::size_t command_line_bytes_max = limits::command_line_bytes_max;
inline constexpr std::size_t command_line_words_max = 64;

enum class CommandLineActionKind : std::uint8_t {
  command,
  switch_session,
  detach,
  hosted,
};

struct CommandLineContext final {
  SessionId session;
  TabId tab;
  PaneId pane;
};

struct CommandLineAction final {
  api::Command command;
  api::SessionSelector switch_session;
  std::string hosted_command;
  std::vector<std::string> arguments;
  CommandLineActionKind kind{CommandLineActionKind::command};
};

enum class CommandLineError : std::uint8_t {
  invalid_syntax,
  unknown_command,
  capacity,
};

// Parses the bounded interactive grammar. It deliberately performs no shell expansion: quotes and
// backslashes only group literal argv, title, and path text.
[[nodiscard]] auto parse_command_line(std::string_view line, CommandLineContext context,
                                      std::span<const extension::CommandDescriptor> commands = {})
    -> std::expected<CommandLineAction, CommandLineError>;

enum class CommandLineCompletionKind : std::uint8_t {
  none,
  root,
  session_operation,
  tab_operation,
  pane_operation,
  session,
  tab,
  pane,
  tab_new_option,
  pane_split_option,
  pane_resize_direction,
  pane_zoom_option,
  focus_policy,
};

struct CommandLineCompletionQuery final {
  CommandLineCompletionKind kind{CommandLineCompletionKind::none};
  std::size_t replace_begin{0};
  std::size_t replace_end{0};
  std::string_view prefix;
};

// Completion is lexical and bounded. Dynamic session, tab, and pane candidates are supplied by the
// reactor; every other candidate comes from the native command catalog.
[[nodiscard]] auto command_line_completion_query(std::string_view line, std::size_t cursor) noexcept
    -> CommandLineCompletionQuery;
[[nodiscard]] auto command_line_static_completions(CommandLineCompletionKind kind) noexcept
    -> std::span<const std::string_view>;

struct CommandLineCompletion final {
  std::string replacement;
  std::size_t matches{0};
  bool append_space{false};
};

// Returns the unique match, a shared-prefix extension, or only the ambiguous match count.
[[nodiscard]] auto complete_command_line(std::string_view prefix,
                                         std::span<const std::string_view> candidates)
    -> CommandLineCompletion;

} // namespace frame::core

#endif // FRAME_CORE_COMMAND_LINE_HPP
