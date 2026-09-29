#include "core/command_history.hpp"

#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace frame::core {
namespace {

constexpr std::string_view history_header = "frame-command-history-v1\n";
constexpr std::size_t history_file_bytes_max =
    history_header.size() +
    ((limits::command_line_bytes_max + 1U) * limits::command_line_history_max);
constexpr std::size_t history_path_bytes_max = 4'096;

class Descriptor final {
public:
  explicit Descriptor(const int value) noexcept : value_(value) {}
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  Descriptor(Descriptor&&) = delete;
  auto operator=(Descriptor&&) -> Descriptor& = delete;
  ~Descriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }

  [[nodiscard]] auto get() const noexcept -> int { return value_; }

private:
  int value_{-1};
};

[[nodiscard]] constexpr auto command_valid(const std::string_view command) noexcept -> bool {
  return !command.empty() && command.size() <= limits::command_line_bytes_max &&
         std::ranges::all_of(command, [](const char character) {
           const auto byte = static_cast<unsigned char>(character);
           return byte >= 0x20U && byte <= 0x7eU;
         });
}

[[nodiscard]] auto path_string(const std::string_view path) noexcept -> std::string {
  if (path.empty() || path.size() > history_path_bytes_max || path.contains('\0')) {
    return {};
  }
  try {
    return std::string(path);
  } catch (...) {
    return {};
  }
}

[[nodiscard]] auto write_bytes(const int descriptor, const std::string_view bytes) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto remaining = std::span(bytes).subspan(offset);
    const auto written = ::write(descriptor, remaining.data(), remaining.size());
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]] auto read_history_file(const int descriptor, const std::span<char> storage) noexcept
    -> std::optional<std::size_t> {
  std::size_t size = 0;
  while (size < storage.size()) {
    const auto remaining = storage.subspan(size);
    const auto read = ::read(descriptor, remaining.data(), remaining.size());
    if (read > 0) {
      size += static_cast<std::size_t>(read);
      continue;
    }
    if (read < 0 && errno == EINTR) {
      continue;
    }
    return read == 0 ? std::optional{size} : std::nullopt;
  }
  return size;
}

// File limits and indexes are validated before bounded storage access.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] auto parse_history_file(const std::string_view contents) noexcept
    -> std::optional<CommandLineHistory> {
  if (!contents.starts_with(history_header)) {
    return std::nullopt;
  }
  CommandLineHistory history;
  auto remaining = contents.substr(history_header.size());
  std::size_t entries = 0;
  while (!remaining.empty()) {
    if (entries == limits::command_line_history_max) {
      return std::nullopt;
    }
    const auto newline = remaining.find('\n');
    if (newline == std::string_view::npos) {
      return std::nullopt;
    }
    const auto command = remaining.substr(0, newline);
    if (!command_valid(command)) {
      return std::nullopt;
    }
    remember_command_line(history, command);
    ++entries;
    remaining.remove_prefix(newline + 1U);
  }
  return history;
}

} // namespace

void remember_command_line(CommandLineHistory& history, const std::string_view command) noexcept {
  if (!command_valid(command) || (history.size > 0 && history.entries.front().view() == command)) {
    return;
  }
  const auto retained = std::min<std::size_t>(history.size, history.entries.size() - 1U);
  auto entries = std::span(history.entries).first(retained + 1U);
  std::ranges::copy_backward(entries.first(retained), entries.end());
  auto& newest = history.entries.front();
  newest = {};
  std::ranges::copy(command, newest.text.begin());
  newest.size = static_cast<std::uint16_t>(command.size());
  history.size = static_cast<std::uint8_t>(
      std::min<std::size_t>(history.entries.size(), static_cast<std::size_t>(history.size) + 1U));
}

[[nodiscard]] auto load_command_line_history(const std::string_view path) noexcept
    -> CommandLineHistoryLoadResult {
  const auto owned_path = path_string(path);
  if (owned_path.empty()) {
    return {};
  }
  // open is variadic even though an existing file needs no mode argument.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const Descriptor descriptor(::open(owned_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
  if (descriptor.get() < 0) {
    return {.history = {}, .replace_on_shutdown = errno == ENOENT};
  }
  struct stat metadata{};
  if (::fstat(descriptor.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 || std::cmp_greater(metadata.st_size, history_file_bytes_max)) {
    return {};
  }
  std::array<char, history_file_bytes_max + 1U> storage{};
  const auto size = read_history_file(descriptor.get(), storage);
  if (!size.has_value() || *size > history_file_bytes_max) {
    return {};
  }
  const auto history = parse_history_file(std::string_view(storage.data(), *size));
  return history.has_value()
             ? CommandLineHistoryLoadResult{.history = *history, .replace_on_shutdown = true}
             : CommandLineHistoryLoadResult{};
}

[[nodiscard]] auto save_command_line_history(const std::string_view path,
                                             const CommandLineHistory& history) noexcept -> bool {
  const auto owned_path = path_string(path);
  if (owned_path.empty()) {
    return path.empty();
  }
  std::string temporary;
  try {
    temporary = owned_path + ".tmp.XXXXXX";
  } catch (...) {
    return false;
  }
  const int raw_descriptor = ::mkstemp(temporary.data());
  if (raw_descriptor < 0) {
    return false;
  }
  const Descriptor descriptor(raw_descriptor);
  // fcntl is variadic because the final argument depends on the selected operation.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  static_cast<void>(::fcntl(descriptor.get(), F_SETFD, FD_CLOEXEC));
  bool written = ::fchmod(descriptor.get(), S_IRUSR | S_IWUSR) == 0 &&
                 write_bytes(descriptor.get(), history_header);
  for (std::size_t index = history.size; written && index > 0; --index) {
    const auto command = std::span(history.entries).subspan(index - 1U, 1U).front().view();
    written = command_valid(command) && write_bytes(descriptor.get(), command) &&
              write_bytes(descriptor.get(), "\n");
  }
  written = written && ::fsync(descriptor.get()) == 0;
  if (written) {
    written = ::rename(temporary.c_str(), owned_path.c_str()) == 0;
  }
  if (!written) {
    static_cast<void>(::unlink(temporary.c_str()));
  }
  return written;
}

} // namespace frame::core
