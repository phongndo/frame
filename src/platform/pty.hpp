#ifndef LEMMA_PLATFORM_PTY_HPP
#define LEMMA_PLATFORM_PTY_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <sys/types.h>

namespace lemma::platform {

enum class EnvironmentMode : std::uint8_t {
  inherit,
  replace,
};

struct EnvironmentVariable final {
  std::string_view name;
  std::string_view value;
};

// Captures deterministic process launch defaults into caller-owned bounded storage.
[[nodiscard]] auto account_home_directory(std::span<char> output) noexcept -> std::size_t;
[[nodiscard]] auto capture_process_environment(std::span<std::byte> output) noexcept
    -> std::optional<std::size_t>;

// Spawns an exact NUL-separated argv, or the account's login shell when launch_command is empty,
// with a new controlling PTY. The parent receives the child PID and master descriptor; the child
// replaces itself or exits with status 127. Replacement clears the inherited environment even when
// the supplied snapshot is empty. When the child cannot enter working_directory, it enters a
// nonempty fallback_working_directory instead of exiting.
[[nodiscard]] auto spawn_process(int& pty_descriptor, std::string_view working_directory = {},
                                 std::span<const std::byte> environment = {},
                                 EnvironmentMode environment_mode = EnvironmentMode::inherit,
                                 std::span<const std::byte> launch_command = {},
                                 std::span<const EnvironmentVariable> overlay = {},
                                 std::string_view fallback_working_directory = {}) noexcept
    -> pid_t;

[[nodiscard]] auto resize_pty(int pty_descriptor, std::uint16_t columns, std::uint16_t rows,
                              std::uint32_t cell_width_px, std::uint32_t cell_height_px) noexcept
    -> bool;

// Returns the child output bytes the master can read without blocking, or zero when unknown.
[[nodiscard]] auto pty_readable_bytes(int pty_descriptor) noexcept -> std::size_t;

// Returns the bounded process name for the PTY's foreground process group, or zero when the
// foreground process cannot be inspected.
[[nodiscard]] auto foreground_process_name(int pty_descriptor, std::span<char> output) noexcept
    -> std::size_t;

} // namespace lemma::platform

#endif // LEMMA_PLATFORM_PTY_HPP
