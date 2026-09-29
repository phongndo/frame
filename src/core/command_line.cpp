#include "core/command_line.hpp"

#include "api/command.hpp"
#include "extension/commands.hpp"
#include "frame/command.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace frame::core {
namespace {
using namespace std::string_view_literals;

// Every dynamic index below is bounded by the token, line, or fixed-candidate size checked in the
// same parser branch. Subscript syntax keeps the grammar readable without introducing throwing
// access into its cold error path.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)

struct CommandDescriptor final {
  std::string_view path;
};

// One catalog owns discovery. Future descriptor sources project into this same ordered view while
// their handlers compile to typed Frame commands.
constexpr std::array command_catalog{
    CommandDescriptor{.path = "switch"},
    CommandDescriptor{.path = "attach"},
    CommandDescriptor{.path = "detach"},
    CommandDescriptor{.path = "reload"},
    CommandDescriptor{.path = "paste-image"},
    CommandDescriptor{.path = "session"},
    CommandDescriptor{.path = "session switch"},
    CommandDescriptor{.path = "session attach"},
    CommandDescriptor{.path = "session rename"},
    CommandDescriptor{.path = "session kill"},
    CommandDescriptor{.path = "tab"},
    CommandDescriptor{.path = "tab new"},
    CommandDescriptor{.path = "tab select"},
    CommandDescriptor{.path = "tab move"},
    CommandDescriptor{.path = "tab rename"},
    CommandDescriptor{.path = "tab kill"},
    CommandDescriptor{.path = "pane"},
    CommandDescriptor{.path = "pane split"},
    CommandDescriptor{.path = "pane focus"},
    CommandDescriptor{.path = "pane swap"},
    CommandDescriptor{.path = "pane resize"},
    CommandDescriptor{.path = "pane zoom"},
    CommandDescriptor{.path = "pane kill"},
};

struct CatalogCompletions final {
  std::array<std::string_view, command_catalog.size()> values{};
  std::size_t size{0};

  [[nodiscard]] constexpr auto view() const noexcept -> std::span<const std::string_view> {
    return std::span(values).first(size);
  }
};

// Catalog and completion storage are fixed and bounded.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] constexpr auto discover_catalog_completions(const std::string_view parent) noexcept
    -> CatalogCompletions {
  CatalogCompletions completions;
  for (const auto& descriptor : command_catalog) {
    auto remaining = descriptor.path;
    if (!parent.empty()) {
      if (!remaining.starts_with(parent) || remaining.size() <= parent.size() ||
          remaining[parent.size()] != ' ') {
        continue;
      }
      remaining.remove_prefix(parent.size() + 1U);
    }
    const auto candidate = remaining.substr(0, remaining.find(' '));
    if (std::ranges::find(completions.view(), candidate) == completions.view().end()) {
      completions.values[completions.size++] = candidate;
    }
  }
  return completions;
}

constexpr auto root_completions = discover_catalog_completions({});
constexpr auto session_completions = discover_catalog_completions("session");
constexpr auto tab_completions = discover_catalog_completions("tab");
constexpr auto pane_completions = discover_catalog_completions("pane");
constexpr std::array tab_new_completions{"--title"sv, "--cwd"sv, "--hold"sv, "--focus"sv, "--"sv};
constexpr std::array pane_split_completions{"--right"sv, "--down"sv,  "--cwd"sv,
                                            "--hold"sv,  "--focus"sv, "--"sv};
constexpr std::array resize_completions{"--left"sv, "--right"sv, "--up"sv, "--down"sv};
constexpr std::array zoom_completions{"--on"sv, "--off"sv};
constexpr std::array focus_completions{"created"sv, "preserve"sv};

struct TokenizedLine final {
  std::vector<std::string> words;
  bool valid{true};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto tokenize(const std::string_view line) -> TokenizedLine {
  TokenizedLine result;
  result.words.reserve(8);
  std::string word;
  bool token_started = false;
  bool single_quote = false;
  bool double_quote = false;
  bool escaped = false;
  for (const char character : line) {
    if (escaped) {
      word.push_back(character);
      token_started = true;
      escaped = false;
      continue;
    }
    if (character == '\\' && !single_quote) {
      escaped = true;
      token_started = true;
      continue;
    }
    if (character == '\'' && !double_quote) {
      single_quote = !single_quote;
      token_started = true;
      continue;
    }
    if (character == '"' && !single_quote) {
      double_quote = !double_quote;
      token_started = true;
      continue;
    }
    const auto byte = static_cast<unsigned char>(character);
    if (!single_quote && !double_quote && (byte == ' ' || byte == '\t')) {
      if (token_started) {
        result.words.push_back(std::move(word));
        word.clear();
        token_started = false;
        if (result.words.size() > command_line_words_max) {
          result.valid = false;
          return result;
        }
      }
      continue;
    }
    word.push_back(character);
    token_started = true;
  }
  if (escaped || single_quote || double_quote) {
    result.valid = false;
    return result;
  }
  if (token_started) {
    result.words.push_back(std::move(word));
  }
  result.valid = result.words.size() <= command_line_words_max;
  return result;
}

template <typename Id>
[[nodiscard]] auto parse_id(const std::string_view value) -> std::optional<Id> {
  const auto separator = value.find(':');
  if (separator == std::string_view::npos || separator == 0 || separator + 1U == value.size()) {
    return std::nullopt;
  }
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;
  const auto slot_text = value.substr(0, separator);
  const auto generation_text = value.substr(separator + 1U);
  const auto slot_result = std::from_chars(slot_text.begin(), slot_text.end(), slot);
  const auto generation_result =
      std::from_chars(generation_text.begin(), generation_text.end(), generation);
  return slot_result.ec == std::errc{} && slot_result.ptr == slot_text.end() &&
                 generation_result.ec == std::errc{} &&
                 generation_result.ptr == generation_text.end()
             ? Id::try_from_parts(slot, generation)
             : std::nullopt;
}

template <typename Integer>
[[nodiscard]] auto parse_integer(const std::string_view value) -> std::optional<Integer> {
  Integer result{};
  const auto parsed = std::from_chars(value.begin(), value.end(), result);
  return parsed.ec == std::errc{} && parsed.ptr == value.end() ? std::optional{result}
                                                               : std::nullopt;
}

[[nodiscard]] auto session_selector(const std::string_view value)
    -> std::optional<api::SessionSelector> {
  if (const auto id = parse_id<SessionId>(value); id.has_value()) {
    return api::SessionSelector{.id = *id, .name = {}};
  }
  return SessionNameValue::create(value).has_value()
             ? std::optional{api::SessionSelector{.id = {}, .name = std::string(value)}}
             : std::nullopt;
}

[[nodiscard]] auto tab_selector(const std::string_view value) -> std::optional<api::TabSelector> {
  if (const auto id = parse_id<TabId>(value); id.has_value()) {
    return api::TabSelector{.id = *id, .position = 0};
  }
  const auto position = parse_integer<std::uint16_t>(value);
  return position.has_value() && *position > 0 && *position <= command_tab_slots_max
             ? std::optional{api::TabSelector{.id = {}, .position = *position}}
             : std::nullopt;
}

[[nodiscard]] auto pane_selector(const std::string_view value) -> std::optional<api::PaneSelector> {
  const auto id = parse_id<PaneId>(value);
  return id.has_value() ? std::optional{api::PaneSelector{.id = *id}} : std::nullopt;
}

[[nodiscard]] constexpr auto current_session(const CommandLineContext context) noexcept
    -> api::SessionSelector {
  return {.id = context.session, .name = {}};
}

[[nodiscard]] constexpr auto current_tab(const CommandLineContext context) noexcept
    -> api::TabSelector {
  return {.id = context.tab, .position = 0};
}

[[nodiscard]] constexpr auto current_pane(const CommandLineContext context) noexcept
    -> api::PaneSelector {
  return {.id = context.pane};
}

[[nodiscard]] auto invalid() -> std::expected<CommandLineAction, CommandLineError> {
  return std::unexpected(CommandLineError::invalid_syntax);
}

[[nodiscard]] auto parse_focus_policy(const std::string_view value)
    -> std::optional<api::FocusPolicy> {
  if (value == "created") {
    return api::FocusPolicy::created;
  }
  if (value == "preserve") {
    return api::FocusPolicy::preserve;
  }
  return std::nullopt;
}

struct LaunchOptions final {
  std::string title;
  std::string working_directory;
  std::vector<std::string> arguments;
  api::FocusPolicy focus{api::FocusPolicy::created};
  bool hold{false};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto parse_launch_options(const std::span<const std::string> words,
                                        const bool allow_title) -> std::optional<LaunchOptions> {
  LaunchOptions options;
  for (std::size_t index = 0; index < words.size();) {
    const auto& word = words[index];
    if (word == "--") {
      ++index;
      if (index == words.size()) {
        return std::nullopt;
      }
      options.arguments.assign(words.begin() + static_cast<std::ptrdiff_t>(index), words.end());
      break;
    }
    if (allow_title && word == "--title" && index + 1U < words.size()) {
      options.title = words[index + 1U];
      index += 2U;
      continue;
    }
    if (word == "--cwd" && index + 1U < words.size()) {
      options.working_directory = words[index + 1U];
      index += 2U;
      continue;
    }
    if (word == "--hold") {
      options.hold = true;
      ++index;
      continue;
    }
    if (word == "--focus" && index + 1U < words.size()) {
      const auto focus = parse_focus_policy(words[index + 1U]);
      if (!focus.has_value()) {
        return std::nullopt;
      }
      options.focus = *focus;
      index += 2U;
      continue;
    }
    return std::nullopt;
  }
  if ((!options.title.empty() && !TabTitleValue::create(options.title).has_value()) ||
      options.working_directory.size() > limits::working_directory_bytes_max ||
      options.arguments.size() > limits::command_arguments_hard_max) {
    return std::nullopt;
  }
  std::size_t argument_bytes = 0;
  for (const auto& argument : options.arguments) {
    argument_bytes += argument.size();
  }
  return argument_bytes <= limits::command_bytes_hard_max ? std::optional{std::move(options)}
                                                          : std::nullopt;
}

void assign_launch(api::Command& command, LaunchOptions options) {
  command.title = std::move(options.title);
  command.working_directory = std::move(options.working_directory);
  command.arguments = std::move(options.arguments);
  command.hold = options.hold;
  command.focus = options.focus;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto parse_session_command(const std::span<const std::string> words,
                                         const CommandLineContext context)
    -> std::expected<CommandLineAction, CommandLineError> {
  if (words.size() < 2U) {
    return invalid();
  }
  if (words[1] == "switch" || words[1] == "attach") {
    if (words.size() != 3U) {
      return invalid();
    }
    const auto target = session_selector(words[2]);
    if (!target.has_value()) {
      return invalid();
    }
    CommandLineAction action;
    action.kind = CommandLineActionKind::switch_session;
    action.switch_session = *target;
    return action;
  }
  CommandLineAction action;
  action.command.session = current_session(context);
  if (words[1] == "rename" && words.size() == 3U &&
      SessionNameValue::create(words[2]).has_value()) {
    action.command.kind = api::CommandKind::session_rename;
    action.command.name = words[2];
    return action;
  }
  if (words[1] == "kill" && (words.size() == 2U || words.size() == 3U)) {
    if (words.size() == 3U) {
      const auto target = session_selector(words[2]);
      if (!target.has_value()) {
        return invalid();
      }
      action.command.session = *target;
    }
    action.command.kind = api::CommandKind::session_kill;
    return action;
  }
  return words[1] == "switch" || words[1] == "attach" || words[1] == "rename" || words[1] == "kill"
             ? invalid()
             : std::expected<CommandLineAction, CommandLineError>{
                   std::unexpected(CommandLineError::unknown_command)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto parse_tab_command(const std::span<const std::string> words,
                                     const CommandLineContext context)
    -> std::expected<CommandLineAction, CommandLineError> {
  if (words.size() < 2U) {
    return invalid();
  }
  CommandLineAction action;
  auto& command = action.command;
  command.session = current_session(context);
  command.tab = current_tab(context);
  if (words[1] == "new") {
    const auto options = parse_launch_options(words.subspan(2), true);
    if (!options.has_value()) {
      return invalid();
    }
    command.kind = api::CommandKind::tab_new;
    assign_launch(command, *options);
    return action;
  }
  if (words[1] == "select" && words.size() == 3U) {
    const auto target = tab_selector(words[2]);
    if (!target.has_value()) {
      return invalid();
    }
    command.kind = api::CommandKind::tab_select;
    command.tab = *target;
    return action;
  }
  if (words[1] == "move" && (words.size() == 3U || words.size() == 4U)) {
    const auto target = words.size() == 4U ? tab_selector(words[2]) : std::optional{command.tab};
    const auto destination = parse_integer<std::uint16_t>(words.back());
    if (!target.has_value() || !destination.has_value() || *destination == 0 ||
        *destination > command_tab_slots_max) {
      return invalid();
    }
    command.kind = api::CommandKind::tab_move;
    command.tab = *target;
    command.to_position = *destination;
    return action;
  }
  if (words[1] == "rename" && words.size() >= 2U && words.size() <= 4U) {
    std::size_t title_index = 2U;
    if (words.size() == 4U) {
      const auto target = tab_selector(words[2]);
      if (!target.has_value()) {
        return invalid();
      }
      command.tab = *target;
      title_index = 3U;
    }
    command.title = title_index < words.size() ? words[title_index] : std::string{};
    if (!TabTitleValue::create(command.title).has_value()) {
      return invalid();
    }
    command.kind = api::CommandKind::tab_rename;
    return action;
  }
  if (words[1] == "kill" && (words.size() == 2U || words.size() == 3U)) {
    if (words.size() == 3U) {
      const auto target = tab_selector(words[2]);
      if (!target.has_value()) {
        return invalid();
      }
      command.tab = *target;
    }
    command.kind = api::CommandKind::tab_kill;
    return action;
  }
  return words[1] == "new" || words[1] == "select" || words[1] == "move" || words[1] == "rename" ||
                 words[1] == "kill"
             ? invalid()
             : std::expected<CommandLineAction, CommandLineError>{
                   std::unexpected(CommandLineError::unknown_command)};
}

[[nodiscard]] auto parse_split(const std::span<const std::string> arguments,
                               const CommandLineContext context, api::Command& command) -> bool {
  std::size_t index = 0;
  command.pane = current_pane(context);
  if (index < arguments.size()) {
    if (const auto target = pane_selector(arguments[index]); target.has_value()) {
      command.pane = *target;
      ++index;
    }
  }
  if (index >= arguments.size() ||
      (arguments[index] != "--right" && arguments[index] != "--down")) {
    return false;
  }
  command.direction = arguments[index] == "--right" ? api::Direction::right : api::Direction::down;
  ++index;
  const auto options = parse_launch_options(arguments.subspan(index), false);
  if (!options.has_value()) {
    return false;
  }
  assign_launch(command, *options);
  command.kind = api::CommandKind::pane_split;
  return true;
}

[[nodiscard]] auto resize_direction(const std::string_view value) -> std::optional<api::Direction> {
  if (value == "--left") {
    return api::Direction::left;
  }
  if (value == "--right") {
    return api::Direction::right;
  }
  if (value == "--up") {
    return api::Direction::up;
  }
  if (value == "--down") {
    return api::Direction::down;
  }
  return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto parse_pane_command(const std::span<const std::string> words,
                                      const CommandLineContext context)
    -> std::expected<CommandLineAction, CommandLineError> {
  if (words.size() < 2U) {
    return invalid();
  }
  CommandLineAction action;
  auto& command = action.command;
  command.session = current_session(context);
  command.tab = current_tab(context);
  command.pane = current_pane(context);
  if (words[1] == "split") {
    return parse_split(words.subspan(2), context, command)
               ? std::expected<CommandLineAction, CommandLineError>{std::move(action)}
               : invalid();
  }
  if ((words[1] == "focus" || words[1] == "kill") && words.size() <= 3U) {
    if (words.size() == 3U) {
      const auto target = pane_selector(words[2]);
      if (!target.has_value()) {
        return invalid();
      }
      command.pane = *target;
    } else if (words[1] == "focus") {
      return invalid();
    }
    command.kind = words[1] == "focus" ? api::CommandKind::pane_focus : api::CommandKind::pane_kill;
    return action;
  }
  if (words[1] == "swap" && (words.size() == 3U || words.size() == 4U)) {
    const auto source = words.size() == 4U ? pane_selector(words[2]) : std::optional{command.pane};
    const auto other = pane_selector(words.back());
    if (!source.has_value() || !other.has_value()) {
      return invalid();
    }
    command.kind = api::CommandKind::pane_swap;
    command.pane = *source;
    command.other = *other;
    return action;
  }
  if (words[1] == "resize" && (words.size() == 4U || words.size() == 5U)) {
    std::size_t index = 2U;
    if (words.size() == 5U) {
      const auto target = pane_selector(words[index]);
      if (!target.has_value()) {
        return invalid();
      }
      command.pane = *target;
      ++index;
    }
    const auto direction = resize_direction(words[index]);
    const auto amount = parse_integer<std::uint16_t>(words[index + 1U]);
    if (!direction.has_value() || !amount.has_value() || *amount == 0 ||
        *amount > command_resize_amount_max) {
      return invalid();
    }
    command.kind = api::CommandKind::pane_resize;
    command.direction = *direction;
    command.amount = *amount;
    return action;
  }
  if (words[1] == "zoom" && (words.size() == 3U || words.size() == 4U)) {
    std::size_t index = 2U;
    if (words.size() == 4U) {
      const auto target = pane_selector(words[index]);
      if (!target.has_value()) {
        return invalid();
      }
      command.pane = *target;
      ++index;
    }
    if (words[index] != "--on" && words[index] != "--off") {
      return invalid();
    }
    command.kind = api::CommandKind::pane_zoom;
    command.enabled = words[index] == "--on";
    return action;
  }
  return words[1] == "split" || words[1] == "focus" || words[1] == "swap" || words[1] == "resize" ||
                 words[1] == "zoom" || words[1] == "kill"
             ? invalid()
             : std::expected<CommandLineAction, CommandLineError>{
                   std::unexpected(CommandLineError::unknown_command)};
}

// The caller bounds end by line size and completion storage is checked before access.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] auto simple_completion_words(const std::string_view line, const std::size_t end,
                                           std::array<std::string_view, 8>& storage) noexcept
    -> std::span<const std::string_view> {
  std::size_t count = 0;
  std::size_t index = 0;
  while (index < end) {
    while (index < end && (line[index] == ' ' || line[index] == '\t')) {
      ++index;
    }
    if (index == end) {
      break;
    }
    const auto begin = index;
    while (index < end && line[index] != ' ' && line[index] != '\t') {
      if (line[index] == '\\' || line[index] == '\'' || line[index] == '"' ||
          count == storage.size()) {
        return {};
      }
      ++index;
    }
    storage[count++] = line.substr(begin, index - begin);
  }
  return std::span(storage).first(count);
}

[[nodiscard]] constexpr auto option_value(const std::span<const std::string_view> words) noexcept
    -> bool {
  if (words.empty()) {
    return false;
  }
  const auto previous = words.back();
  return previous == "--title" || previous == "--cwd" || previous == "--focus" || previous == "--";
}

[[nodiscard]] constexpr auto
launch_completion_kind(const std::span<const std::string_view> words,
                       const CommandLineCompletionKind option_kind) noexcept
    -> CommandLineCompletionKind {
  if (words.back() == "--focus") {
    return CommandLineCompletionKind::focus_policy;
  }
  return option_value(words) ? CommandLineCompletionKind::none : option_kind;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto parse_command_line(const std::string_view line, const CommandLineContext context,
                                      const std::span<const extension::CommandDescriptor> commands)
    -> std::expected<CommandLineAction, CommandLineError> {
  if (line.empty() || line.size() > command_line_bytes_max) {
    return invalid();
  }
  auto tokenized = tokenize(line);
  if (!tokenized.valid || tokenized.words.empty()) {
    return invalid();
  }
  const auto words = std::span<const std::string>(tokenized.words);
  if (words[0] == "switch" || words[0] == "attach") {
    if (words.size() != 2U) {
      return invalid();
    }
    const auto target = session_selector(words[1]);
    if (!target.has_value()) {
      return invalid();
    }
    CommandLineAction action;
    action.kind = CommandLineActionKind::switch_session;
    action.switch_session = *target;
    return action;
  }
  if (words[0] == "paste-image") {
    if (words.size() != 1U) {
      return invalid();
    }
    CommandLineAction action;
    action.command.kind = api::CommandKind::pane_paste_image;
    action.command.session = current_session(context);
    action.command.pane = current_pane(context);
    return action;
  }
  if (words[0] == "reload") {
    if (words.size() != 1U) {
      return invalid();
    }
    CommandLineAction action;
    action.command.kind = api::CommandKind::config_reload;
    return action;
  }
  if (words[0] == "detach") {
    if (words.size() != 1U) {
      return invalid();
    }
    CommandLineAction action;
    action.kind = CommandLineActionKind::detach;
    return action;
  }
  if (words[0] == "session") {
    return parse_session_command(words, context);
  }
  if (words[0] == "tab") {
    return parse_tab_command(words, context);
  }
  if (words[0] == "pane") {
    return parse_pane_command(words, context);
  }
  if (std::ranges::any_of(commands,
                          [&](const auto& command) { return command.name == words[0]; })) {
    CommandLineAction action;
    action.kind = CommandLineActionKind::hosted;
    action.hosted_command = words[0];
    const auto arguments = words.subspan(1);
    action.arguments.assign(arguments.begin(), arguments.end());
    return action;
  }
  return std::unexpected(CommandLineError::unknown_command);
}

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
[[nodiscard]] auto command_line_completion_query(const std::string_view line,
                                                 const std::size_t cursor) noexcept
    -> CommandLineCompletionQuery {
  if (cursor > line.size()) {
    return {};
  }
  auto begin = cursor;
  while (begin > 0 && line[begin - 1U] != ' ' && line[begin - 1U] != '\t') {
    --begin;
  }
  auto end = cursor;
  while (end < line.size() && line[end] != ' ' && line[end] != '\t') {
    ++end;
  }
  const auto prefix = line.substr(begin, cursor - begin);
  if (prefix.find_first_of("\\\"'") != std::string_view::npos) {
    return {};
  }
  std::array<std::string_view, 8> storage{};
  const auto words = simple_completion_words(line, begin, storage);
  if (begin > 0 && words.empty() &&
      line.substr(0, begin).find_first_not_of(" \t") != std::string_view::npos) {
    return {};
  }
  CommandLineCompletionKind kind = CommandLineCompletionKind::none;
  if (words.empty()) {
    kind = CommandLineCompletionKind::root;
  } else if (words[0] == "switch" || words[0] == "attach") {
    kind =
        words.size() == 1U ? CommandLineCompletionKind::session : CommandLineCompletionKind::none;
  } else if (words[0] == "session") {
    if (words.size() == 1U) {
      kind = CommandLineCompletionKind::session_operation;
    } else if ((words[1] == "switch" || words[1] == "attach" || words[1] == "kill") &&
               words.size() == 2U) {
      kind = CommandLineCompletionKind::session;
    }
  } else if (words[0] == "tab") {
    if (words.size() == 1U) {
      kind = CommandLineCompletionKind::tab_operation;
    } else if (words[1] == "new") {
      kind = launch_completion_kind(words, CommandLineCompletionKind::tab_new_option);
    } else if ((words[1] == "select" || words[1] == "move" || words[1] == "kill") &&
               words.size() <= 3U) {
      kind = CommandLineCompletionKind::tab;
    }
  } else if (words[0] == "pane") {
    if (words.size() == 1U) {
      kind = CommandLineCompletionKind::pane_operation;
    } else if ((words[1] == "focus" || words[1] == "swap" || words[1] == "kill") &&
               words.size() <= 3U) {
      kind = CommandLineCompletionKind::pane;
    } else if (words[1] == "split") {
      kind = launch_completion_kind(words, CommandLineCompletionKind::pane_split_option);
    } else if (words[1] == "resize" && words.size() <= 3U) {
      kind = CommandLineCompletionKind::pane_resize_direction;
    } else if (words[1] == "zoom" && words.size() <= 3U) {
      kind = CommandLineCompletionKind::pane_zoom_option;
    }
  }
  return {.kind = kind, .replace_begin = begin, .replace_end = end, .prefix = prefix};
}

[[nodiscard]] auto command_line_static_completions(const CommandLineCompletionKind kind) noexcept
    -> std::span<const std::string_view> {
  switch (kind) {
  case CommandLineCompletionKind::root:
    return root_completions.view();
  case CommandLineCompletionKind::session_operation:
    return session_completions.view();
  case CommandLineCompletionKind::tab_operation:
    return tab_completions.view();
  case CommandLineCompletionKind::pane_operation:
    return pane_completions.view();
  case CommandLineCompletionKind::tab_new_option:
    return tab_new_completions;
  case CommandLineCompletionKind::pane_split_option:
    return pane_split_completions;
  case CommandLineCompletionKind::pane_resize_direction:
    return resize_completions;
  case CommandLineCompletionKind::pane_zoom_option:
    return zoom_completions;
  case CommandLineCompletionKind::focus_policy:
    return focus_completions;
  case CommandLineCompletionKind::none:
  case CommandLineCompletionKind::session:
  case CommandLineCompletionKind::tab:
  case CommandLineCompletionKind::pane:
    return {};
  }
  return {};
}

[[nodiscard]] auto complete_command_line(const std::string_view prefix,
                                         const std::span<const std::string_view> candidates)
    -> CommandLineCompletion {
  CommandLineCompletion completion;
  std::string_view first;
  std::size_t common_size = 0;
  for (const auto candidate : candidates) {
    if (!candidate.starts_with(prefix)) {
      continue;
    }
    if (completion.matches == 0) {
      first = candidate;
      common_size = candidate.size();
    } else {
      common_size = std::min(common_size, candidate.size());
      std::size_t index = 0;
      while (index < common_size && first[index] == candidate[index]) {
        ++index;
      }
      common_size = index;
    }
    ++completion.matches;
  }
  if (completion.matches == 1U) {
    completion.replacement = first;
    completion.append_space = true;
  } else if (completion.matches > 1U && common_size > prefix.size()) {
    completion.replacement = std::string(first.substr(0, common_size));
  }
  return completion;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)

} // namespace frame::core
