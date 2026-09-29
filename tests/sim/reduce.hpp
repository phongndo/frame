#ifndef FRAME_TESTS_SIM_REDUCE_HPP
#define FRAME_TESTS_SIM_REDUCE_HPP

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace frame::test::sim {

inline constexpr std::size_t trace_reduction_evaluations_max = 16'384;

struct TraceReductionStats final {
  std::size_t original_operations{0};
  std::size_t reduced_operations{0};
  std::size_t evaluations{0};
  bool limit_reached{false};
};

template <typename Operation> struct TraceReduction final {
  std::vector<Operation> operations;
  TraceReductionStats stats;
};

// Deterministic bounded delta debugging. fails(candidate) must return true only when the original
// failure is reproduced. shrink(operation, attempt) returns progressively simpler values and
// std::nullopt after its finite candidate set is exhausted.
template <typename Operation, typename FailurePredicate, typename ValueShrinker>
  requires std::invocable<ValueShrinker&, const Operation&, std::size_t>
// Delta debugging is inherently a bounded nested search over chunks and value candidates.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto reduce_trace(const std::span<const Operation> original, FailurePredicate fails,
                                ValueShrinker shrink,
                                const std::size_t evaluations_max = trace_reduction_evaluations_max)
    -> TraceReduction<Operation> {
  TraceReduction<Operation> result;
  result.operations.assign(original.begin(), original.end());
  result.stats.original_operations = original.size();
  if (result.operations.empty() || evaluations_max == 0) {
    result.stats.reduced_operations = result.operations.size();
    result.stats.limit_reached = evaluations_max == 0;
    return result;
  }

  const auto reproduces = [&result, evaluations_max, &fails](const auto& candidate) {
    if (result.stats.evaluations == evaluations_max) {
      result.stats.limit_reached = true;
      return false;
    }
    ++result.stats.evaluations;
    return fails(std::span<const Operation>(candidate));
  };

  std::size_t granularity = 2;
  while (result.operations.size() >= 2 && !result.stats.limit_reached) {
    const auto chunk = (result.operations.size() + granularity - 1U) / granularity;
    bool removed = false;
    for (std::size_t begin = 0; begin < result.operations.size() && !result.stats.limit_reached;
         begin += chunk) {
      const auto end = std::min(begin + chunk, result.operations.size());
      std::vector<Operation> candidate;
      candidate.reserve(result.operations.size() - (end - begin));
      candidate.insert(candidate.end(), result.operations.begin(),
                       result.operations.begin() + static_cast<std::ptrdiff_t>(begin));
      candidate.insert(candidate.end(),
                       result.operations.begin() + static_cast<std::ptrdiff_t>(end),
                       result.operations.end());
      if (!candidate.empty() && reproduces(candidate)) {
        result.operations = std::move(candidate);
        granularity = std::max(std::size_t{2}, granularity - 1U);
        removed = true;
        break;
      }
    }
    if (removed) {
      continue;
    }
    if (granularity >= result.operations.size()) {
      break;
    }
    granularity = std::min(result.operations.size(), granularity * 2U);
  }

  for (std::size_t index = 0; index < result.operations.size() && !result.stats.limit_reached;
       ++index) {
    std::size_t attempt = 0;
    while (!result.stats.limit_reached) {
      const auto simplified = shrink(result.operations.at(index), attempt);
      ++attempt;
      if (!simplified.has_value()) {
        break;
      }
      auto candidate = result.operations;
      candidate.at(index) = *simplified;
      if (reproduces(candidate)) {
        result.operations = std::move(candidate);
        attempt = 0;
      }
    }
  }

  result.stats.reduced_operations = result.operations.size();
  return result;
}

template <typename Operation, typename FailurePredicate>
[[nodiscard]] auto reduce_trace(const std::span<const Operation> original, FailurePredicate fails,
                                const std::size_t evaluations_max = trace_reduction_evaluations_max)
    -> TraceReduction<Operation> {
  const auto no_value_shrinks = [](const Operation&, const std::size_t) {
    return std::optional<Operation>{};
  };
  return reduce_trace(original, std::move(fails), no_value_shrinks, evaluations_max);
}

} // namespace frame::test::sim

#endif // FRAME_TESTS_SIM_REDUCE_HPP
