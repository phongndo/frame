#include "daemon/server.hpp"

#include "api/command.hpp"
#include "api/json.hpp"
#include "api/proc.hpp"
#include "config/config.hpp"
#include "core/engine.hpp"
#include "extension/lua_host.hpp"
#include "extension/services.hpp"
#include "lemma/id.hpp"
#include "platform/io.hpp"
#include "platform/pty.hpp"
#include "protocol/attachment.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lemma::daemon {
namespace {

constexpr auto response_ready = protocol::wire_byte(protocol::ControlResponse::ready);
constexpr auto response_no_effect = protocol::wire_byte(protocol::ControlResponse::no_effect);
constexpr auto response_unavailable = protocol::wire_byte(protocol::ControlResponse::unavailable);
constexpr auto response_capacity = protocol::wire_byte(protocol::ControlResponse::capacity);
constexpr auto response_conflict = protocol::wire_byte(protocol::ControlResponse::conflict);
constexpr auto response_missing = protocol::wire_byte(protocol::ControlResponse::missing);
using platform::close_descriptor;
using platform::read_exact;
using platform::send_all;
using platform::set_nonblocking;
using platform::write_all;
using platform::write_text;

volatile sig_atomic_t child_exit_pending = 0;
volatile sig_atomic_t child_exit_wakeup_descriptor = -1;

[[nodiscard]] auto set_close_on_exec(const int descriptor) noexcept -> bool {
  // fcntl is variadic because its final argument depends on the command.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const auto flags = ::fcntl(descriptor, F_GETFD, 0);
  if (flags < 0) {
    return false;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  return ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

class ConfigurationReloader final : public core::ConfigurationReloader {
public:
  explicit ConfigurationReloader(extension::ConfigurationLoad& active) noexcept
      : active_(&active) {}

  auto start() noexcept -> bool override {
    reap();
    if (loader_.has_value() || retired_.active()) {
      return false;
    }
    loader_.emplace();
    if (!loader_->start()) {
      loader_.reset();
      return false;
    }
    return true;
  }
  [[nodiscard]] auto descriptor() const noexcept -> int override {
    return loader_.has_value() ? loader_->descriptor() : -1;
  }
  [[nodiscard]] auto deadline() const noexcept -> core::ReactorClock::time_point override {
    return loader_.has_value() ? loader_->deadline() : core::ReactorClock::time_point::max();
  }
  auto advance(const core::ReactorClock::time_point now) noexcept
      -> std::optional<core::ConfigurationCandidate> override {
    if (!loader_.has_value() || !loader_->advance(now)) {
      return std::nullopt;
    }
    const auto& candidate = loader_->result();
    if (candidate.status == extension::ConfigurationStatus::invalid ||
        candidate.generation == nullptr || active_->generation == nullptr) {
      return core::ConfigurationCandidate{.commands = {}, .diagnostic = candidate.diagnostic};
    }
    const auto& next = *candidate.generation;
    const auto& old = *active_->generation;
    if (next.history_file() != old.history_file() || next.status_line() != old.status_line() ||
        !std::ranges::equal(next.extensions(), old.extensions(), [](const auto& a, const auto& b) {
          return a.name == b.name && a.argv == b.argv;
        })) {
      return core::ConfigurationCandidate{
          .commands = {},
          .error = core::ConfigurationReloadError::restart_required,
          .diagnostic =
              "Changes to history.file, ui.status_line, or managed extensions require a restart"};
    }
    return core::ConfigurationCandidate{
        .generation = candidate.generation.get(),
        .commands = candidate.commands,
        .descriptor = candidate.host.descriptor(),
        .stop =
            [](void* context) noexcept {
              static_cast<extension::HostProcess*>(context)->terminate();
            },
        .context = &active_->host,
        .diagnostic = {}};
  }
  void commit() noexcept override {
    if (loader_.has_value()) {
      retired_ = std::move(active_->host);
      retired_.terminate();
      *active_ = loader_->take();
      loader_.reset();
    }
  }
  void discard() noexcept override {
    if (loader_.has_value()) {
      retired_ = std::move(loader_->host());
      retired_.terminate();
      loader_.reset();
    }
  }
  void reap() noexcept {
    retired_.reap_exited();
    if (loader_.has_value()) {
      loader_->host().reap_exited();
    }
  }

private:
  extension::ConfigurationLoad* active_;
  extension::HostProcess retired_;
  std::optional<extension::ConfigurationLoader> loader_;
};

class ChildExitReaper final {
public:
  explicit ChildExitReaper(extension::HostProcess* const host) noexcept : host_(host) {
    std::array<int, 2> created{};
    if (::pipe(created.data()) != 0) {
      return;
    }
    read_descriptor_ = created.front();
    write_descriptor_ = created.back();
    if (!set_nonblocking(read_descriptor_) || !set_nonblocking(write_descriptor_) ||
        !set_close_on_exec(read_descriptor_) || !set_close_on_exec(write_descriptor_)) {
      close_descriptor(read_descriptor_);
      close_descriptor(write_descriptor_);
    }
  }

  ChildExitReaper(const ChildExitReaper&) = delete;
  auto operator=(const ChildExitReaper&) -> ChildExitReaper& = delete;
  ChildExitReaper(ChildExitReaper&&) = delete;
  auto operator=(ChildExitReaper&&) -> ChildExitReaper& = delete;

  ~ChildExitReaper() {
    close_descriptor(read_descriptor_);
    close_descriptor(write_descriptor_);
  }

  [[nodiscard]] auto valid() const noexcept -> bool {
    return read_descriptor_ >= 0 && write_descriptor_ >= 0;
  }
  [[nodiscard]] auto read_descriptor() const noexcept -> int { return read_descriptor_; }
  [[nodiscard]] auto write_descriptor() const noexcept -> int { return write_descriptor_; }

  void services(extension::Services& services) noexcept { services_ = &services; }
  void reloader(ConfigurationReloader& reloader) noexcept { reloader_ = &reloader; }

  // Preserve every helper's process-group identity before the general PTY-child reap.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto reap_process(int& status) const noexcept -> pid_t {
    if (reloader_ == nullptr && services_ == nullptr && (host_ == nullptr || !host_->active())) {
      return ::waitpid(-1, &status, WNOHANG);
    }
    // Select without consuming an exit. A generic waitpid(-1) after checking a live host would
    // race its exit and could release its process-group identity before revocation.
    while (true) {
      siginfo_t information{};
      if (::waitid(P_ALL, 0, &information, WEXITED | WNOHANG | WNOWAIT) != 0) {
        return -1;
      }
      if (information.si_pid == 0) {
        return 0;
      }
      if (host_ != nullptr) {
        host_->reap_exited();
      }
      if (reloader_ != nullptr) {
        reloader_->reap();
      }
      if (services_ != nullptr) {
        services_->reap_exited();
      }
      const auto process = ::waitpid(information.si_pid, &status, WNOHANG);
      if (process < 0 && errno == ECHILD) {
        // The selected child was the host, consumed by its owner after revoking the group.
        continue;
      }
      return process;
    }
  }

  void drain_wakeup() const noexcept {
    std::array<std::byte, 64> bytes{};
    while (true) {
      const auto count = ::read(read_descriptor_, bytes.data(), bytes.size());
      if (count > 0) {
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      return;
    }
  }

private:
  extension::HostProcess* host_;
  extension::Services* services_{nullptr};
  ConfigurationReloader* reloader_{nullptr};
  int read_descriptor_{-1};
  int write_descriptor_{-1};
};

void record_child_exit([[maybe_unused]] const int signal_number) noexcept {
  const int preserved_errno = errno;
  child_exit_pending = 1;
  const auto descriptor = static_cast<int>(child_exit_wakeup_descriptor);
  if (descriptor >= 0) {
    constexpr std::byte wake{1};
    while (::write(descriptor, &wake, sizeof(wake)) < 0 && errno == EINTR) {
    }
  }
  errno = preserved_errno;
}

[[nodiscard]] auto reap_child(void* const context) noexcept -> std::optional<core::ChildExit> {
  if (child_exit_pending == 0) {
    return std::nullopt;
  }
  auto& reaper = *static_cast<ChildExitReaper*>(context);
  // Clear before draining and probing so a concurrent SIGCHLD leaves both a pending flag and a
  // readable wake byte. Returning one child keeps the pass active until waitpid is exhausted.
  child_exit_pending = 0;
  reaper.drain_wakeup();
  int status = 0;
  const auto process = reaper.reap_process(status);
  if (process > 0) {
    child_exit_pending = 1;
    return core::ChildExit{.process = static_cast<int>(process), .status = status};
  }
  if (process < 0 && errno == EINTR) {
    child_exit_pending = 1;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr auto valid_session_name(const std::string_view session) noexcept -> bool {
  if (session.empty() || session.front() == '-' ||
      session.size() > protocol::session_name_bytes_max) {
    return false;
  }
  return std::ranges::all_of(session, [](const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
  });
}

[[nodiscard]] constexpr auto valid_tab_title(const std::string_view title) noexcept -> bool {
  return title.size() <= protocol::tab_title_bytes_max &&
         std::ranges::all_of(title, [](const char character) {
           const auto byte = static_cast<unsigned char>(character);
           return byte >= 0x20U && byte <= 0x7eU;
         });
}

[[nodiscard]] auto socket_address(const std::string& path) noexcept
    -> std::expected<sockaddr_un, int> {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    return std::unexpected(ENAMETOOLONG);
  }
  std::memcpy(std::span(address.sun_path).data(), path.c_str(), path.size() + 1U);
  return address;
}

constexpr std::string_view development_build_id_extension = ".build-id";
constexpr std::size_t development_build_id_bytes = 64;

[[nodiscard]] constexpr auto valid_development_build_id(const std::string_view value) noexcept
    -> bool {
  return value.size() == development_build_id_bytes &&
         std::ranges::all_of(value, [](const char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] auto development_build_id() noexcept -> std::optional<std::string_view> {
  const char* const value = std::getenv("LEMMA_DEV_BUILD_ID");
  if (value == nullptr) {
    return std::nullopt;
  }
  const std::string_view build_id(value);
  return valid_development_build_id(build_id) ? std::optional{build_id} : std::nullopt;
}

[[nodiscard]] auto development_build_id_path(const std::string& socket_path,
                                             const std::span<char> output) noexcept -> bool {
  if (socket_path.size() + development_build_id_extension.size() + 1U > output.size()) {
    return false;
  }
  std::ranges::copy(socket_path, output.begin());
  const auto suffix = output.subspan(socket_path.size());
  std::ranges::copy(development_build_id_extension, suffix.begin());
  suffix.subspan(development_build_id_extension.size(), 1).front() = '\0';
  return true;
}

[[nodiscard]] auto read_file_exact(const int descriptor, const std::span<char> output) noexcept
    -> bool {
  std::size_t offset = 0;
  while (offset < output.size()) {
    const auto available = output.subspan(offset);
    const auto result = ::read(descriptor, available.data(), available.size());
    if (result > 0) {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  std::array<char, 1> trailing{};
  while (true) {
    const auto result = ::read(descriptor, trailing.data(), trailing.size());
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return result == 0;
  }
}

[[nodiscard]] auto development_build_matches(const std::string& path) noexcept -> bool {
  const char* const configured = std::getenv("LEMMA_DEV_BUILD_ID");
  if (configured == nullptr) {
    return true;
  }
  const auto expected = development_build_id();
  std::array<char, 256> marker_path{};
  if (!expected.has_value() || !development_build_id_path(path, marker_path)) {
    return false;
  }
  // open is variadic even when no creation mode argument is present.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  int descriptor = ::open(marker_path.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  struct stat details{};
  std::array<char, development_build_id_bytes> recorded{};
  const bool regular = ::fstat(descriptor, &details) == 0 && S_ISREG(details.st_mode) &&
                       details.st_uid == ::getuid();
  const bool matches =
      regular && read_file_exact(descriptor, recorded) && std::ranges::equal(recorded, *expected);
  close_descriptor(descriptor);
  return matches;
}

[[nodiscard]] auto open_connection_unchecked(const std::string& path) noexcept -> int {
  int connection = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (connection < 0) {
    return -1;
  }
  const auto address = socket_address(path);
  if (!address.has_value()) {
    close_descriptor(connection);
    return -1;
  }
  // The C socket ABI erases the concrete sockaddr type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* generic_address = reinterpret_cast<const sockaddr*>(&*address);
  if (::connect(connection, generic_address, sizeof(*address)) != 0) {
    close_descriptor(connection);
    return -1;
  }
  return connection;
}

[[nodiscard]] auto open_connection(const std::string& path) noexcept -> int {
  return development_build_matches(path) ? open_connection_unchecked(path) : -1;
}

[[nodiscard]] auto acquire_server_lock(const std::string& path, int& lock_descriptor) noexcept
    -> bool {
  std::array<char, 256> lock_path{};
  constexpr std::string_view extension = ".lock";
  if (path.size() + extension.size() + 1U > lock_path.size()) {
    return false;
  }
  std::memcpy(lock_path.data(), path.data(), path.size());
  std::memcpy(std::span(lock_path).subspan(path.size()).data(), extension.data(), extension.size());
  // open is variadic because O_CREAT requires a file mode.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  lock_descriptor = ::open(lock_path.data(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock_descriptor < 0 || ::flock(lock_descriptor, LOCK_EX | LOCK_NB) != 0) {
    close_descriptor(lock_descriptor);
    return false;
  }
  return true;
}

[[nodiscard]] auto remove_stale_socket(const std::string& path) noexcept -> bool {
  struct stat existing{};
  if (::lstat(path.c_str(), &existing) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::getuid()) {
    return false;
  }
  int existing_server = open_connection_unchecked(path);
  if (existing_server >= 0) {
    close_descriptor(existing_server);
    return false;
  }
  return ::unlink(path.c_str()) == 0;
}

[[nodiscard]] auto publish_development_build_id(const std::string& path,
                                                std::array<char, 256>& marker_path) noexcept
    -> bool {
  const char* const configured = std::getenv("LEMMA_DEV_BUILD_ID");
  if (configured == nullptr) {
    return true;
  }
  const auto build_id = development_build_id();
  if (!build_id.has_value() || !development_build_id_path(path, marker_path)) {
    return false;
  }

  std::array<char, 272> temporary_path{};
  constexpr std::string_view extension = ".tmp";
  const std::string_view marker(marker_path.data());
  if (marker.size() + extension.size() + 1U > temporary_path.size()) {
    return false;
  }
  std::ranges::copy(marker, temporary_path.begin());
  const auto suffix = std::span(temporary_path).subspan(marker.size());
  std::ranges::copy(extension, suffix.begin());
  suffix.subspan(extension.size(), 1).front() = '\0';

  // open is variadic because O_CREAT requires a file mode.
  int descriptor =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      ::open(temporary_path.data(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    return false;
  }
  const bool written =
      write_all(descriptor, std::as_bytes(std::span(build_id->data(), build_id->size())));
  close_descriptor(descriptor);
  if (!written || ::rename(temporary_path.data(), marker_path.data()) != 0) {
    static_cast<void>(::unlink(temporary_path.data()));
    return false;
  }
  return true;
}

[[nodiscard]] auto create_listener(const std::string& path, int& lock_descriptor,
                                   std::array<char, 256>& marker_path) noexcept -> int {
  if (!acquire_server_lock(path, lock_descriptor) || !remove_stale_socket(path)) {
    return -1;
  }
  int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0) {
    return -1;
  }
  const auto address = socket_address(path);
  if (!address.has_value()) {
    close_descriptor(listener);
    return -1;
  }
  // The C socket ABI erases the concrete sockaddr type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* generic_address = reinterpret_cast<const sockaddr*>(&*address);
  if (::bind(listener, generic_address, sizeof(*address)) != 0) {
    close_descriptor(listener);
    return -1;
  }
  if (::chmod(path.c_str(), 0600) != 0 || !publish_development_build_id(path, marker_path) ||
      ::listen(listener, 16) != 0) {
    close_descriptor(listener);
    static_cast<void>(::unlink(path.c_str()));
    if (marker_path.front() != '\0') {
      static_cast<void>(::unlink(marker_path.data()));
    }
    return -1;
  }
  return listener;
}

struct OwnedEndpoint final {
  const char* path;
  int listener;
  int server_lock;
  std::array<char, 256> development_build_id_path;
};

void release_owned_endpoint(void* const context) noexcept {
  auto& endpoint = *static_cast<OwnedEndpoint*>(context);
  close_descriptor(endpoint.listener);
  static_cast<void>(::unlink(endpoint.path));
  if (endpoint.development_build_id_path.front() != '\0') {
    static_cast<void>(::unlink(endpoint.development_build_id_path.data()));
  }
  close_descriptor(endpoint.server_lock);
}

[[nodiscard]] auto run_owned_server(const std::string& path, const ServeOptions options) noexcept
    -> int {
  static_cast<void>(::signal(SIGPIPE, SIG_IGN));
  auto configured_runtime = extension::load_configuration();
  if (configured_runtime.status == extension::ConfigurationStatus::invalid) {
    static_cast<void>(write_text(STDERR_FILENO, "lemma configuration rejected"));
    if (!configured_runtime.path.empty()) {
      static_cast<void>(write_text(STDERR_FILENO, ": "));
      static_cast<void>(write_text(STDERR_FILENO, configured_runtime.path));
    }
    if (!configured_runtime.diagnostic.empty()) {
      static_cast<void>(write_text(STDERR_FILENO, ": "));
      static_cast<void>(write_text(STDERR_FILENO, configured_runtime.diagnostic));
    }
    static_cast<void>(write_text(STDERR_FILENO, "\n"));
    configured_runtime = extension::load_builtin_configuration();
  }
  ConfigurationReloader reloader(configured_runtime);
  ChildExitReaper child_reaper(&configured_runtime.host);
  child_reaper.reloader(reloader);
  struct sigaction child_action{};
  child_action.sa_handler = &record_child_exit;
  if (!child_reaper.valid() || sigemptyset(&child_action.sa_mask) != 0 ||
      ::sigaction(SIGCHLD, &child_action, nullptr) != 0) {
    return 1;
  }
  child_exit_pending = 0;
  const auto previous_mask = ::umask(0077);
  int server_lock = -1;
  std::array<char, 256> development_marker{};
  int listener = create_listener(path, server_lock, development_marker);
  static_cast<void>(::umask(previous_mask));
  if (listener < 0) {
    close_descriptor(server_lock);
    return 1;
  }
  OwnedEndpoint endpoint{
      .path = path.c_str(),
      .listener = listener,
      .server_lock = server_lock,
      .development_build_id_path = development_marker,
  };
  std::optional<extension::Services> services;
  try {
    services.emplace(configured_runtime.generation != nullptr
                         ? configured_runtime.generation->extensions()
                         : std::span<const config::ExtensionConfiguration>{},
                     path);
  } catch (...) {
    release_owned_endpoint(&endpoint);
    return 1;
  }
  child_reaper.services(*services);
  child_exit_wakeup_descriptor = child_reaper.write_descriptor();
  auto reactor_environment = core::production_reactor_environment();
  reactor_environment.configuration_reloader = &reloader;
  if (configured_runtime.generation != nullptr) {
    reactor_environment.input_map = &configured_runtime.generation->input_map();
    reactor_environment.scrollback_lines = configured_runtime.generation->scrollback_lines();
    reactor_environment.default_program = configured_runtime.generation->default_program();
    reactor_environment.default_cwd = configured_runtime.generation->default_cwd();
    reactor_environment.command_history_file = configured_runtime.generation->history_file();
    reactor_environment.status_line = configured_runtime.generation->status_line();
    reactor_environment.outer_title = configured_runtime.generation->outer_title();
    reactor_environment.clipboard_read = configured_runtime.generation->clipboard_read();
    reactor_environment.clipboard_write = configured_runtime.generation->clipboard_write();
    if (!configured_runtime.commands.empty()) {
      reactor_environment.extension_descriptor = configured_runtime.host.descriptor();
      reactor_environment.extension_commands = configured_runtime.commands;
      reactor_environment.stop_extension = [](void* context) noexcept {
        static_cast<extension::HostProcess*>(context)->terminate();
      };
      reactor_environment.extension_context = &configured_runtime.host;
    }
  }
  const auto result = core::run_server_with_environment(
      listener, &release_owned_endpoint, &endpoint, options.stop_requested,
      {.wake_descriptor = child_reaper.read_descriptor(),
       .reap = &reap_child,
       .context = &child_reaper},
      reactor_environment);
  child_exit_wakeup_descriptor = -1;
  return result;
}

[[nodiscard]] auto server_available(const std::string& path) noexcept -> bool {
  int connection = open_connection(path);
  if (connection < 0) {
    return false;
  }
  const std::array command{protocol::wire_byte(protocol::ControlCommand::list)};
  const auto sent = send_all(connection, command);
  std::array<std::byte, 1> response{};
  const auto received = sent && read_exact(connection, response);
  close_descriptor(connection);
  return received;
}

void redirect_standard_descriptors() noexcept {
  // open is variadic when file creation mode is present.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  int null_descriptor = ::open("/dev/null", O_RDWR | O_NOCTTY);
  if (null_descriptor < 0) {
    return;
  }
  static_cast<void>(::dup2(null_descriptor, STDIN_FILENO));
  static_cast<void>(::dup2(null_descriptor, STDOUT_FILENO));
  static_cast<void>(::dup2(null_descriptor, STDERR_FILENO));
  if (null_descriptor > STDERR_FILENO) {
    close_descriptor(null_descriptor);
  }
}

[[nodiscard]] auto launch_server(const std::string& path) noexcept -> bool {
  const auto first_child = ::fork();
  if (first_child < 0) {
    return false;
  }
  if (first_child == 0) {
    if (::setsid() < 0) {
      ::_exit(1);
    }
    const auto daemon_child = ::fork();
    if (daemon_child < 0) {
      ::_exit(1);
    }
    if (daemon_child > 0) {
      ::_exit(0);
    }
    redirect_standard_descriptors();
    ::_exit(run_owned_server(path, {}));
  }

  static_cast<void>(::waitpid(first_child, nullptr, 0));
  for (std::size_t attempt = 0; attempt < 200; ++attempt) {
    if (server_available(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

[[nodiscard]] auto endpoint_lock_available(const std::string& path) noexcept -> bool {
  int descriptor = -1;
  const bool available = acquire_server_lock(path, descriptor);
  close_descriptor(descriptor);
  return available;
}

enum class DevelopmentServerProbeStatus : std::uint8_t {
  matching,
  absent,
  stale,
  timed_out,
};

struct DevelopmentServerProbe final {
  DevelopmentServerProbeStatus status{DevelopmentServerProbeStatus::timed_out};
  int connection{-1};
};

[[nodiscard]] auto probe_development_server(const std::string& path) noexcept
    -> DevelopmentServerProbe {
  for (std::size_t attempt = 0; attempt < 200; ++attempt) {
    if (development_build_matches(path)) {
      return {.status = DevelopmentServerProbeStatus::matching};
    }
    int connection = open_connection_unchecked(path);
    if (connection >= 0) {
      return {.status = DevelopmentServerProbeStatus::stale, .connection = connection};
    }
    if (endpoint_lock_available(path)) {
      return {.status = DevelopmentServerProbeStatus::absent};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return {};
}

[[nodiscard]] auto wait_for_endpoint_release(const std::string& path) noexcept -> bool {
  for (std::size_t attempt = 0; attempt < 200; ++attempt) {
    int connection = open_connection_unchecked(path);
    if (connection >= 0) {
      close_descriptor(connection);
    } else if (endpoint_lock_available(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

[[nodiscard]] auto stop_stale_development_server(const std::string& path) noexcept -> bool {
  const char* const configured = std::getenv("LEMMA_DEV_BUILD_ID");
  if (configured == nullptr || !valid_development_build_id(configured)) {
    return configured == nullptr;
  }
  auto probe = probe_development_server(path);
  if (probe.status == DevelopmentServerProbeStatus::matching ||
      probe.status == DevelopmentServerProbeStatus::absent) {
    return true;
  }
  if (probe.status != DevelopmentServerProbeStatus::stale) {
    return false;
  }
  const std::array command{protocol::wire_byte(protocol::ControlCommand::shutdown)};
  std::array<std::byte, protocol::shutdown_response.size()> response{};
  const bool stopped =
      send_all(probe.connection, command) && read_exact(probe.connection, response);
  close_descriptor(probe.connection);
  const auto expected = std::as_bytes(
      std::span(protocol::shutdown_response.data(), protocol::shutdown_response.size()));
  return stopped && std::ranges::equal(response, expected) && wait_for_endpoint_release(path);
}

[[nodiscard]] auto ensure_server(const std::string& path) noexcept -> bool {
  if (server_available(path)) {
    return true;
  }
  return stop_stale_development_server(path) && launch_server(path);
}

[[nodiscard]] auto send_session_request(const int connection,
                                        const protocol::ControlCommand command,
                                        const std::string_view session) noexcept -> bool {
  const auto header = protocol::encode_session_header(command, session);
  return send_all(connection, header) &&
         send_all(connection, std::as_bytes(std::span(session.data(), session.size())));
}

[[nodiscard]] auto capture_home_directory(const std::span<char> output) noexcept -> std::size_t {
  return platform::account_home_directory(output);
}

[[nodiscard]] auto
capture_working_directory(const std::string_view requested,
                          std::array<char, protocol::working_directory_bytes_max + 1U>& output,
                          bool& used_home_directory) noexcept -> std::optional<std::string_view> {
  used_home_directory = false;
  if (requested.empty()) {
    if (::getcwd(output.data(), output.size()) != nullptr) {
      return std::string_view(output.data());
    }
    const auto home_size = capture_home_directory(output);
    if (home_size == 0) {
      return std::nullopt;
    }
    used_home_directory = true;
    return std::string_view(output.data(), home_size);
  }
  if (requested.contains('\0') || requested.size() > protocol::working_directory_bytes_max) {
    return std::nullopt;
  }
  std::array<char, protocol::working_directory_bytes_max + 1U> candidate{};
  std::ranges::copy(requested, candidate.begin());
  if (::realpath(candidate.data(), output.data()) == nullptr) {
    return std::nullopt;
  }
  struct stat info{};
  const std::string_view resolved(output.data());
  if (resolved.size() > protocol::working_directory_bytes_max ||
      ::stat(output.data(), &info) != 0 || !S_ISDIR(info.st_mode)) {
    return std::nullopt;
  }
  return resolved;
}

[[nodiscard]] auto
encode_launch_command(const std::span<const std::string_view> arguments,
                      std::array<std::byte, protocol::command_bytes_max>& output) noexcept
    -> std::optional<std::size_t> {
  if (arguments.size() > protocol::command_arguments_max ||
      (!arguments.empty() && arguments.front().empty())) {
    return std::nullopt;
  }
  std::size_t size = 0;
  for (const auto argument : arguments) {
    if (argument.contains('\0') || argument.size() + 1U > output.size() - size) {
      return std::nullopt;
    }
    std::ranges::copy(std::as_bytes(std::span(argument.data(), argument.size())),
                      std::span(output).subspan(size).begin());
    size += argument.size();
    std::span(output).subspan(size, 1).front() = std::byte{0};
    ++size;
  }
  return size;
}

struct CapturedLaunchContext final {
  std::array<char, protocol::working_directory_bytes_max + 1U> working_directory{};
  std::array<std::byte, protocol::environment_bytes_max> environment{};
  std::array<std::byte, protocol::command_bytes_max> command{};
  std::size_t working_directory_size{0};
  std::size_t environment_size{0};
  std::size_t command_size{0};
  bool hold{false};
};

[[nodiscard]] auto capture_launch_context(const LaunchOptions options,
                                          CapturedLaunchContext& context) noexcept -> bool {
  bool used_home_directory = false;
  const auto working_directory = capture_working_directory(
      options.working_directory, context.working_directory, used_home_directory);
  if (!working_directory.has_value()) {
    static_cast<void>(write_text(STDERR_FILENO, "invalid or unavailable working directory\n"));
    return false;
  }
  context.working_directory_size = working_directory->size();

  const auto environment_size = platform::capture_process_environment(context.environment);
  if (!environment_size.has_value()) {
    static_cast<void>(write_text(STDERR_FILENO, "launch environment exceeds lemma limits\n"));
    return false;
  }
  context.environment_size = *environment_size;

  const auto command_size = encode_launch_command(options.command, context.command);
  if (!command_size.has_value()) {
    static_cast<void>(write_text(STDERR_FILENO, "launch command exceeds lemma limits\n"));
    return false;
  }
  context.command_size = *command_size;
  context.hold = options.hold;
  return !used_home_directory ||
         write_text(
             STDERR_FILENO,
             "warning: current directory unavailable; new session will use home directory\n");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto capture_control_command(const RuntimeEndpoint& endpoint,
                                           const protocol::ControlCommand command,
                                           const std::string_view session) -> TextResult {
  int connection = open_connection(std::string(endpoint.socket_path()));
  if (connection < 0) {
    return {};
  }
  const bool named = !session.empty();
  const std::array encoded_command{protocol::wire_byte(command)};
  const bool sent = named ? send_session_request(connection, command, session)
                          : send_all(connection, encoded_command);
  if (!sent) {
    close_descriptor(connection);
    return {};
  }

  TextResult result{.status = OperationStatus::applied, .text = {}};
  std::array<std::byte, std::size_t{4} * 1'024U> response{};
  bool first = true;
  while (true) {
    const auto bytes_read = ::recv(connection, response.data(), response.size(), 0);
    if (bytes_read == 0) {
      break;
    }
    if (bytes_read < 0) {
      if (errno == EINTR) {
        continue;
      }
      close_descriptor(connection);
      return {};
    }
    const auto bytes = std::span(response).first(static_cast<std::size_t>(bytes_read));
    if (first && bytes.front() == response_missing) {
      close_descriptor(connection);
      return {.status = OperationStatus::missing, .text = {}};
    }
    first = false;
    // Control listing bytes are printable UTF-8/ASCII by construction.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    result.text.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  close_descriptor(connection);
  return first ? TextResult{} : result;
}

[[nodiscard]] auto run_control_command(const RuntimeEndpoint& endpoint,
                                       const protocol::ControlCommand command,
                                       const std::string_view session, const bool report_missing)
    -> int {
  const auto result = capture_control_command(endpoint, command, session);
  if (result.status != OperationStatus::applied) {
    if (report_missing) {
      static_cast<void>(write_text(STDERR_FILENO, result.status == OperationStatus::missing
                                                      ? "no lemma session\n"
                                                      : "no lemma daemon\n"));
    }
    return 1;
  }
  return write_text(STDOUT_FILENO, result.text) ? 0 : 1;
}

[[nodiscard]] auto report_rename_status(const OperationStatus status) -> int {
  if (status == OperationStatus::applied) {
    return 0;
  }
  if (status == OperationStatus::missing) {
    static_cast<void>(write_text(STDERR_FILENO, "no matching lemma session or tab\n"));
  } else if (status == OperationStatus::conflict) {
    static_cast<void>(write_text(STDERR_FILENO, "lemma session name already exists\n"));
  } else if (status == OperationStatus::capacity) {
    static_cast<void>(write_text(STDERR_FILENO, "lemma identity capacity reached\n"));
  } else {
    static_cast<void>(write_text(STDERR_FILENO, "failed to rename lemma session or tab\n"));
  }
  return 1;
}

[[nodiscard]] auto run_shutdown_command(const RuntimeEndpoint& endpoint) -> int {
  int connection = open_connection(std::string(endpoint.socket_path()));
  if (connection < 0) {
    static_cast<void>(write_text(STDERR_FILENO, "no lemma daemon\n"));
    return 1;
  }
  const std::array command{protocol::wire_byte(protocol::ControlCommand::shutdown)};
  if (!send_all(connection, command)) {
    close_descriptor(connection);
    static_cast<void>(write_text(STDERR_FILENO, "failed to shut down lemma daemon\n"));
    return 1;
  }

  std::array<std::byte, protocol::shutdown_response.size()> response{};
  const bool received = read_exact(connection, response);
  close_descriptor(connection);
  const auto expected = std::as_bytes(
      std::span(protocol::shutdown_response.data(), protocol::shutdown_response.size()));
  if (received && std::ranges::equal(response, expected)) {
    return write_all(STDOUT_FILENO, expected) ? 0 : 1;
  }
  static_cast<void>(write_text(STDERR_FILENO, response.front() == response_capacity
                                                  ? "lemma daemon connection capacity reached\n"
                                                  : "failed to shut down lemma daemon\n"));
  return 1;
}

[[nodiscard]] constexpr auto operation_status(const std::byte response) noexcept
    -> OperationStatus {
  if (response == response_ready) {
    return OperationStatus::applied;
  }
  if (response == response_no_effect) {
    return OperationStatus::no_effect;
  }
  if (response == response_missing) {
    return OperationStatus::missing;
  }
  if (response == response_conflict) {
    return OperationStatus::conflict;
  }
  if (response == response_capacity) {
    return OperationStatus::capacity;
  }
  if (response == response_unavailable) {
    return OperationStatus::unavailable;
  }
  return OperationStatus::failed;
}

[[nodiscard]] auto open_payload_request(const RuntimeEndpoint& endpoint,
                                        const protocol::ControlCommand command,
                                        const std::string_view session,
                                        const std::span<const std::byte> payload) -> int {
  if (!valid_session_name(session) || payload.empty() ||
      payload.size() > protocol::control_payload_bytes_max) {
    return -1;
  }
  int connection = open_connection(std::string(endpoint.socket_path()));
  const auto size = protocol::encode_bounded_size(payload.size());
  if (connection < 0 || !send_session_request(connection, command, session) ||
      !send_all(connection, size) || !send_all(connection, payload)) {
    close_descriptor(connection);
    return -1;
  }
  return connection;
}

[[nodiscard]] constexpr auto decode_control_u32(const std::span<const std::byte, 4> input) noexcept
    -> std::uint32_t {
  return (std::to_integer<std::uint32_t>(input.subspan(0, 1).front()) << 24U) |
         (std::to_integer<std::uint32_t>(input.subspan(1, 1).front()) << 16U) |
         (std::to_integer<std::uint32_t>(input.subspan(2, 1).front()) << 8U) |
         std::to_integer<std::uint32_t>(input.subspan(3, 1).front());
}

template <typename Id>
[[nodiscard]] auto parse_public_id(std::string_view value) noexcept -> std::optional<Id>;

[[nodiscard]] auto event_request(const std::optional<std::string_view> session,
                                 const std::optional<PaneId> pane, const bool screen)
    -> std::optional<std::string> {
  const auto session_id = session.has_value() ? parse_public_id<SessionId>(*session) : std::nullopt;
  if ((session.has_value() && !session_id.has_value() && !valid_session_name(*session)) ||
      (pane.has_value() && (!pane->is_valid() || !session.has_value())) ||
      (screen && !pane.has_value())) {
    return std::nullopt;
  }
  try {
    std::string request = R"({"schema":"lemma.events/v1")";
    if (session_id.has_value()) {
      request += R"(,"session":{"id":")";
      request += std::to_string(session_id->slot());
      request += ":";
      request += std::to_string(session_id->generation());
      request += "\"}";
    } else if (session.has_value()) {
      request += R"(,"session":{"name":")";
      request += *session;
      request += "\"}";
    }
    if (pane.has_value()) {
      request += R"(,"pane":{"id":")";
      request += std::to_string(pane->slot());
      request += ":";
      request += std::to_string(pane->generation());
      request += "\"}";
    }
    if (screen) {
      request += R"(,"screen":true)";
    }
    request += "}\n";
    return request;
  } catch (...) {
    return std::nullopt;
  }
}

// Multi-Pane request encoding keeps one closed public grammar and one legacy single-Pane spelling.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto event_request(const std::optional<std::string_view> session,
                                 const std::span<const PaneId> panes, const bool screen)
    -> std::optional<std::string> {
  if (panes.size() > api::event_panes_max || (screen && panes.empty()) ||
      (!panes.empty() && !session.has_value())) {
    return std::nullopt;
  }
  if (panes.size() <= 1U) {
    return event_request(
        session, panes.empty() ? std::optional<PaneId>{} : std::optional{panes.front()}, screen);
  }
  const auto session_value = session.value_or(std::string_view{});
  const auto session_id = parse_public_id<SessionId>(session_value);
  if (!session_id.has_value() && !valid_session_name(session_value)) {
    return std::nullopt;
  }
  try {
    std::string request = R"({"schema":"lemma.events/v1")";
    if (session_id.has_value()) {
      request += R"(,"session":{"id":")" + std::to_string(session_id->slot()) + ":" +
                 std::to_string(session_id->generation()) + "\"}";
    } else {
      request += R"(,"session":{"name":")" + std::string(session_value) + "\"}";
    }
    request += R"(,"panes":[)";
    bool separator = false;
    for (const auto pane : panes) {
      if (!pane.is_valid()) {
        return std::nullopt;
      }
      if (separator) {
        request += ',';
      }
      separator = true;
      request += R"({"id":")" + std::to_string(pane.slot()) + ":" +
                 std::to_string(pane.generation()) + "\"}";
    }
    request += screen ? "],\"screen\":true}\n" : "]}\n";
    return request;
  } catch (...) {
    return std::nullopt;
  }
}

enum class EventReadError : std::uint8_t {
  timeout,
  closed,
  failed,
};

class EventReader final {
public:
  explicit EventReader(const int descriptor) noexcept : descriptor_(descriptor) {}

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto next(const std::chrono::steady_clock::time_point deadline)
      -> std::expected<std::string, EventReadError> {
    while (true) {
      if (const auto newline = buffered_.find('\n'); newline != std::string::npos) {
        auto line = buffered_.substr(0, newline);
        buffered_.erase(0, newline + 1U);
        return line;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return std::unexpected(EventReadError::timeout);
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      pollfd descriptor{.fd = descriptor_, .events = POLLIN, .revents = 0};
      const auto waited =
          ::poll(&descriptor, 1, static_cast<int>(std::max(remaining.count(), std::int64_t{1})));
      if (waited == 0) {
        return std::unexpected(EventReadError::timeout);
      }
      if (waited < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::unexpected(EventReadError::failed);
      }
      std::array<char, std::size_t{16} * 1'024U> input{};
      const auto received = ::recv(descriptor_, input.data(), input.size(), 0);
      if (received > 0) {
        const auto size = static_cast<std::size_t>(received);
        if (size > api::json_bytes_max - buffered_.size()) {
          return std::unexpected(EventReadError::failed);
        }
        try {
          buffered_.append(input.data(), size);
        } catch (...) {
          return std::unexpected(EventReadError::failed);
        }
        continue;
      }
      if (received < 0 && errno == EINTR) {
        continue;
      }
      return std::unexpected(received == 0 ? EventReadError::closed : EventReadError::failed);
    }
  }

private:
  int descriptor_{-1};
  std::string buffered_;
};

[[nodiscard]] auto public_operation_status(const api::JsonValue& result) noexcept
    -> OperationStatus {
  const auto status = api::json_string(result, "status");
  if (status == std::optional<std::string_view>{"applied"}) {
    return OperationStatus::applied;
  }
  if (status == std::optional<std::string_view>{"no_effect"}) {
    return OperationStatus::no_effect;
  }
  if (status == std::optional<std::string_view>{"missing"} ||
      status == std::optional<std::string_view>{"stale"} ||
      status == std::optional<std::string_view>{"wrong_owner"}) {
    return OperationStatus::missing;
  }
  if (status == std::optional<std::string_view>{"conflict"}) {
    return OperationStatus::conflict;
  }
  if (status == std::optional<std::string_view>{"capacity"}) {
    return OperationStatus::capacity;
  }
  if (status == std::optional<std::string_view>{"unavailable"}) {
    return OperationStatus::unavailable;
  }
  return OperationStatus::failed;
}

[[nodiscard]] auto
invoke_public_request(const RuntimeEndpoint& endpoint, std::string request,
                      const std::chrono::milliseconds response_timeout = std::chrono::seconds(10))
    -> std::optional<api::JsonValue> {
  try {
    if (!request.ends_with('\n')) {
      request.push_back('\n');
    }
  } catch (...) {
    return std::nullopt;
  }
  int connection = open_connection(std::string(endpoint.socket_path()));
  if (connection < 0 ||
      !send_all(connection, std::as_bytes(std::span(request.data(), request.size())))) {
    close_descriptor(connection);
    return std::nullopt;
  }
  EventReader reader(connection);
  const auto line = reader.next(std::chrono::steady_clock::now() + response_timeout);
  close_descriptor(connection);
  if (!line.has_value()) {
    return std::nullopt;
  }
  auto parsed = api::parse_json(*line);
  return parsed.value.has_value() ? std::move(parsed.value) : std::nullopt;
}

[[nodiscard]] auto
invoke_public_command(const RuntimeEndpoint& endpoint, const std::string_view command,
                      const std::chrono::milliseconds response_timeout = std::chrono::seconds(10))
    -> std::optional<api::JsonValue> {
  try {
    std::string request = R"({"schema":"lemma.proc/v1","commands":[)";
    request += command;
    request += "]}";
    auto proc_result = invoke_public_request(endpoint, std::move(request), response_timeout);
    if (!proc_result.has_value()) {
      return std::nullopt;
    }
    const auto* const results = api::json_member(*proc_result, "results");
    if (results != nullptr && results->kind == api::JsonKind::array && !results->array.empty()) {
      const auto* const result = api::json_member(results->array.front(), "result");
      if (result != nullptr) {
        return *result;
      }
    }
    return proc_result;
  } catch (...) {
    return std::nullopt;
  }
}

struct ProcRequestPolicy final {
  std::chrono::milliseconds response_timeout{std::chrono::seconds(10)};
  bool starts_session{false};
};

[[nodiscard]] auto proc_request_policy(const api::JsonValue& document) noexcept
    -> ProcRequestPolicy {
  ProcRequestPolicy policy;
  const auto* const commands = api::json_member(document, "commands");
  if (commands == nullptr || commands->kind != api::JsonKind::array) {
    return policy;
  }
  for (const auto& command : commands->array) {
    const auto name = api::json_string(command, "command");
    policy.starts_session =
        policy.starts_session || name == std::optional<std::string_view>{"session.start"};
    if (name != std::optional<std::string_view>{"pane.wait"}) {
      continue;
    }
    const auto timeout = std::min(
        api::json_unsigned(command, "timeout_ms").value_or(api::wait_timeout_default_milliseconds),
        static_cast<std::uint64_t>(api::wait_timeout_max_milliseconds));
    policy.response_timeout += std::chrono::milliseconds(static_cast<std::int64_t>(timeout));
  }
  return policy;
}

[[nodiscard]] auto append_public_session_selector(std::string& request,
                                                  const std::string_view session) -> bool {
  try {
    request += R"(,"session":{"name":")";
    request += session;
    request += "\"}";
    return true;
  } catch (...) {
    return false;
  }
}

template <typename Id>
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] auto parse_public_id(const std::string_view value) noexcept -> std::optional<Id> {
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

[[nodiscard]] auto append_public_id_selector(std::string& request, const std::string_view field,
                                             const auto id) -> bool {
  try {
    request += ",\"";
    request += field;
    request += R"(":{"id":")";
    request += std::to_string(id.slot());
    request += ":";
    request += std::to_string(id.generation());
    request += "\"}";
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] auto add_command_launch_context(api::Command& command) -> bool {
  if (command.kind != api::CommandKind::session_start ||
      (!command.working_directory.empty() && command.environment_set)) {
    return true;
  }
  std::vector<std::string_view> arguments;
  try {
    arguments.reserve(command.arguments.size());
    for (const auto& argument : command.arguments) {
      arguments.emplace_back(argument);
    }
  } catch (...) {
    return false;
  }
  CapturedLaunchContext context;
  if (!capture_launch_context({.working_directory = command.working_directory,
                               .command = arguments,
                               .hold = command.hold},
                              context)) {
    return false;
  }
  if (!command.environment_set) {
    std::size_t offset = 0;
    while (offset < context.environment_size) {
      const auto remaining =
          std::span(context.environment).first(context.environment_size).subspan(offset);
      const auto terminator = std::ranges::find(remaining, std::byte{0});
      if (terminator == remaining.end()) {
        return false;
      }
      const auto size = static_cast<std::size_t>(std::distance(remaining.begin(), terminator));
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      command.environment.emplace_back(reinterpret_cast<const char*>(remaining.data()), size);
      offset += size + 1U;
    }
    command.environment_set = true;
  }
  return true;
}

[[nodiscard]] auto encode_single_command_proc(const api::Command& command)
    -> std::optional<std::string> {
  const auto encoded = api::encode_command(command);
  if (!encoded.has_value()) {
    return std::nullopt;
  }
  std::string document = R"({"schema":)";
  if (!api::append_json_string(document, api::proc_schema)) {
    return std::nullopt;
  }
  document += R"(,"commands":[)";
  document += *encoded;
  document += "]}";
  return document;
}

} // namespace

[[nodiscard]] auto validate_session(const std::string_view session) noexcept -> bool {
  if (valid_session_name(session)) {
    return true;
  }
  static_cast<void>(write_text(STDERR_FILENO,
                               "invalid session name; use 1-32 ASCII letters, digits, "
                               "underscores, or hyphens, and do not start with a hyphen\n"));
  return false;
}

[[nodiscard]] auto RuntimeEndpoint::create(const std::string_view socket_path)
    -> std::optional<RuntimeEndpoint> {
  if (socket_path.empty() || socket_path.front() != '/' || socket_path.contains('\0') ||
      !socket_address(std::string(socket_path)).has_value()) {
    return std::nullopt;
  }
  return RuntimeEndpoint(std::string(socket_path));
}

[[nodiscard]] auto default_runtime_endpoint() -> RuntimeEndpoint {
  const char* const development_runtime = std::getenv("LEMMA_DEV_RUNTIME_DIR");
  if (development_runtime != nullptr) {
    const std::string_view directory(development_runtime);
    const char* const configured_build = std::getenv("LEMMA_DEV_BUILD_ID");
    if (directory.empty() || directory.front() != '/' || directory.contains('\0') ||
        configured_build == nullptr || !valid_development_build_id(configured_build)) {
      std::abort();
    }
    auto endpoint = RuntimeEndpoint::create(std::string(directory) + "/daemon.sock");
    if (!endpoint.has_value()) {
      std::abort();
    }
    return std::move(*endpoint);
  }

  auto endpoint = RuntimeEndpoint::create("/tmp/lemma-" + std::to_string(::getuid()) + ".sock");
  // The fixed production path is absolute and well below sockaddr_un::sun_path on supported hosts.
  if (!endpoint.has_value()) {
    std::abort();
  }
  return std::move(*endpoint);
}

[[nodiscard]] auto serve(const RuntimeEndpoint& endpoint, const ServeOptions options) noexcept
    -> int {
  return run_owned_server(endpoint.socket_path_storage(), options);
}

[[nodiscard]] auto open_server_connection(const RuntimeEndpoint& endpoint) -> int {
  return open_connection(std::string(endpoint.socket_path()));
}

namespace {

[[nodiscard]] auto proc_error(const std::string_view reason, const int exit_status)
    -> ProcResponse {
  api::JsonValue document{.kind = api::JsonKind::object, .string = {}, .array = {}, .object = {}};
  document.object.push_back({.key = "schema",
                             .value = {.kind = api::JsonKind::string,
                                       .string = std::string(api::proc_result_schema),
                                       .array = {},
                                       .object = {}}});
  document.object.push_back(
      {.key = "ok",
       .value = {.kind = api::JsonKind::boolean, .string = {}, .array = {}, .object = {}}});
  api::JsonValue error{.kind = api::JsonKind::object, .string = {}, .array = {}, .object = {}};
  error.object.push_back({.key = "reason",
                          .value = {.kind = api::JsonKind::string,
                                    .string = std::string(reason),
                                    .array = {},
                                    .object = {}}});
  document.object.push_back({.key = "error", .value = std::move(error)});
  document.object.push_back(
      {.key = "results",
       .value = {.kind = api::JsonKind::array, .string = {}, .array = {}, .object = {}}});
  return {.document = std::move(document), .exit_status = exit_status};
}

[[nodiscard]] auto request_proc(const RuntimeEndpoint& endpoint, const std::string_view document)
    -> ProcResponse {
  auto parsed = api::parse_json(document);
  if (!parsed.value.has_value()) {
    return proc_error("invalid_json", 2);
  }
  std::string compact;
  if (!api::append_json_value(compact, *parsed.value)) {
    return proc_error("invalid_json", 2);
  }
  const auto policy = proc_request_policy(*parsed.value);
  if (policy.starts_session && !ensure_server(std::string(endpoint.socket_path()))) {
    static_cast<void>(write_text(STDERR_FILENO, "failed to start lemma daemon\n"));
    return proc_error("unavailable", 1);
  }
  auto result = invoke_public_request(endpoint, std::move(compact), policy.response_timeout);
  if (!result.has_value()) {
    return proc_error("unavailable", 1);
  }
  const auto ok = api::json_boolean(*result, "ok");
  const auto partial = api::json_boolean(*result, "partial").value_or(false);
  auto status = ok.value_or(false) ? 0 : 1;
  if (api::json_member(*result, "error") != nullptr && !partial) {
    status = 2;
  }
  return {.document = std::move(*result), .exit_status = status};
}

[[nodiscard]] auto print_proc_response(const ProcResponse& response) -> int {
  std::string encoded;
  return api::append_json_value(encoded, response.document) && write_text(STDOUT_FILENO, encoded) &&
                 write_text(STDOUT_FILENO, "\n")
             ? response.exit_status
             : 1;
}

} // namespace

auto run_proc(const RuntimeEndpoint& endpoint, const std::string_view document) -> int {
  return print_proc_response(request_proc(endpoint, document));
}

auto execute_command(const RuntimeEndpoint& endpoint, const api::Command& command) -> ProcResponse {
  auto concrete = command;
  if (!add_command_launch_context(concrete)) {
    return proc_error("launch_failed", 1);
  }
  const auto document = encode_single_command_proc(concrete);
  return document.has_value() ? request_proc(endpoint, *document)
                              : proc_error("invalid_command", 2);
}

auto run_proc(const RuntimeEndpoint& endpoint, const api::Command& command) -> int {
  return print_proc_response(execute_command(endpoint, command));
}

// Creation reports each bounded setup and daemon outcome without publishing partial client state.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto create_detailed(const RuntimeEndpoint& endpoint, const std::optional<std::string_view> session,
                     const LaunchOptions options) -> SurfaceResult {
  if (session.has_value() && !validate_session(*session)) {
    return {};
  }
  CapturedLaunchContext launch_context;
  if (!capture_launch_context(options, launch_context)) {
    return {};
  }
  const std::string path(endpoint.socket_path());
  if (!ensure_server(path)) {
    static_cast<void>(write_text(STDERR_FILENO, "failed to start lemma daemon\n"));
    return {};
  }
  try {
    std::string request = R"({"command":"session.start")";
    if (session.has_value()) {
      request += R"(,"name":)";
      if (!api::append_json_string(request, *session)) {
        return {};
      }
    }
    if (!options.working_directory.empty()) {
      request += R"(,"cwd":)";
      if (!api::append_json_string(request, {launch_context.working_directory.data(),
                                             launch_context.working_directory_size})) {
        return {};
      }
    }
    if (launch_context.hold) {
      request += R"(,"hold":true)";
    }
    if (!options.command.empty()) {
      request += R"(,"argv":[)";
      for (std::size_t index = 0; index < options.command.size(); ++index) {
        if (index > 0) {
          request += ",";
        }
        if (!api::append_json_string(request,
                                     std::span(options.command).subspan(index, 1).front())) {
          return {};
        }
      }
      request += "]";
    }
    request += R"(,"environment":[)";
    std::size_t environment_offset = 0;
    std::size_t environment_index = 0;
    while (environment_offset < launch_context.environment_size) {
      const auto remaining = std::span(launch_context.environment)
                                 .first(launch_context.environment_size)
                                 .subspan(environment_offset);
      const auto terminator = std::ranges::find(remaining, std::byte{0});
      if (terminator == remaining.end()) {
        return {};
      }
      const auto size = static_cast<std::size_t>(std::distance(remaining.begin(), terminator));
      if (environment_index++ > 0) {
        request += ",";
      }
      // Environment entries are validated byte strings captured from the local POSIX process.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      const std::string_view entry(reinterpret_cast<const char*>(remaining.data()), size);
      if (!api::append_json_string(request, entry)) {
        return {};
      }
      environment_offset += size + 1U;
    }
    request += "]}";
    auto public_result = invoke_public_command(endpoint, request);
    if (public_result.has_value()) {
      const auto status = public_operation_status(*public_result);
      if (status != OperationStatus::applied) {
        if (status == OperationStatus::conflict) {
          static_cast<void>(write_text(STDERR_FILENO, "lemma session already exists\n"));
        } else if (status == OperationStatus::capacity) {
          static_cast<void>(write_text(STDERR_FILENO, "lemma session capacity reached\n"));
        }
        return {.status = status, .session = {}, .tab = {}, .pane = {}};
      }
      const auto* const session_value = api::json_member(*public_result, "session");
      const auto name = session_value == nullptr ? std::optional<std::string_view>{}
                                                 : api::json_string(*session_value, "name");
      const auto tab_text = api::json_string(*public_result, "tab");
      const auto pane_text = api::json_string(*public_result, "pane");
      const auto tab = tab_text.has_value() ? parse_public_id<TabId>(*tab_text) : std::nullopt;
      const auto pane = pane_text.has_value() ? parse_public_id<PaneId>(*pane_text) : std::nullopt;
      if (!name.has_value() || !tab.has_value() || !pane.has_value()) {
        return {};
      }
      return {.status = status, .session = std::string(*name), .tab = *tab, .pane = *pane};
    }
    return {};
  } catch (...) {
    return {};
  }
}

auto create(const RuntimeEndpoint& endpoint, const std::optional<std::string_view> session,
            const LaunchOptions options) -> std::optional<std::string> {
  auto result = create_detailed(endpoint, session, options);
  return result.succeeded() ? std::optional{std::move(result.session)} : std::nullopt;
}

auto start(const RuntimeEndpoint& endpoint, const std::optional<std::string_view> session,
           const LaunchOptions options) -> int {
  const auto created = create(endpoint, session, options);
  return created.has_value() ? list(endpoint, *created) : 1;
}

auto query(const RuntimeEndpoint& endpoint, const QueryKind kind, const std::string_view session)
    -> TextResult {
  if (kind != QueryKind::sessions && !valid_session_name(session)) {
    return {};
  }
  if (kind == QueryKind::sessions) {
    int connection = open_server_connection(endpoint);
    if (connection < 0) {
      return {.status = OperationStatus::applied, .text = "[]"};
    }
    close_descriptor(connection);
  }
  std::string_view command;
  std::string_view field;
  switch (kind) {
  case QueryKind::sessions:
    command = "session.list";
    field = "sessions";
    break;
  case QueryKind::session:
    command = "session.inspect";
    field = "sessions";
    break;
  case QueryKind::tabs:
    command = "tab.list";
    field = "tabs";
    break;
  case QueryKind::panes:
    command = "pane.list";
    field = "panes";
    break;
  }
  try {
    std::string request = R"({"command":)";
    if (!api::append_json_string(request, command) ||
        (kind != QueryKind::sessions && !append_public_session_selector(request, session))) {
      return {};
    }
    request += "}";
    auto result = invoke_public_command(endpoint, request);
    if (!result.has_value()) {
      return {};
    }
    const auto status = public_operation_status(*result);
    const auto* const value = api::json_member(*result, field);
    std::string text;
    if (status != OperationStatus::applied || value == nullptr ||
        !api::append_json_value(text, *value)) {
      return {.status = status, .text = {}};
    }
    return {.status = status, .text = std::move(text)};
  } catch (...) {
    return {};
  }
}

auto list(const RuntimeEndpoint& endpoint) -> int {
  int connection = open_server_connection(endpoint);
  if (connection < 0) {
    static_cast<void>(write_text(STDOUT_FILENO, "no lemma sessions\n"));
    return 0;
  }
  close_descriptor(connection);
  return run_control_command(endpoint, protocol::ControlCommand::list, {}, false);
}

auto list(const RuntimeEndpoint& endpoint, const std::string_view session) -> int {
  return validate_session(session)
             ? run_control_command(endpoint, protocol::ControlCommand::list_session, session, true)
             : 1;
}

auto list_tabs(const RuntimeEndpoint& endpoint, const std::string_view session) -> int {
  return validate_session(session)
             ? run_control_command(endpoint, protocol::ControlCommand::list_tabs, session, true)
             : 1;
}

auto list_panes(const RuntimeEndpoint& endpoint, const std::string_view session) -> int {
  return validate_session(session)
             ? run_control_command(endpoint, protocol::ControlCommand::list_panes, session, true)
             : 1;
}

// Surface creation is cold control work. One bounded payload retains exact argv without exposing
// CLI or JSON grammar to the daemon reactor.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto create_surface(const RuntimeEndpoint& endpoint, const std::string_view session,
                    const SurfaceCreateKind kind, const PaneId target, const SurfaceOptions options)
    -> SurfaceResult {
  if (!validate_session(session) || !valid_tab_title(options.title) ||
      (kind == SurfaceCreateKind::tab && target.is_valid()) ||
      (kind != SurfaceCreateKind::tab && !target.is_valid())) {
    return {};
  }
  std::array<char, protocol::working_directory_bytes_max + 1U> directory{};
  std::size_t directory_size = 0;
  if (!options.working_directory.empty()) {
    bool used_home = false;
    const auto captured =
        capture_working_directory(options.working_directory, directory, used_home);
    if (!captured.has_value() || used_home) {
      static_cast<void>(write_text(STDERR_FILENO, "invalid or unavailable working directory\n"));
      return {};
    }
    directory_size = captured->size();
  }
  std::array<std::byte, protocol::command_bytes_max> command{};
  const auto command_size = encode_launch_command(options.command, command);
  if (!command_size.has_value()) {
    static_cast<void>(write_text(STDERR_FILENO, "launch command exceeds lemma limits\n"));
    return {};
  }
  try {
    std::string request = R"({"command":)";
    if (!api::append_json_string(request,
                                 kind == SurfaceCreateKind::tab ? "tab.new" : "pane.split") ||
        !append_public_session_selector(request, session)) {
      return {};
    }
    if (kind != SurfaceCreateKind::tab) {
      if (!append_public_id_selector(request, "pane", target)) {
        return {};
      }
      request += kind == SurfaceCreateKind::split_right ? R"(,"direction":"right")"
                                                        : R"(,"direction":"down")";
    }
    if (!options.title.empty()) {
      request += R"(,"title":)";
      if (!api::append_json_string(request, options.title)) {
        return {};
      }
    }
    if (directory_size > 0) {
      request += R"(,"cwd":)";
      if (!api::append_json_string(request, {directory.data(), directory_size})) {
        return {};
      }
    }
    if (options.hold) {
      request += R"(,"hold":true)";
    }
    if (!options.command.empty()) {
      request += R"(,"argv":[)";
      for (std::size_t index = 0; index < options.command.size(); ++index) {
        if (index > 0) {
          request += ",";
        }
        if (!api::append_json_string(request,
                                     std::span(options.command).subspan(index, 1).front())) {
          return {};
        }
      }
      request += "]";
    }
    request += "}";
    auto public_result = invoke_public_command(endpoint, request);
    if (public_result.has_value()) {
      const auto status = public_operation_status(*public_result);
      const auto tab_text = api::json_string(*public_result, "tab");
      const auto pane_text = api::json_string(*public_result, "pane");
      const auto tab = tab_text.has_value() ? parse_public_id<TabId>(*tab_text) : std::nullopt;
      const auto pane = pane_text.has_value() ? parse_public_id<PaneId>(*pane_text) : std::nullopt;
      if (status != OperationStatus::applied || !tab.has_value() || !pane.has_value()) {
        return {.status = status, .session = {}, .tab = {}, .pane = {}};
      }
      return {.status = status, .session = std::string(session), .tab = *tab, .pane = *pane};
    }
    return {};
  } catch (...) {
    return {};
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-branch-clone)
auto perform_command(const RuntimeEndpoint& endpoint, const std::string_view session,
                     const SemanticCommand command, const CommandTarget target) -> OperationStatus {
  if (!validate_session(session)) {
    return OperationStatus::failed;
  }
  try {
    std::string_view name;
    switch (command) {
    case SemanticCommand::tab_select:
      name = "tab.select";
      break;
    case SemanticCommand::tab_move:
      name = "tab.move";
      break;
    case SemanticCommand::tab_kill:
      name = "tab.kill";
      break;
    case SemanticCommand::pane_focus:
      name = "pane.focus";
      break;
    case SemanticCommand::pane_swap:
      name = "pane.swap";
      break;
    case SemanticCommand::pane_resize_left:
    case SemanticCommand::pane_resize_right:
    case SemanticCommand::pane_resize_up:
    case SemanticCommand::pane_resize_down:
      name = "pane.resize";
      break;
    case SemanticCommand::pane_zoom_on:
    case SemanticCommand::pane_zoom_off:
      name = "pane.zoom";
      break;
    case SemanticCommand::pane_kill:
      name = "pane.kill";
      break;
    case SemanticCommand::session_kill:
      name = "session.kill";
      break;
    }
    std::string request = R"({"command":)";
    if (!api::append_json_string(request, name) ||
        !append_public_session_selector(request, session)) {
      return OperationStatus::failed;
    }
    if (command == SemanticCommand::tab_select || command == SemanticCommand::tab_move ||
        command == SemanticCommand::tab_kill) {
      request += R"(,"tab":{)";
      if (target.tab.is_valid()) {
        request += R"("id":")" + std::to_string(target.tab.slot()) + ":" +
                   std::to_string(target.tab.generation()) + "\"}";
      } else {
        request += R"("position":)" + std::to_string(target.tab_position) + "}";
      }
      if (command == SemanticCommand::tab_move) {
        request += R"(,"to_position":)" + std::to_string(target.value);
      }
    } else if (command != SemanticCommand::session_kill) {
      if (!append_public_id_selector(request, "pane", target.pane)) {
        return OperationStatus::failed;
      }
      if (command == SemanticCommand::pane_swap &&
          !append_public_id_selector(request, "other", target.peer_pane)) {
        return OperationStatus::failed;
      }
      if (command == SemanticCommand::pane_resize_left ||
          command == SemanticCommand::pane_resize_right ||
          command == SemanticCommand::pane_resize_up ||
          command == SemanticCommand::pane_resize_down) {
        std::string_view direction = "left";
        if (command == SemanticCommand::pane_resize_right) {
          direction = "right";
        } else if (command == SemanticCommand::pane_resize_up) {
          direction = "up";
        } else if (command == SemanticCommand::pane_resize_down) {
          direction = "down";
        }
        request += R"(,"direction":")";
        request += direction;
        request += R"(","amount":)" + std::to_string(target.value);
      } else if (command == SemanticCommand::pane_zoom_on ||
                 command == SemanticCommand::pane_zoom_off) {
        request +=
            command == SemanticCommand::pane_zoom_on ? R"(,"enabled":true)" : R"(,"enabled":false)";
      }
    }
    request += "}";
    const auto result = invoke_public_command(endpoint, request);
    return result.has_value() ? public_operation_status(*result) : OperationStatus::failed;
  } catch (...) {
    return OperationStatus::failed;
  }
}

auto send_pane(const RuntimeEndpoint& endpoint, const std::string_view session, const PaneId pane,
               const std::string_view text) -> OperationStatus {
  if (!validate_session(session) || !pane.is_valid() || text.empty() ||
      text.size() > protocol::control_payload_bytes_max - protocol::control_id_bytes) {
    return OperationStatus::failed;
  }
  try {
    std::string request = R"({"command":"pane.send")";
    if (!append_public_session_selector(request, session) ||
        !append_public_id_selector(request, "pane", pane)) {
      return OperationStatus::failed;
    }
    request += R"(,"text":)";
    if (!api::append_json_string(request, text)) {
      return OperationStatus::failed;
    }
    request += "}";
    const auto result = invoke_public_command(endpoint, request);
    return result.has_value() ? public_operation_status(*result) : OperationStatus::failed;
  } catch (...) {
    return OperationStatus::failed;
  }
}

auto capture_pane(const RuntimeEndpoint& endpoint, const std::string_view session,
                  const PaneId pane) -> std::pair<OperationStatus, std::string> {
  if (!validate_session(session) || !pane.is_valid()) {
    return {OperationStatus::failed, {}};
  }
  try {
    std::string request = R"({"command":"pane.capture")";
    if (!append_public_session_selector(request, session) ||
        !append_public_id_selector(request, "pane", pane)) {
      return {OperationStatus::failed, {}};
    }
    request += "}";
    auto result = invoke_public_command(endpoint, request);
    if (!result.has_value()) {
      return {OperationStatus::failed, {}};
    }
    const auto status = public_operation_status(*result);
    const auto* const capture = api::json_member(*result, "capture");
    const auto text = capture != nullptr && capture->kind == api::JsonKind::object
                          ? api::json_string(*capture, "text")
                          : api::json_string(*result, "text");
    return status == OperationStatus::applied && text.has_value()
               ? std::pair{status, std::string(*text)}
               : std::pair{status, std::string{}};
  } catch (...) {
    return {OperationStatus::failed, {}};
  }
}

auto pane_status(const RuntimeEndpoint& endpoint, const std::string_view session, const PaneId pane)
    -> PaneStatus {
  if (!validate_session(session) || !pane.is_valid()) {
    return {};
  }
  const auto payload = protocol::encode_control_id(pane);
  int connection =
      open_payload_request(endpoint, protocol::ControlCommand::pane_status, session, payload);
  if (connection < 0) {
    return {};
  }
  std::array<std::byte, 7> response{};
  if (!read_exact(connection, response) || response.front() != response_ready) {
    close_descriptor(connection);
    return {
        .status = operation_status(response.front()), .process = ProcessState::running, .value = 0};
  }
  close_descriptor(connection);
  const bool exited = std::to_integer<std::uint8_t>(std::span(response).subspan(1, 1).front()) != 0;
  const auto kind = std::to_integer<std::uint8_t>(std::span(response).subspan(2, 1).front());
  const auto value = decode_control_u32(std::span(response).subspan<3, 4>());
  auto process = ProcessState::running;
  if (exited && kind == 1U) {
    process = ProcessState::exited;
  } else if (exited && kind == 2U) {
    process = ProcessState::signaled;
  } else if (exited) {
    process = ProcessState::exited_unknown;
  }
  return {.status = OperationStatus::applied, .process = process, .value = value};
}

// Every encoded subscription ends with the object's closing brace and LF.
[[nodiscard]] auto request_signals(std::optional<std::string> request)
    -> std::optional<std::string> {
  try {
    if (request.has_value()) {
      request->insert(request->size() - 2U, R"(,"signals":true)");
    }
    return request;
  } catch (...) {
    return std::nullopt;
  }
}

auto events(const RuntimeEndpoint& endpoint, const std::optional<std::string_view> session,
            const std::span<const PaneId> panes, const bool screen, const bool signals) -> int {
  auto request = event_request(session, panes, screen);
  if (signals) {
    request = request_signals(std::move(request));
  }
  if (!request.has_value()) {
    return 2;
  }
  int connection = open_server_connection(endpoint);
  if (connection < 0 ||
      !send_all(connection, std::as_bytes(std::span(request->data(), request->size())))) {
    close_descriptor(connection);
    return 1;
  }
  std::array<std::byte, std::size_t{16} * 1'024U> buffer{};
  while (true) {
    const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
    if (received > 0) {
      if (!write_all(STDOUT_FILENO, std::span(buffer).first(static_cast<std::size_t>(received)))) {
        close_descriptor(connection);
        return 1;
      }
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    close_descriptor(connection);
    return received == 0 ? 0 : 1;
  }
}

auto rename_session_status(const RuntimeEndpoint& endpoint, const std::string_view session,
                           const std::string_view new_name) -> OperationStatus {
  if (!valid_session_name(session) || !valid_session_name(new_name)) {
    return OperationStatus::failed;
  }
  try {
    std::string request = R"({"command":"session.rename")";
    if (!append_public_session_selector(request, session)) {
      return OperationStatus::failed;
    }
    request += R"(,"name":)";
    if (!api::append_json_string(request, new_name)) {
      return OperationStatus::failed;
    }
    request += "}";
    const auto result = invoke_public_command(endpoint, request);
    return result.has_value() ? public_operation_status(*result) : OperationStatus::failed;
  } catch (...) {
    return OperationStatus::failed;
  }
}

auto rename_session(const RuntimeEndpoint& endpoint, const std::string_view session,
                    const std::string_view new_name) -> int {
  if (!validate_session(session) || !validate_session(new_name)) {
    return 1;
  }
  return report_rename_status(rename_session_status(endpoint, session, new_name));
}

auto rename_tab_status(const RuntimeEndpoint& endpoint, const std::string_view session,
                       const std::size_t one_based_position, const std::string_view title)
    -> OperationStatus {
  if (!valid_session_name(session) || one_based_position == 0 ||
      one_based_position > protocol::tab_slots_max || !valid_tab_title(title)) {
    return OperationStatus::failed;
  }
  try {
    std::string request = R"({"command":"tab.rename")";
    if (!append_public_session_selector(request, session)) {
      return OperationStatus::failed;
    }
    request += R"(,"tab":{"position":)" + std::to_string(one_based_position) + R"(},"title":)";
    if (!api::append_json_string(request, title)) {
      return OperationStatus::failed;
    }
    request += "}";
    const auto result = invoke_public_command(endpoint, request);
    return result.has_value() ? public_operation_status(*result) : OperationStatus::failed;
  } catch (...) {
    return OperationStatus::failed;
  }
}

auto rename_tab(const RuntimeEndpoint& endpoint, const std::string_view session,
                const std::size_t one_based_position, const std::string_view title) -> int {
  if (!validate_session(session) || one_based_position == 0 ||
      one_based_position > protocol::tab_slots_max || !valid_tab_title(title)) {
    static_cast<void>(write_text(STDERR_FILENO, "invalid tab rename; position must be 1-16 and "
                                                "title must be 0-64 printable ASCII bytes\n"));
    return 1;
  }
  return report_rename_status(rename_tab_status(endpoint, session, one_based_position, title));
}

auto kill(const RuntimeEndpoint& endpoint, const std::string_view session) -> int {
  if (!validate_session(session)) {
    return 1;
  }
  const auto status = perform_command(endpoint, session, SemanticCommand::session_kill, {});
  if (status == OperationStatus::applied || status == OperationStatus::no_effect) {
    const auto message = "lemma session \"" + std::string(session) + "\" stopped\n";
    return write_text(STDOUT_FILENO, message) ? 0 : 1;
  }
  static_cast<void>(write_text(STDERR_FILENO, status == OperationStatus::missing
                                                  ? "no lemma session\n"
                                                  : "lemma operation failed\n"));
  return 1;
}

auto kill_all(const RuntimeEndpoint& endpoint) -> int {
  int connection = open_server_connection(endpoint);
  if (connection < 0) {
    return 0;
  }
  close_descriptor(connection);
  return run_control_command(endpoint, protocol::ControlCommand::kill_all, {}, false);
}

auto shutdown(const RuntimeEndpoint& endpoint) -> int { return run_shutdown_command(endpoint); }

} // namespace lemma::daemon
