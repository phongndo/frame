#include "extension/external_command.hpp"

#include "api/json.hpp"
#include "frame/limits.hpp"

#include <array>
#include <cerrno>
#include <csignal> // IWYU pragma: keep — POSIX kill and SIGKILL.
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace frame::extension {
namespace {
void close_descriptor(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

// POSIX descriptor flag operations have a variadic system interface.
[[nodiscard]] auto close_on_exec(const int descriptor) noexcept -> bool {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
}
} // namespace

ExternalCommand::ExternalCommand(ExternalCommand&& other) noexcept
    : process_(std::exchange(other.process_, -1)),
      descriptor_(std::exchange(other.descriptor_, -1)), output_bytes_(other.output_bytes_),
      diagnostic_(std::move(other.diagnostic_)) {}

auto ExternalCommand::operator=(ExternalCommand&& other) noexcept -> ExternalCommand& {
  if (this != &other) {
    reset();
    process_ = std::exchange(other.process_, -1);
    descriptor_ = std::exchange(other.descriptor_, -1);
    output_bytes_ = other.output_bytes_;
    diagnostic_ = std::move(other.diagnostic_);
  }
  return *this;
}

ExternalCommand::~ExternalCommand() { reset(); }

void ExternalCommand::cancel() const noexcept {
  if (process_ > 0) {
    static_cast<void>(::kill(process_, SIGKILL));
  }
}

void ExternalCommand::reset() noexcept {
  close_descriptor(descriptor_);
  if (process_ > 0) {
    cancel();
    static_cast<void>(::waitpid(process_, nullptr, WNOHANG));
    process_ = -1;
  }
}

// Launch takes place in the single-threaded isolated host, never in the native reactor.
// POSIX open and descriptor flag operations use variadic system interfaces.
// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto ExternalCommand::start(const std::span<const std::string> arguments,
                            const std::string_view context) -> bool {
  if (process_ > 0 || arguments.empty() || arguments.front().empty() ||
      arguments.size() > limits::command_arguments_hard_max) {
    return false;
  }
  std::size_t bytes = 0;
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1U);
  for (const auto& argument : arguments) {
    if (argument.contains('\0') || argument.size() + 1U > limits::command_bytes_hard_max - bytes) {
      return false;
    }
    bytes += argument.size() + 1U;
    // execvp borrows but does not modify argument storage.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  const std::string context_text(context);
  int input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  std::array<int, 2> pipe{-1, -1};
  if (input < 0 || ::pipe(pipe.data()) != 0 || !close_on_exec(pipe.front()) ||
      !close_on_exec(pipe.back()) || ::fcntl(pipe.front(), F_SETFL, O_NONBLOCK) != 0) {
    close_descriptor(input);
    close_descriptor(pipe.front());
    close_descriptor(pipe.back());
    return false;
  }
  const auto child = ::fork();
  if (child == 0) {
    close_descriptor(pipe.front());
    if (::dup2(input, STDIN_FILENO) < 0 || ::dup2(pipe.back(), STDOUT_FILENO) < 0 ||
        ::dup2(pipe.back(), STDERR_FILENO) < 0 || ::fcntl(STDIN_FILENO, F_SETFD, 0) != 0 ||
        ::fcntl(STDOUT_FILENO, F_SETFD, 0) != 0 || ::fcntl(STDERR_FILENO, F_SETFD, 0) != 0 ||
        ::setenv("FRAME_COMMAND_CONTEXT", context_text.c_str(), 1) != 0) {
      ::_exit(127);
    }
    if (input > STDERR_FILENO) {
      close_descriptor(input);
    }
    if (pipe.back() > STDERR_FILENO) {
      close_descriptor(pipe.back());
    }
    // Stay in the host process group so its native watchdog also revokes descendants.
    ::execvp(argv.front(), argv.data());
    ::_exit(127);
  }
  close_descriptor(input);
  close_descriptor(pipe.back());
  if (child < 0) {
    close_descriptor(pipe.front());
    return false;
  }
  process_ = static_cast<int>(child);
  descriptor_ = pipe.front();
  output_bytes_ = 0;
  diagnostic_.clear();
  return true;
}

// NOLINTEND(cppcoreguidelines-pro-type-vararg)

void ExternalCommand::read_output() {
  std::array<char, 4096> bytes{};
  const auto received = ::read(descriptor_, bytes.data(), bytes.size());
  if (received <= 0) {
    if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      close_descriptor(descriptor_);
    }
    return;
  }
  const auto count = static_cast<std::size_t>(received);
  output_bytes_ += count;
  if (output_bytes_ > bytes.size()) {
    diagnostic_ = "external command output limit exceeded";
    cancel();
    close_descriptor(descriptor_);
    return;
  }
  for (const char character : std::span(bytes).first(count)) {
    if (diagnostic_.size() == 180U) {
      break;
    }
    diagnostic_ += character >= 32 && character < 127 ? character : '?';
  }
}

auto ExternalCommand::service(const short events) -> std::optional<std::string> {
  if (process_ <= 0) {
    return std::nullopt;
  }
  if ((events & (POLLIN | POLLHUP)) != 0 && descriptor_ >= 0) {
    read_output();
  }
  if ((events & (POLLERR | POLLNVAL)) != 0) {
    cancel();
    close_descriptor(descriptor_);
  }
  if (descriptor_ >= 0) {
    return std::nullopt;
  }
  int status = 0;
  const auto waited = ::waitpid(process_, &status, WNOHANG);
  if (waited == 0 || (waited < 0 && errno == EINTR)) {
    return std::nullopt;
  }
  process_ = -1;
  return completion(status, waited > 0);
}

// Darwin's wait-status macros require a non-const status to avoid casting away qualifiers.
auto ExternalCommand::completion(int status, const bool observed) -> std::string {
  if (observed && WIFEXITED(status) && WEXITSTATUS(status) == 0 && output_bytes_ <= 4096U) {
    return R"({"ok":true})";
  }
  if (diagnostic_.empty()) {
    diagnostic_ = observed && WIFEXITED(status)
                      ? "external command exited with status " + std::to_string(WEXITSTATUS(status))
                      : "external command terminated";
  }
  std::string result = R"({"ok":false,"error":)";
  if (!api::append_json_string(result, diagnostic_)) {
    return R"({"ok":false,"error":"external command failed"})";
  }
  return result + '}';
}

} // namespace frame::extension
