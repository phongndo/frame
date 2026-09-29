#ifndef FRAME_PLATFORM_READINESS_HPP
#define FRAME_PLATFORM_READINESS_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <poll.h>

namespace frame::platform {

// Equal identities mean the same open-file lifetime, even if the descriptor list is reordered.
// Use existing owner generations, not an fd number. A zero domain opts out of retained watches.
struct ReadinessIdentity final {
  std::uint64_t domain{0};
  std::uint64_t owner{0};
  std::uint64_t generation{0};

  friend constexpr auto operator==(const ReadinessIdentity&, const ReadinessIdentity&) noexcept
      -> bool = default;
};

// Borrowed descriptors; owned kernel registration. Linux retains level-triggered epoll watches
// while membership/lifetimes are unchanged. Other platforms and unsupported inputs use poll.
// Returns poll-compatible revents/count/errno, including EINTR. No steady-state allocations.
class Readiness final {
public:
  explicit Readiness(std::size_t capacity) noexcept;
  ~Readiness();
  Readiness(const Readiness&) = delete;
  auto operator=(const Readiness&) -> Readiness& = delete;
  Readiness(Readiness&&) = delete;
  auto operator=(Readiness&&) -> Readiness& = delete;

  [[nodiscard]] auto uses_native_wait() const noexcept -> bool;

  [[nodiscard]] auto wait(std::span<pollfd> descriptors,
                          std::span<const ReadinessIdentity> identities,
                          int timeout_milliseconds) noexcept -> int;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace frame::platform

#endif // FRAME_PLATFORM_READINESS_HPP
