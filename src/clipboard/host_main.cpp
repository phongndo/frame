#include "frame/limits.hpp"
#include "image/png.hpp"
#include "platform/io.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <new>
#include <span>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {
std::atomic<const char*> pending_path{nullptr};
static_assert(decltype(pending_path)::is_always_lock_free);
void cancel_save(int /*signal*/) noexcept {
  if (const auto* path = pending_path.load(std::memory_order_acquire); path != nullptr) {
    static_cast<void>(::unlink(path));
  }
  ::_exit(1);
}
struct SignalBlock final {
  explicit SignalBlock(const sigset_t& signals) noexcept
      : active(::sigprocmask(SIG_BLOCK, &signals, &previous) == 0) {}
  SignalBlock(const SignalBlock&) = delete;
  auto operator=(const SignalBlock&) -> SignalBlock& = delete;
  SignalBlock(SignalBlock&&) = delete;
  auto operator=(SignalBlock&&) -> SignalBlock& = delete;
  ~SignalBlock() {
    if (active) {
      static_cast<void>(::sigprocmask(SIG_SETMASK, &previous, nullptr));
    }
  }
  auto unblock() noexcept -> bool {
    if (!active || ::sigprocmask(SIG_SETMASK, &previous, nullptr) != 0) {
      return false;
    }
    active = false;
    return true;
  }
  sigset_t previous{};
  bool active{false};
};
auto allocate(void* const context, const std::size_t size) noexcept -> std::span<std::byte> {
  auto& pixels = *static_cast<std::vector<std::byte>*>(context);
  try {
    pixels.resize(size);
    return pixels;
  } catch (const std::bad_alloc&) {
    return {};
  } // Reject the bounded decode before creating a file.
}
struct TemporaryFile final {
  TemporaryFile(int fd, const char* filename) noexcept : descriptor(fd), path(filename) {
    pending_path.store(filename, std::memory_order_release);
  }
  TemporaryFile(const TemporaryFile&) = delete;
  auto operator=(const TemporaryFile&) -> TemporaryFile& = delete;
  TemporaryFile(TemporaryFile&&) = delete;
  auto operator=(TemporaryFile&&) -> TemporaryFile& = delete;
  int descriptor{-1};
  const char* path{nullptr};
  ~TemporaryFile() {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
    }
    if (path != nullptr) {
      static_cast<void>(::unlink(path));
      pending_path.store(nullptr, std::memory_order_release);
    }
  }
};
// The worker's single transaction owns validation, file publication, and cleanup.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto run() -> int {
  constexpr int channel = 3;
  sigset_t cancellations{};
  static_cast<void>(sigemptyset(&cancellations));
  constexpr std::array signals{SIGALRM, SIGTERM, SIGHUP, SIGINT, SIGXFSZ, SIGPIPE};
  for (const auto signal : signals) {
    // Every signal is a platform constant; Darwin implements this infallibly as a macro.
    static_cast<void>(sigaddset(&cancellations, signal));
  }
  struct sigaction action{};
  action.sa_handler = cancel_save;
  action.sa_mask = cancellations;
  for (const auto signal : signals) {
    if (::sigaction(signal, &action, nullptr) != 0) {
      return 1;
    }
  }
  static_cast<void>(::alarm(10));
  // POSIX descriptor flags have a variadic ABI.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  if (::fcntl(channel, F_SETFD, FD_CLOEXEC) != 0) {
    return 1;
  }
  std::vector<std::byte> input;
  std::array<std::byte, std::size_t{16} * 1024U> chunk{};
  while (true) {
    const auto count = ::read(channel, chunk.data(), chunk.size());
    if (count == 0) {
      break;
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 1;
    }
    const auto size = static_cast<std::size_t>(count);
    if (size > frame::limits::clipboard_decoded_bytes_max - input.size()) {
      return 1;
    }
    const auto received = std::span(chunk).first(size);
    input.insert(input.end(), received.begin(), received.end());
  }
  std::vector<std::byte> pixels;
  if (!frame::image::decode_png(input, allocate, &pixels)) {
    return 1;
  }
  const auto* cache = std::getenv("XDG_CACHE_HOME");
  const auto* home = std::getenv("HOME");
  std::filesystem::path root;
  if (cache != nullptr && *cache != '\0') {
    root = cache;
  } else if (home != nullptr && *home != '\0') {
    root = std::filesystem::path(home) / ".cache";
  } else {
    return 1;
  }
  if (!root.is_absolute()) {
    return 1;
  }
  std::error_code error;
  root /= "frame";
  std::filesystem::create_directories(root, error);
  if (error) {
    return 1;
  }
  root /= "clipboard";
  if (::mkdir(root.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
    return 1;
  }
  struct stat info{};
  if (::lstat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() ||
      (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return 1;
  }
  std::string temporary = (root / "image-XXXXXX").string();
  std::string path; // Both path buffers outlive the cleanup boundary, including allocation failure.
  // Publish cleanup ownership without a signal window between creation and registration.
  SignalBlock creation(cancellations);
  if (!creation.active) {
    return 1;
  }
  const int file = ::mkstemp(temporary.data());
  if (file < 0) {
    return 1;
  }
  TemporaryFile owned(file, temporary.c_str());
  if (!creation.unblock()) {
    return 1;
  }
  path = temporary + ".png";
  const std::string response = std::string(1, '\1') + path + '\0';
  if (response.size() > 4096U || !frame::platform::write_all(file, input)) {
    return 1;
  }
  const bool closed = ::close(file) == 0;
  owned.descriptor = -1;
  SignalBlock rename(cancellations);
  if (!closed || !rename.active || ::rename(temporary.c_str(), path.c_str()) != 0) {
    return 1;
  }
  owned.path = path.c_str();
  pending_path.store(owned.path, std::memory_order_release);
  if (!rename.unblock()) {
    return 1;
  }
  // Nonblocking, terminated publication cannot expose a truncated path as success. Once the
  // complete record is sent, cancellation must not remove the published file.
  SignalBlock publication(cancellations);
  if (!publication.active || !frame::platform::set_nonblocking(channel) ||
      !frame::platform::send_text(channel, response)) {
    return 1;
  }
  pending_path.store(nullptr, std::memory_order_release);
  owned.path = nullptr;
  return 0;
}
} // namespace
int main() {
  try {
    return run();
  } catch (const std::bad_alloc&) {
    return 1;
  } // Private worker boundary: EOF reports failure, not partial success.
  catch (const std::filesystem::filesystem_error&) {
    return 1;
  }
}
