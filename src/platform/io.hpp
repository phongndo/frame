#ifndef FRAME_PLATFORM_IO_HPP
#define FRAME_PLATFORM_IO_HPP

#include <cstddef>
#include <span>
#include <string_view>

namespace frame::platform {

// Blocking completion helpers. Use only at setup/control boundaries or with descriptors whose
// readiness has already been established. Hot-path output uses explicit partial-write state.
[[nodiscard]] auto write_all(int descriptor, std::span<const std::byte> bytes) noexcept -> bool;
[[nodiscard]] auto send_all(int socket, std::span<const std::byte> bytes) noexcept -> bool;
[[nodiscard]] auto read_exact(int socket, std::span<std::byte> output) noexcept -> bool;
[[nodiscard]] auto write_text(int descriptor, std::string_view text) noexcept -> bool;
[[nodiscard]] auto send_text(int socket, std::string_view text) noexcept -> bool;

// Resolve resources relative to this executable, not cwd or PATH. Empty means unavailable.
[[nodiscard]] auto executable_path(std::span<char> output) noexcept -> std::size_t;
[[nodiscard]] auto terminfo_directory(std::span<char> output) noexcept -> std::size_t;

void close_descriptor(int& descriptor) noexcept;
[[nodiscard]] auto set_nonblocking(int descriptor) noexcept -> bool;

} // namespace frame::platform

#endif // FRAME_PLATFORM_IO_HPP
