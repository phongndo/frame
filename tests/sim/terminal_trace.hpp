#ifndef FRAME_TESTS_SIM_TERMINAL_TRACE_HPP
#define FRAME_TESTS_SIM_TERMINAL_TRACE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <span>
#include <string_view>

namespace frame::test::sim {

inline constexpr std::size_t terminal_trace_operations_max = 16'384;

enum class TerminalOperationKind : std::uint8_t {
  write,
  resize,
  compose,
  encode_key,
  encode_paste,
  encode_focus,
  encode_mouse,
  drain_pty,
  set_theme,
  scroll_viewport,
  select,
  invalidate_render,
  count,
};

struct TerminalOperation final {
  TerminalOperationKind kind{TerminalOperationKind::write};
  std::uint16_t argument_0{0};
  std::uint16_t argument_1{0};
  std::uint16_t argument_2{0};
  std::int32_t result{0};
  std::uint64_t state_hash{0};
};

[[nodiscard]] constexpr auto terminal_operation_name(const TerminalOperationKind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case TerminalOperationKind::write:
    return "terminal.write";
  case TerminalOperationKind::resize:
    return "terminal.resize";
  case TerminalOperationKind::compose:
    return "terminal.compose";
  case TerminalOperationKind::encode_key:
    return "terminal.encode-key";
  case TerminalOperationKind::encode_paste:
    return "terminal.encode-paste";
  case TerminalOperationKind::encode_focus:
    return "terminal.encode-focus";
  case TerminalOperationKind::encode_mouse:
    return "terminal.encode-mouse";
  case TerminalOperationKind::drain_pty:
    return "terminal.drain-pty";
  case TerminalOperationKind::set_theme:
    return "terminal.set-theme";
  case TerminalOperationKind::scroll_viewport:
    return "terminal.scroll-viewport";
  case TerminalOperationKind::select:
    return "terminal.select";
  case TerminalOperationKind::invalidate_render:
    return "terminal.invalidate-render";
  case TerminalOperationKind::count:
    break;
  }
  return "terminal.unknown";
}

inline auto operator<<(std::ostream& stream, const TerminalOperation& operation) -> std::ostream& {
  stream << terminal_operation_name(operation.kind) << " arg0=" << operation.argument_0
         << " arg1=" << operation.argument_1 << " arg2=" << operation.argument_2
         << " result=" << operation.result << " hash=0x" << std::hex << std::setw(16)
         << std::setfill('0') << operation.state_hash << std::dec << std::setfill(' ');
  return stream;
}

class TerminalTrace final {
public:
  TerminalTrace(const std::uint64_t seed, const std::size_t requested_operations) noexcept
      : seed_(seed), requested_operations_(requested_operations) {}

  [[nodiscard]] auto append(const TerminalOperation operation) noexcept -> bool {
    if (size_ == operations_.size()) {
      return false;
    }
    std::span(operations_).subspan(size_, 1).front() = operation;
    ++size_;
    return true;
  }

  void complete_last(const std::uint64_t state_hash) noexcept {
    if (size_ > 0) {
      std::span(operations_).subspan(size_ - 1U, 1).front().state_hash = state_hash;
    }
  }

  void write(std::ostream& stream) const {
    stream << "replay: FRAME_SIM_SEED=0x" << std::hex << seed_ << std::dec
           << " FRAME_SIM_OPERATIONS=" << requested_operations_ << " ./test sim\n"
           << "seed=0x" << std::hex << seed_ << std::dec << " operations=" << requested_operations_
           << '\n';
    for (std::size_t index = 0; index < size_; ++index) {
      stream << index << ' ' << std::span(operations_).subspan(index, 1).front() << '\n';
    }
  }

private:
  std::array<TerminalOperation, terminal_trace_operations_max> operations_{};
  std::uint64_t seed_{0};
  std::size_t requested_operations_{0};
  std::size_t size_{0};
};

inline auto operator<<(std::ostream& stream, const TerminalTrace& trace) -> std::ostream& {
  trace.write(stream);
  return stream;
}

} // namespace frame::test::sim

#endif // FRAME_TESTS_SIM_TERMINAL_TRACE_HPP
