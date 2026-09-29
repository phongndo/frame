#ifndef FRAME_TESTS_SIM_RANDOM_HPP
#define FRAME_TESTS_SIM_RANDOM_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace frame::test::sim {

// SplitMix64 gives the simulator one small, stable random stream. Its output is part of the replay
// contract: changing this algorithm intentionally invalidates seed-only replay across commits.
class Random final {
public:
  explicit constexpr Random(const std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] constexpr auto next() noexcept -> std::uint64_t {
    state_ += 0x9E3779B97F4A7C15ULL;
    auto value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] constexpr auto index(const std::size_t bound) noexcept -> std::size_t {
    assert(bound > 0);
    return static_cast<std::size_t>(next() % bound);
  }

  [[nodiscard]] constexpr auto boolean() noexcept -> bool { return (next() & 1U) != 0; }

  [[nodiscard]] constexpr auto between(const std::uint16_t minimum,
                                       const std::uint16_t maximum) noexcept -> std::uint16_t {
    assert(minimum <= maximum);
    const auto width = static_cast<std::uint64_t>(maximum) - minimum + 1U;
    return static_cast<std::uint16_t>(minimum + (next() % width));
  }

private:
  std::uint64_t state_;
};

} // namespace frame::test::sim

#endif // FRAME_TESTS_SIM_RANDOM_HPP
