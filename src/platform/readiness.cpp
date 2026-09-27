#include "platform/readiness.hpp"

#include <cstddef>
#include <span>

#ifdef __linux__
#include "platform/io.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>
#endif

namespace lemma::platform {

struct Readiness::Impl final {
#ifdef __linux__
  struct Watch final {
    ReadinessIdentity identity;
    std::size_t index;
    short events;
    bool visited{false};
  };

  explicit Impl(const std::size_t limit) : capacity(limit) {}
  ~Impl() { close_descriptor(descriptor); }
  Impl(const Impl&) = delete;
  auto operator=(const Impl&) -> Impl& = delete;
  Impl(Impl&&) = delete;
  auto operator=(Impl&&) -> Impl& = delete;

  void reset() noexcept {
    close_descriptor(descriptor);
    watches.clear();
  }

  [[nodiscard]] auto register_watch(const int operation, const int fd,
                                    const short events) const noexcept -> bool {
    epoll_event event{};
    if ((events & POLLIN) != 0) {
      event.events |= EPOLLIN;
    }
    if ((events & POLLOUT) != 0) {
      event.events |= EPOLLOUT;
    }
    if ((events & POLLPRI) != 0) {
      event.events |= EPOLLPRI;
    }
    event.data.fd = fd;
    return ::epoll_ctl(descriptor, operation, fd, &event) == 0;
  }

  [[nodiscard]] auto rebuild(const std::span<const pollfd> descriptors,
                             const std::span<const ReadinessIdentity> identities) -> bool {
    // Rebuild on ownership changes, not just numeric-fd changes. Closing the old epoll instance
    // also removes registrations whose old open file survives in a dup or a forked child.
    reset();
    ready.resize(std::max(std::size_t{1}, descriptors.size()));
    descriptor = ::epoll_create1(EPOLL_CLOEXEC);
    if (descriptor < 0) {
      return false;
    }
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      const auto& entry = descriptors.subspan(index, 1).front();
      if (entry.fd < 0) {
        continue;
      }
      const auto inserted =
          watches.emplace(entry.fd, Watch{.identity = identities.subspan(index, 1).front(),
                                          .index = index,
                                          .events = entry.events});
      if (!inserted.second || !register_watch(EPOLL_CTL_ADD, entry.fd, entry.events)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] auto synchronize(const std::span<const pollfd> descriptors,
                                 const std::span<const ReadinessIdentity> identities) -> bool {
    const auto count = static_cast<std::size_t>(
        std::ranges::count_if(descriptors, [](const pollfd& entry) { return entry.fd >= 0; }));
    if (descriptor < 0 || count != watches.size()) {
      return rebuild(descriptors, identities);
    }
    for (auto& [fd, watch] : watches) {
      watch.visited = false;
    }
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      const auto& entry = descriptors.subspan(index, 1).front();
      if (entry.fd < 0) {
        continue;
      }
      const auto found = watches.find(entry.fd);
      if (found == watches.end() ||
          found->second.identity != identities.subspan(index, 1).front()) {
        return rebuild(descriptors, identities);
      }
      auto& watch = found->second;
      if (watch.visited) {
        return false;
      }
      watch.visited = true;
      watch.index = index;
      if (watch.events != entry.events && !register_watch(EPOLL_CTL_MOD, entry.fd, entry.events)) {
        return false;
      }
      watch.events = entry.events;
    }
    return true;
  }

  [[nodiscard]] static auto poll_events(const std::uint32_t ready_events) noexcept -> short {
    short events = 0;
    if ((ready_events & EPOLLIN) != 0) {
      events |= POLLIN;
    }
    if ((ready_events & EPOLLOUT) != 0) {
      events |= POLLOUT;
    }
    if ((ready_events & EPOLLPRI) != 0) {
      events |= POLLPRI;
    }
    if ((ready_events & EPOLLERR) != 0) {
      events |= POLLERR;
    }
    if ((ready_events & EPOLLHUP) != 0) {
      events |= POLLHUP;
    }
    return events;
  }

  [[nodiscard]] auto wait(const std::span<pollfd> descriptors,
                          const int timeout_milliseconds) noexcept -> int {
    for (auto& entry : descriptors) {
      entry.revents = 0;
    }
    const auto count = ::epoll_wait(descriptor, ready.data(), static_cast<int>(ready.size()),
                                    timeout_milliseconds);
    if (count <= 0) {
      return count;
    }
    int reported = 0;
    for (const auto& event : std::span(ready).first(static_cast<std::size_t>(count))) {
      const int fd = event.data.fd;
      const auto found = watches.find(fd);
      if (found == watches.end()) {
        continue;
      }
      auto& entry = descriptors.subspan(found->second.index, 1).front();
      const auto events = poll_events(event.events);
      entry.revents = events;
      reported += events != 0 ? 1 : 0;
    }
    return reported;
  }

  std::size_t capacity;
  int descriptor{-1};
  std::unordered_map<int, Watch> watches;
  std::vector<epoll_event> ready;
#endif
};

Readiness::Readiness([[maybe_unused]] const std::size_t capacity) noexcept {
#ifdef __linux__
  if (capacity > 0 && capacity <= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    try {
      impl_ = std::make_unique<Impl>(capacity);
    } catch (...) {
      // Readiness still works through poll if optional retained registration cannot be allocated.
      return;
    }
  }
#endif
}

Readiness::~Readiness() = default;

auto Readiness::uses_native_wait() const noexcept -> bool { return impl_ != nullptr; }

// Only Linux retains native registration state; elsewhere this is a plain poll.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
auto Readiness::wait(const std::span<pollfd> descriptors,
                     [[maybe_unused]] const std::span<const ReadinessIdentity> identities,
                     const int timeout_milliseconds) noexcept -> int {
#ifdef __linux__
  if (impl_ != nullptr && descriptors.size() <= impl_->capacity &&
      identities.size() == descriptors.size()) {
    constexpr short supported = POLLIN | POLLOUT | POLLPRI;
    const bool reusable =
        std::ranges::all_of(identities,
                            [](const auto& identity) { return identity.domain != 0; }) &&
        std::ranges::all_of(descriptors, [](const auto& entry) {
          return entry.fd < 0 || (entry.events & ~supported) == 0;
        });
    if (reusable) {
      try {
        if (impl_->synchronize(descriptors, identities)) {
          return impl_->wait(descriptors, timeout_milliseconds);
        }
      } catch (...) {
        impl_.reset();
      }
      // Unsupported descriptors (including duplicates and regular files) and registration
      // failures preserve poll semantics without retaining a partial registration set.
      impl_.reset();
    } else {
      impl_->reset();
    }
  } else if (impl_ != nullptr) {
    impl_->reset();
  }
#endif
  return ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), timeout_milliseconds);
}

} // namespace lemma::platform
