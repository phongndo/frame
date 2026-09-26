#ifndef LEMMA_TESTS_SIM_TRACE_HPP
#define LEMMA_TESTS_SIM_TRACE_HPP

#include "lemma/id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <span>
#include <string_view>

namespace lemma::test::sim {

inline constexpr std::size_t trace_operations_max = 16'384;

enum class OperationKind : std::uint8_t {
  pane_split,
  pane_remove,
  pane_swap,
  pane_resize,
  pane_resize_divider,
  pane_invalidate_divider,
  pane_change_viewport,
  pane_probe_stale,
  tab_append,
  tab_erase,
  tab_place,
  tab_probe_stale,
  float_push,
  float_erase,
  float_raise,
  float_place,
};

struct Operation final {
  OperationKind kind{OperationKind::pane_probe_stale};
  PaneId pane;
  PaneId peer_pane;
  PaneId other_pane;
  TabId tab;
  TabId anchor_tab;
  std::uint16_t argument_0{0};
  std::uint16_t argument_1{0};
  std::int32_t result{0};
  std::uint64_t state_hash{0};
};

[[nodiscard]] constexpr auto operation_name(const OperationKind kind) noexcept -> std::string_view {
  switch (kind) {
  case OperationKind::pane_split:
    return "pane.split";
  case OperationKind::pane_remove:
    return "pane.remove";
  case OperationKind::pane_swap:
    return "pane.swap";
  case OperationKind::pane_resize:
    return "pane.resize";
  case OperationKind::pane_resize_divider:
    return "pane.resize-divider";
  case OperationKind::pane_invalidate_divider:
    return "pane.invalidate-divider";
  case OperationKind::pane_change_viewport:
    return "pane.viewport";
  case OperationKind::pane_probe_stale:
    return "pane.probe-stale";
  case OperationKind::tab_append:
    return "tab.append";
  case OperationKind::tab_erase:
    return "tab.erase";
  case OperationKind::tab_place:
    return "tab.place";
  case OperationKind::tab_probe_stale:
    return "tab.probe-stale";
  case OperationKind::float_push:
    return "float.push";
  case OperationKind::float_erase:
    return "float.erase";
  case OperationKind::float_raise:
    return "float.raise";
  case OperationKind::float_place:
    return "float.place";
  }
  return "unknown";
}

template <typename Id> void write_id(std::ostream& stream, const Id id) {
  if (id.is_valid()) {
    stream << id.slot() << ':' << id.generation();
  } else {
    stream << '-';
  }
}

inline auto operator<<(std::ostream& stream, const Operation& operation) -> std::ostream& {
  stream << operation_name(operation.kind) << " pane=";
  write_id(stream, operation.pane);
  stream << " peer=";
  write_id(stream, operation.peer_pane);
  stream << " other=";
  write_id(stream, operation.other_pane);
  stream << " tab=";
  write_id(stream, operation.tab);
  stream << " anchor=";
  write_id(stream, operation.anchor_tab);
  stream << " arg0=" << operation.argument_0 << " arg1=" << operation.argument_1
         << " result=" << operation.result << " hash=0x" << std::hex << std::setw(16)
         << std::setfill('0') << operation.state_hash << std::dec << std::setfill(' ');
  return stream;
}

class Trace final {
public:
  Trace(const std::uint64_t seed, const std::size_t requested_operations) noexcept
      : seed_(seed), requested_operations_(requested_operations) {}

  [[nodiscard]] auto append(const Operation operation) noexcept -> bool {
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
    stream << "replay: LEMMA_SIM_SEED=0x" << std::hex << seed_ << std::dec
           << " LEMMA_SIM_OPERATIONS=" << requested_operations_ << " ./test sim\n"
           << "seed=0x" << std::hex << seed_ << std::dec << " operations=" << requested_operations_
           << '\n';
    for (std::size_t index = 0; index < size_; ++index) {
      stream << index << ' ' << std::span(operations_).subspan(index, 1).front() << '\n';
    }
  }

private:
  std::array<Operation, trace_operations_max> operations_{};
  std::uint64_t seed_{0};
  std::size_t requested_operations_{0};
  std::size_t size_{0};
};

inline auto operator<<(std::ostream& stream, const Trace& trace) -> std::ostream& {
  trace.write(stream);
  return stream;
}

} // namespace lemma::test::sim

#endif // LEMMA_TESTS_SIM_TRACE_HPP
