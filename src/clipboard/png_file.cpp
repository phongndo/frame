#include "clipboard/png_file.hpp"
#include "lemma/limits.hpp"
#include "platform/io.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal> // IWYU pragma: keep
#include <cstddef>
#include <fcntl.h>
#include <memory>
#include <new>
#include <span>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ; // NOLINT(readability-redundant-declaration)
#endif

namespace lemma::clipboard {
namespace {
auto spawn(const std::string& helper, const int child, const int parent) noexcept -> bool {
  posix_spawn_file_actions_t actions{};
  posix_spawnattr_t attributes{};
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    return false;
  }
  if (::posix_spawnattr_init(&attributes) != 0) {
    ::posix_spawn_file_actions_destroy(&actions);
    return false;
  }
  sigset_t defaults{};
  sigset_t mask{};
  sigfillset(&defaults);
  sigemptyset(&mask);
  const short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK
#ifdef __APPLE__
                      | POSIX_SPAWN_CLOEXEC_DEFAULT
#endif
      ;
#ifdef __APPLE__
  auto** environment = *_NSGetEnviron();
#else
  auto** environment = ::environ;
#endif
  // posix_spawn does not modify argv. The API's historical signature is non-const.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  std::array<char*, 2> arguments{const_cast<char*>(helper.c_str()), nullptr};
  decltype(::getpid()) process = -1;
  const bool started =
      ::posix_spawn_file_actions_adddup2(&actions, child, 3) == 0 &&
      (parent == 3 || ::posix_spawn_file_actions_addclose(&actions, parent) == 0) &&
      (child == 3 || ::posix_spawn_file_actions_addclose(&actions, child) == 0) &&
#ifndef __APPLE__
      ::posix_spawn_file_actions_addclosefrom_np(&actions, 4) == 0 &&
#endif
      ::posix_spawnattr_setpgroup(&attributes, 0) == 0 &&
      ::posix_spawnattr_setsigdefault(&attributes, &defaults) == 0 &&
      ::posix_spawnattr_setsigmask(&attributes, &mask) == 0 &&
      ::posix_spawnattr_setflags(&attributes, flags) == 0 &&
      ::posix_spawn(&process, helper.c_str(), &actions, &attributes, arguments.data(),
                    environment) == 0;
  ::posix_spawn_file_actions_destroy(&actions);
  ::posix_spawnattr_destroy(&attributes);
  return started;
}
} // namespace

PngFile::~PngFile() { platform::close_descriptor(channel_); }
auto PngFile::start(const std::span<const std::byte> png, const Clock::time_point now) noexcept
    -> std::unique_ptr<PngFile> {
  if (png.empty() || png.size() > limits::clipboard_decoded_bytes_max) {
    return nullptr;
  }
  try {
    std::array<char, 4096> executable{};
    const auto length = platform::executable_path(executable);
    if (length == 0) {
      return nullptr;
    }
    std::string helper(executable.data(), length);
    const auto slash = helper.rfind('/');
    if (slash == std::string::npos) {
      return nullptr;
    }
    helper.resize(slash + 1U);
    helper += "lemma-clipboard-host";
    auto result = std::make_unique<PngFile>();
    result->png_.assign(png.begin(), png.end());
    result->response_.reserve(4096);
    result->expires_ = now + std::chrono::seconds(10);
    result->next_ = now;
    std::array<int, 2> sockets{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0) {
      return nullptr;
    }
    result->channel_ = sockets.front();
#ifdef __APPLE__
    const int enabled = 1;
    if (::setsockopt(result->channel_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
      platform::close_descriptor(sockets.back());
      return nullptr;
    }
#endif
    // POSIX descriptor flags have a variadic ABI.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
    const bool started = ::fcntl(sockets.front(), F_SETFD, FD_CLOEXEC) == 0 &&
                         ::fcntl(sockets.back(), F_SETFD, FD_CLOEXEC) == 0 &&
                         platform::set_nonblocking(result->channel_) &&
                         spawn(helper, sockets.back(), sockets.front());
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    platform::close_descriptor(sockets.back());
    return started ? std::move(result) : nullptr;
  } catch (const std::bad_alloc&) {
    // The unpublished file job owns all prepared descriptors and allocations.
    return nullptr;
  }
}

// One bounded worker-transport step owns send, completion, and deadline transitions.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void PngFile::advance(const Clock::time_point now) noexcept {
  if (done_ || now < next_) {
    return;
  }
  const auto finish = [this]() {
    done_ = true;
    platform::close_descriptor(channel_);
    png_.clear();
  };
  if (now >= expires_) {
    finish();
    return;
  }
  next_ = now + std::chrono::milliseconds(10);
  if (sent_ < png_.size()) {
    const auto bytes =
        std::span(png_).subspan(sent_, std::min(std::size_t{16} * 1024U, png_.size() - sent_));
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count = ::send(channel_, bytes.data(), bytes.size(), flags);
    if (count > 0) {
      sent_ += static_cast<std::size_t>(count);
      if (sent_ == png_.size()) {
        static_cast<void>(::shutdown(channel_, SHUT_WR));
      }
    } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      finish();
      return;
    }
  }
  std::array<char, 4096> buffer{};
  const auto count = ::read(channel_, buffer.data(), buffer.size());
  if (count > 0) {
    const auto size = static_cast<std::size_t>(count);
    if (size > 4096U - response_.size()) {
      finish();
      return;
    }
    response_.append(buffer.data(), size); // reserve() above bounds all response storage.
  } else if (count == 0) {
    if (response_.size() > 3U && response_.starts_with("\1/") && response_.back() == '\0' &&
        std::ranges::none_of(
            std::string_view(response_).substr(1, response_.size() - 2U),
            [](const unsigned char value) { return value < 32U || value == 127U; })) {
      response_.pop_back();
      response_.erase(0, 1);
      path_ = std::move(response_);
    }
    finish();
  } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    finish();
  }
}
} // namespace lemma::clipboard
