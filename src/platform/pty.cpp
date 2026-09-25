#include "platform/pty.hpp"

#include "lemma/limits.hpp"
#include "lemma/terminal_identity.hpp"
#include "lemma/version.hpp"
#include "platform/io.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <pwd.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#include <libproc.h>
#include <util.h>
#elifdef __linux__
#include <charconv>
#include <system_error>

#include <fcntl.h>
#include <pty.h>
#else
#error "lemma requires forkpty"
#endif

namespace lemma::platform {
namespace {

[[nodiscard]] auto copy_process_name(const std::span<const char> source,
                                     const std::span<char> output) noexcept -> std::size_t {
  const auto terminator = std::ranges::find_if(source, [](const char character) {
    return character == '\0' || character == '\n' || character == '\r';
  });
  const auto available = static_cast<std::size_t>(std::distance(source.begin(), terminator));
  const auto size = std::min(available, output.size());
  std::ranges::copy(source.first(size), output.begin());
  return size;
}

[[nodiscard]] auto install_environment(const std::span<char> environment,
                                       const EnvironmentMode mode) noexcept -> bool {
  if (mode == EnvironmentMode::inherit) {
    return environment.empty();
  }
#ifdef __APPLE__
  // Darwin setenv requires a writable allocated vector after replacing the inherited environment.
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
  auto** const empty_environment = static_cast<char**>(std::calloc(1, sizeof(char*)));
  if (empty_environment == nullptr) {
    return false;
  }
  *_NSGetEnviron() = empty_environment;
#elifdef __linux__
  if (::clearenv() != 0) {
    return false;
  }
#endif
  std::size_t offset = 0;
  while (offset < environment.size()) {
    auto entry = environment.subspan(offset);
    const auto terminator = std::ranges::find(entry, '\0');
    if (terminator == entry.end()) {
      return false;
    }
    const auto entry_size = static_cast<std::size_t>(std::distance(entry.begin(), terminator));
    auto value = entry.first(entry_size);
    const auto separator = std::ranges::find(value, '=');
    if (separator == value.begin() || separator == value.end()) {
      return false;
    }
    *separator = '\0';
    if (::setenv(value.data(), std::to_address(separator + 1), 1) != 0) {
      return false;
    }
    offset += entry_size + 1U;
  }
  return true;
}

[[nodiscard]] auto process_environment() noexcept -> char** {
#ifdef __APPLE__
  return *_NSGetEnviron();
#elifdef __linux__
  return ::environ;
#endif
}

[[nodiscard]] auto install_overlay(const std::span<const EnvironmentVariable> overlay) noexcept
    -> bool {
  for (const auto& variable : overlay) {
    if (variable.name.empty() || variable.name.contains('=') || variable.name.contains('\0') ||
        variable.value.contains('\0')) {
      return false;
    }
    std::array<char, 64> name{};
    std::array<char, 128> value{};
    if (variable.name.size() >= name.size() || variable.value.size() >= value.size()) {
      return false;
    }
    std::ranges::copy(variable.name, name.begin());
    std::ranges::copy(variable.value, value.begin());
    if (::setenv(name.data(), value.data(), 1) != 0) {
      return false;
    }
  }
  return true;
}

} // namespace

auto account_home_directory(const std::span<char> output) noexcept -> std::size_t {
  std::array<char, std::size_t{16} * 1'024U> account_buffer{};
  struct passwd account{};
  struct passwd* result = nullptr;
  if (::getpwuid_r(::getuid(), &account, account_buffer.data(), account_buffer.size(), &result) !=
          0 ||
      result == nullptr || account.pw_dir == nullptr) {
    return 0;
  }
  const std::string_view home(account.pw_dir);
  if (home.empty() || home.front() != '/' || home.size() >= output.size() || home.contains('\0')) {
    return 0;
  }
  std::ranges::copy(home, output.begin());
  return home.size();
}

auto capture_process_environment(const std::span<std::byte> output) noexcept
    -> std::optional<std::size_t> {
  std::size_t size = 0;
  std::size_t entries = 0;
  // POSIX exposes the process environment as a null-terminated pointer vector.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (char** entry = process_environment(); entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view value(*entry);
    if (value.size() + 1U > output.size() - size || entries == limits::environment_entries_max) {
      return std::nullopt;
    }
    std::ranges::copy(std::as_bytes(std::span(value.data(), value.size())),
                      output.subspan(size).begin());
    size += value.size();
    output.subspan(size, 1).front() = std::byte{0};
    ++size;
    ++entries;
  }
  return size;
}

// Validation and child replacement outcomes are intentionally explicit at the platform boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto spawn_process(int& pty_descriptor, const std::string_view working_directory,
                                 const std::span<const std::byte> environment,
                                 const EnvironmentMode environment_mode,
                                 const std::span<const std::byte> launch_command,
                                 const std::span<const EnvironmentVariable> overlay,
                                 const std::string_view fallback_working_directory) noexcept
    -> pid_t {
  using Directory = std::array<char, limits::working_directory_bytes_max + 1U>;
  const auto valid_directory = [](const std::string_view path) {
    return path.size() < Directory{}.size() && !path.contains('\0') &&
           (path.empty() || path.front() == '/');
  };
  Directory directory{};
  Directory fallback{};
  if (!valid_directory(working_directory) || !valid_directory(fallback_working_directory)) {
    return -1;
  }
  std::ranges::copy(working_directory, directory.begin());
  std::ranges::copy(fallback_working_directory, fallback.begin());
  std::array<char, limits::environment_bytes_max> environment_copy{};
  if (environment.size() > environment_copy.size()) {
    return -1;
  }
  std::ranges::copy(environment, std::as_writable_bytes(std::span(environment_copy)).begin());

  std::array<char, limits::command_bytes_hard_max> command_copy{};
  std::array<char*, limits::command_arguments_hard_max + 1U> command_arguments{};
  std::size_t command_argument_count = 0;
  if (!launch_command.empty()) {
    if (launch_command.size() > command_copy.size()) {
      return -1;
    }
    std::ranges::copy(launch_command, std::as_writable_bytes(std::span(command_copy)).begin());
    std::size_t offset = 0;
    while (offset < launch_command.size()) {
      if (command_argument_count == limits::command_arguments_hard_max) {
        return -1;
      }
      const auto remaining =
          std::span(command_copy).subspan(offset, launch_command.size() - offset);
      const auto terminator = std::ranges::find(remaining, '\0');
      if (terminator == remaining.end()) {
        return -1;
      }
      if (command_argument_count == 0 && terminator == remaining.begin()) {
        return -1;
      }
      std::span(command_arguments).subspan(command_argument_count, 1).front() = remaining.data();
      ++command_argument_count;
      offset += static_cast<std::size_t>(std::distance(remaining.begin(), terminator)) + 1U;
    }
  }

  std::array<char, 4096> terminfo{};
  if (terminfo_directory(terminfo) == 0) {
    return -1;
  }
  winsize initial_size{.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
  const auto child = ::forkpty(&pty_descriptor, nullptr, nullptr, &initial_size);
  if (child != 0) {
    return child;
  }

  // The daemon ignores these signals for its own I/O and child-reaping behavior. Ignored
  // dispositions survive exec, so restore normal process semantics before launching the child.
  if (::signal(SIGCHLD, SIG_DFL) == SIG_ERR || ::signal(SIGPIPE, SIG_DFL) == SIG_ERR) {
    ::_exit(127);
  }

  // A directory can become unenterable between validation and exec; an inherited directory then
  // yields to its fallback rather than failing the Pane.
  const char* entered = nullptr;
  if (!working_directory.empty()) {
    if (::chdir(directory.data()) == 0) {
      entered = directory.data();
    } else if (!fallback_working_directory.empty() && ::chdir(fallback.data()) == 0) {
      entered = fallback.data();
    } else {
      ::_exit(127);
    }
  }
  if (!install_environment(std::span(environment_copy).first(environment.size()),
                           environment_mode) ||
      (entered != nullptr && ::setenv("PWD", entered, 1) != 0) || !install_overlay(overlay) ||
      // terminal_name is backed by a null-terminated string literal.
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      ::setenv("TERM", terminal_name.data(), 1) != 0 ||
      ::setenv("TERMINFO", terminfo.data(), 1) != 0 || ::setenv("COLORTERM", "truecolor", 1) != 0 ||
      ::setenv("TERM_PROGRAM", "lemma", 1) != 0 ||
      // version is backed by a null-terminated string literal.
      // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
      ::setenv("TERM_PROGRAM_VERSION", lemma::version.data(), 1) != 0) {
    ::_exit(127);
  }

  if (command_argument_count > 0) {
    ::execvp(command_arguments.front(), command_arguments.data());
    ::_exit(127);
  }

  std::array fallback_shell{'/', 'b', 'i', 'n', '/', 's', 'h', '\0'};
  std::array<char, std::size_t{16} * 1'024U> account_buffer{};
  struct passwd account{};
  struct passwd* account_result = nullptr;
  char* shell = fallback_shell.data();
  if (::getpwuid_r(::getuid(), &account, account_buffer.data(), account_buffer.size(),
                   &account_result) == 0 &&
      account_result != nullptr && account.pw_shell != nullptr) {
    const std::string_view configured_shell(account.pw_shell);
    if (!configured_shell.empty() && configured_shell.front() == '/' &&
        ::access(account.pw_shell, X_OK) == 0) {
      shell = account.pw_shell;
    }
  }
  std::array login_argument{'-', 'l', '\0'};
  const std::array arguments{shell, login_argument.data(), static_cast<char*>(nullptr)};
  ::execv(shell, arguments.data());
  ::_exit(127);
}

[[nodiscard]] auto foreground_process_name(const int pty_descriptor,
                                           const std::span<char> output) noexcept -> std::size_t {
  if (output.empty()) {
    return 0;
  }
  const auto foreground_group = ::tcgetpgrp(pty_descriptor);
  if (foreground_group <= 0) {
    return 0;
  }

#ifdef __APPLE__
  proc_bsdshortinfo information{};
  const auto bytes = ::proc_pidinfo(foreground_group, PROC_PIDT_SHORTBSDINFO, 0, &information,
                                    static_cast<int>(sizeof(information)));
  if (bytes < 0 || static_cast<std::size_t>(bytes) != sizeof(information)) {
    return 0;
  }
  return copy_process_name(information.pbsi_comm, output);
#elifdef __linux__
  std::array<char, 64> path{};
  constexpr std::string_view prefix = "/proc/";
  constexpr std::string_view suffix = "/comm";
  std::ranges::copy(prefix, path.begin());
  auto remaining = std::span(path).subspan(prefix.size());
  const auto encoded = std::to_chars(remaining.data(), path.end(), foreground_group);
  const auto digits = static_cast<std::size_t>(std::distance(remaining.data(), encoded.ptr));
  if (encoded.ec != std::errc{} || remaining.size() - digits <= suffix.size()) {
    return 0;
  }
  std::ranges::copy(suffix, remaining.subspan(digits).begin());
  // open is variadic only to accept a mode when creation flags require one.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const auto descriptor = ::open(path.data(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return 0;
  }
  std::array<char, 64> name{};
  const auto bytes = ::read(descriptor, name.data(), name.size());
  static_cast<void>(::close(descriptor));
  return bytes > 0
             ? copy_process_name(std::span(name).first(static_cast<std::size_t>(bytes)), output)
             : 0;
#endif
}

[[nodiscard]] constexpr auto window_pixels(const std::uint32_t cell_px,
                                           const std::uint16_t cells) noexcept -> unsigned short {
  if (cell_px == 0 || cells == 0) {
    return 0;
  }
  constexpr auto pixel_max = std::numeric_limits<unsigned short>::max();
  if (cell_px > static_cast<std::uint32_t>(pixel_max) / cells) {
    return pixel_max;
  }
  return static_cast<unsigned short>(cell_px * cells);
}

[[nodiscard]] auto resize_pty(const int pty_descriptor, const std::uint16_t columns,
                              const std::uint16_t rows, const std::uint32_t cell_width_px,
                              const std::uint32_t cell_height_px) noexcept -> bool {
  winsize native_size{
      .ws_row = rows,
      .ws_col = columns,
      .ws_xpixel = window_pixels(cell_width_px, columns),
      .ws_ypixel = window_pixels(cell_height_px, rows),
  };
  // ioctl is variadic because its third argument depends on the request.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return ::ioctl(pty_descriptor, TIOCSWINSZ, &native_size) == 0;
}

} // namespace lemma::platform
