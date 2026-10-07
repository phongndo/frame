#include "platform/io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <cstdint>
#include <mach-o/dyld.h>
#include <sys/syslimits.h>
#endif

namespace frame::platform {

[[nodiscard]] auto executable_path(const std::span<char> output) noexcept -> std::size_t {
  if (output.empty()) {
    return 0;
  }
#ifdef __APPLE__
  auto size = static_cast<std::uint32_t>(output.size());
  if (_NSGetExecutablePath(output.data(), &size) != 0) {
    return 0;
  }
  // dyld can return the command symlink. Helpers and terminfo belong beside the
  // actual executable, including when only `frame` is linked onto the user's PATH.
  std::array<char, PATH_MAX> resolved{};
  if (::realpath(output.data(), resolved.data()) == nullptr) {
    return 0;
  }
  const auto length = std::char_traits<char>::length(resolved.data());
  if (length >= output.size()) {
    return 0;
  }
  std::copy_n(resolved.begin(), length + 1U, output.begin());
  return length;
#else
  const auto size = ::readlink("/proc/self/exe", output.data(), output.size() - 1U);
  if (size <= 0 || static_cast<std::size_t>(size) >= output.size() - 1U) {
    return 0;
  }
  output.subspan(static_cast<std::size_t>(size), 1).front() = '\0';
  return static_cast<std::size_t>(size);
#endif
}

[[nodiscard]] auto terminfo_directory(const std::span<char> output) noexcept -> std::size_t {
  std::array<char, 4096> executable{};
  const auto size = executable_path(executable);
  if (size == 0) {
    return 0;
  }
  const std::string_view path(executable.data(), size);
  const auto separator = path.find_last_of('/');
  if (separator == std::string_view::npos) {
    return 0;
  }
  try {
    // Build trees and relocatable installations both carry their compiled entry. Do not advertise
    // TERM=frame when a bare copied executable cannot make its terminfo available to children.
    for (const auto* const suffix : {"/terminfo", "/../share/terminfo"}) {
      const auto directory = std::string(path.substr(0, separator)) + suffix;
      if (directory.size() < output.size() &&
          (::access((directory + "/f/frame").c_str(), R_OK) == 0 ||
           ::access((directory + "/66/frame").c_str(), R_OK) == 0)) {
        std::ranges::copy(directory, output.begin());
        output.subspan(directory.size(), 1).front() = '\0';
        return directory.size();
      }
    }
  } catch (...) {
    return 0; // Resource lookup failure is returned to process creation, never hidden.
  }
  return 0;
}

[[nodiscard]] auto write_all(const int descriptor, const std::span<const std::byte> bytes) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto result = ::write(descriptor, bytes.subspan(offset).data(), bytes.size() - offset);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto send_all(const int socket, const std::span<const std::byte> bytes) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto result =
        ::send(socket, bytes.subspan(offset).data(), bytes.size() - offset, MSG_NOSIGNAL);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto read_exact(const int socket, const std::span<std::byte> output) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < output.size()) {
    const auto result = ::recv(socket, output.subspan(offset).data(), output.size() - offset, 0);
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto write_text(const int descriptor, const std::string_view text) noexcept -> bool {
  return write_all(descriptor, std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto send_text(const int socket, const std::string_view text) noexcept -> bool {
  return send_all(socket, std::as_bytes(std::span(text.data(), text.size())));
}

void close_descriptor(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

[[nodiscard]] auto set_nonblocking(const int descriptor) noexcept -> bool {
  // fcntl is variadic because its third argument depends on the command.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const auto flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace frame::platform
