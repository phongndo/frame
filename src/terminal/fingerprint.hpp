#ifndef LEMMA_TERMINAL_FINGERPRINT_HPP
#define LEMMA_TERMINAL_FINGERPRINT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lemma::vt::detail {

using FingerprintLanes = std::array<std::uint64_t, 4>;

// Presentation fingerprints decide that a physical row or cell needs no bytes, so a collision
// leaves the outer terminal showing other content than the pane. Children control that content.
// With public constants, colliding rows could be searched offline and would collide in every
// process; each process therefore keys every fingerprint with secret random values.
struct FingerprintKey final {
  std::uint64_t state{0};
  std::uint64_t value{0};
  // Seed of style, cell, and decoded-row chains.
  std::uint64_t initial{0};
  // Seeds of the plain-row lanes, which fold raw cell values instead of cell fingerprints.
  FingerprintLanes plain_lanes{};

  // An operand equal to its secret annihilates the product and discards the other operand, so no
  // seed may equal a secret.
  [[nodiscard]] constexpr auto valid() const noexcept -> bool {
    const auto seed_valid = [this](const std::uint64_t seed) noexcept {
      return seed != state && seed != value;
    };
    bool valid = state != value && seed_valid(initial);
    for (const auto seed : plain_lanes) {
      valid = valid && seed_valid(seed);
    }
    return valid;
  }
};

// Drawn once per process from the operating system's entropy source; null if unavailable, in which
// case terminals refuse to render rather than trust predictable fingerprints.
[[nodiscard]] auto process_fingerprint_key() noexcept -> const FingerprintKey*;

// One 64x64->128 multiply per word (wyhash's folded multiply). Fingerprints are compared only with
// retained fingerprints under the same key.
[[nodiscard]] constexpr auto fingerprint_mix(const FingerprintKey& key, const std::uint64_t hash,
                                             const std::uint64_t word) noexcept -> std::uint64_t {
  using Product = unsigned __int128;
  const auto product =
      static_cast<Product>(hash ^ key.state) * static_cast<Product>(word ^ key.value);
  return static_cast<std::uint64_t>(product) ^ static_cast<std::uint64_t>(product >> 64U);
}

// Folds one row of words. Four independent lanes keep the multiply chain short; column c folds
// into lane c % 4, so the whole-row and cell-by-cell forms agree. Callers fold a color epoch that
// retires plain-row fingerprints taken under other colors.
class RowFingerprint final {
public:
  constexpr RowFingerprint(const FingerprintKey& key, const FingerprintLanes& seeds) noexcept
      : key_(&key), lanes_(seeds) {}

  constexpr void add(const std::size_t column, const std::uint64_t word) noexcept {
    auto& lane = std::span(lanes_).subspan(column % lanes_.size(), 1).front();
    lane = fingerprint_mix(*key_, lane, word);
  }

  [[nodiscard]] constexpr auto finish(const std::size_t columns,
                                      const std::uint64_t color_epoch) const noexcept
      -> std::uint64_t {
    const auto& key = *key_;
    const auto [first, second, third, fourth] = lanes_;
    auto hash = fingerprint_mix(key, fingerprint_mix(key, first, second),
                                fingerprint_mix(key, third, fourth));
    hash = fingerprint_mix(key, hash, columns);
    return fingerprint_mix(key, hash, color_epoch);
  }

  // Whole-row form, out of line so encoding loops that call it stay small.
  [[nodiscard]] static auto of(const FingerprintKey& key, const FingerprintLanes& seeds,
                               std::span<const std::uint64_t> words,
                               std::uint64_t color_epoch) noexcept -> std::uint64_t;

private:
  const FingerprintKey* key_;
  FingerprintLanes lanes_;
};

} // namespace lemma::vt::detail

#endif // LEMMA_TERMINAL_FINGERPRINT_HPP
