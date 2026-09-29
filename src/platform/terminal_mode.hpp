#ifndef FRAME_PLATFORM_TERMINAL_MODE_HPP
#define FRAME_PLATFORM_TERMINAL_MODE_HPP

#include <chrono>
#include <cstdint>

#include <termios.h>
#include <unistd.h>

namespace frame::platform {

struct WindowSize final {
  std::uint16_t columns{80};
  std::uint16_t rows{24};
  std::uint16_t cell_width_px{8};
  std::uint16_t cell_height_px{16};
};

[[nodiscard]] auto terminal_size(int descriptor, std::uint16_t columns_max,
                                 std::uint16_t rows_max) noexcept -> WindowSize;

class RawTerminal final {
public:
  RawTerminal() = default;
  RawTerminal(const RawTerminal&) = delete;
  auto operator=(const RawTerminal&) -> RawTerminal& = delete;
  RawTerminal(RawTerminal&&) = delete;
  auto operator=(RawTerminal&&) -> RawTerminal& = delete;
  ~RawTerminal();

  [[nodiscard]] auto enter(int descriptor) noexcept -> bool;
  [[nodiscard]] auto restore() noexcept -> bool;
  [[nodiscard]] auto
  wait_for_emergency_restore(std::chrono::steady_clock::time_point deadline) noexcept -> bool;
  [[nodiscard]] auto restore_bounded() noexcept -> bool;
  [[nodiscard]] auto restore_bounded(std::chrono::steady_clock::time_point deadline) noexcept
      -> bool;
  [[nodiscard]] auto restore_bounded_after_output_abort(
      int output_descriptor, std::chrono::steady_clock::time_point deadline) noexcept -> bool;
  [[nodiscard]] auto restore_wakeup_descriptor() const noexcept -> int {
    return restore_wakeup_descriptor_;
  }

private:
  [[nodiscard]] auto start_emergency_restorer(const char* terminal_path) noexcept -> bool;
  [[nodiscard]] auto consume_emergency_restore() noexcept -> bool;
  void stop_emergency_restorer() noexcept;

  termios original_{};
  int entry_descriptor_{-1};
  int restore_descriptor_{-1};
  int restore_wakeup_descriptor_{-1};
  int restore_result_descriptor_{-1};
  decltype(::getpid()) restore_process_{-1};
  bool active_{false};
};

} // namespace frame::platform

#endif // FRAME_PLATFORM_TERMINAL_MODE_HPP
