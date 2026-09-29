#ifndef FRAME_CORE_COMMAND_HISTORY_HPP
#define FRAME_CORE_COMMAND_HISTORY_HPP

#include "frame/limits.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace frame::core {

struct CommandLineHistoryEntry final {
  std::array<char, limits::command_line_bytes_max> text{};
  std::uint16_t size{0};

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {text.data(), size}; }
};

struct CommandLineHistory final {
  // Newest-first storage makes bounded Up/Down navigation direct. Submission is cold and may shift
  // fixed entries; input and rendering never allocate history storage.
  std::array<CommandLineHistoryEntry, limits::command_line_history_max> entries{};
  std::uint8_t size{0};
};

struct CommandLineHistoryLoadResult final {
  CommandLineHistory history;
  // Missing files may be created and successfully parsed files may be replaced. Other failures
  // leave the existing path untouched on shutdown.
  bool replace_on_shutdown{false};
};

// Adds one nonempty command unless it duplicates the newest entry.
void remember_command_line(CommandLineHistory& history, std::string_view command) noexcept;

// Persistence is deliberately best effort. Missing, malformed, oversized, and inaccessible files
// produce an empty history. Only missing or successfully parsed paths are safe to replace; saving
// uses a same-directory temporary file and atomic rename.
[[nodiscard]] auto load_command_line_history(std::string_view path) noexcept
    -> CommandLineHistoryLoadResult;
[[nodiscard]] auto save_command_line_history(std::string_view path,
                                             const CommandLineHistory& history) noexcept -> bool;

} // namespace frame::core

#endif // FRAME_CORE_COMMAND_HISTORY_HPP
