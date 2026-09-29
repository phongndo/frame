#ifndef FRAME_CLIPBOARD_PNG_FILE_HPP
#define FRAME_CLIPBOARD_PNG_FILE_HPP
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frame::clipboard {
// Filesystem access and PNG validation run in a bounded, short-lived helper, never in the reactor.
// Cancellation closes the channel. The helper has its own process deadline; the daemon's normal
// child reaper owns waitpid, so no stale PID can be signalled after another owner reaps it.
class PngFile final {
public:
  using Clock = std::chrono::steady_clock;
  [[nodiscard]] static auto start(std::span<const std::byte> png, Clock::time_point now) noexcept
      -> std::unique_ptr<PngFile>;
  PngFile() = default;
  ~PngFile();
  PngFile(const PngFile&) = delete;
  auto operator=(const PngFile&) -> PngFile& = delete;
  PngFile(PngFile&&) = delete;
  auto operator=(PngFile&&) -> PngFile& = delete;
  void advance(Clock::time_point now) noexcept;
  [[nodiscard]] auto done() const noexcept -> bool { return done_; }
  [[nodiscard]] auto path() const noexcept -> std::string_view { return path_; }
  [[nodiscard]] auto deadline() const noexcept -> Clock::time_point { return next_; }

private:
  int channel_{-1};
  std::vector<std::byte> png_;
  std::string response_;
  std::string path_;
  std::size_t sent_{0};
  Clock::time_point expires_;
  Clock::time_point next_;
  bool done_{false};
};
} // namespace frame::clipboard
#endif
