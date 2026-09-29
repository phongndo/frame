#include "reduce.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace frame::test::sim {
namespace {

struct ReducerOperation final {
  std::uint16_t kind{0};
  std::uint16_t value{0};

  [[nodiscard]] constexpr auto operator==(const ReducerOperation&) const noexcept -> bool = default;
};

[[nodiscard]] auto contains_failure(const std::span<const ReducerOperation> operations) noexcept
    -> bool {
  for (std::size_t index = 0; index + 2U < operations.size(); ++index) {
    const auto candidate = operations.subspan(index, 3);
    if (candidate.front().kind == 7 && candidate.subspan(1, 1).front().kind == 8 &&
        candidate.subspan(2, 1).front().kind == 9 &&
        std::ranges::all_of(candidate,
                            [](const ReducerOperation operation) { return operation.value > 0; })) {
      return true;
    }
  }
  return false;
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TraceReductionTest, RemovesIrrelevantOperationsAndShrinksValuesDeterministically) {
  constexpr std::array input{
      ReducerOperation{.kind = 1, .value = 500}, ReducerOperation{.kind = 2, .value = 400},
      ReducerOperation{.kind = 7, .value = 91},  ReducerOperation{.kind = 8, .value = 82},
      ReducerOperation{.kind = 9, .value = 73},  ReducerOperation{.kind = 3, .value = 300},
      ReducerOperation{.kind = 4, .value = 200},
  };
  const auto shrink = [](const ReducerOperation operation,
                         const std::size_t attempt) -> std::optional<ReducerOperation> {
    if (attempt == 0 && operation.value != 1) {
      auto candidate = operation;
      candidate.value = 1;
      return candidate;
    }
    return std::nullopt;
  };

  const auto first = reduce_trace<ReducerOperation>(input, &contains_failure, shrink);
  const auto second = reduce_trace<ReducerOperation>(input, &contains_failure, shrink);

  constexpr std::array expected{
      ReducerOperation{.kind = 7, .value = 1},
      ReducerOperation{.kind = 8, .value = 1},
      ReducerOperation{.kind = 9, .value = 1},
  };
  EXPECT_EQ(first.operations, std::vector(expected.begin(), expected.end()));
  EXPECT_EQ(first.operations, second.operations);
  EXPECT_EQ(first.stats.evaluations, second.stats.evaluations);
  EXPECT_EQ(first.stats.original_operations, input.size());
  EXPECT_EQ(first.stats.reduced_operations, expected.size());
  EXPECT_FALSE(first.stats.limit_reached);
}

TEST(TraceReductionTest, StopsAtTheConfiguredEvaluationBound) {
  constexpr std::array input{
      ReducerOperation{.kind = 1},
      ReducerOperation{.kind = 2},
      ReducerOperation{.kind = 3},
      ReducerOperation{.kind = 4},
  };
  const auto always_fails = [](const std::span<const ReducerOperation>) { return true; };

  const auto reduced = reduce_trace<ReducerOperation>(input, always_fails, 1);

  EXPECT_EQ(reduced.stats.evaluations, 1U);
  EXPECT_TRUE(reduced.stats.limit_reached);
  EXPECT_LE(reduced.operations.size(), input.size());
}

} // namespace
} // namespace frame::test::sim
