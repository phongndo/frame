#include "terminal/fingerprint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>
#endif

namespace lemma::vt::detail {
namespace {

[[nodiscard]] auto generate_fingerprint_key() noexcept -> std::optional<FingerprintKey> {
  // A random draw is valid with overwhelming probability; bound the retries anyway.
  constexpr int attempts_max = 4;
  for (int attempt = 0; attempt < attempts_max; ++attempt) {
    std::array<std::uint64_t, 7> words{};
    static_assert(sizeof(words) <= 256, "getentropy returns at most 256 bytes");
    if (::getentropy(words.data(), sizeof(words)) != 0) {
      return std::nullopt;
    }
    const auto [state, value, initial, first, second, third, fourth] = words;
    const FingerprintKey key{
        .state = state,
        .value = value,
        .initial = initial,
        .plain_lanes = {first, second, third, fourth},
    };
    if (key.valid()) {
      return key;
    }
  }
  return std::nullopt;
}

} // namespace

auto RowFingerprint::of(const FingerprintKey& key, const FingerprintLanes& seeds,
                        const std::span<const std::uint64_t> words,
                        const std::uint64_t color_epoch) noexcept -> std::uint64_t {
  RowFingerprint fingerprint(key, seeds);
  auto [first, second, third, fourth] = seeds;
  const auto whole = words.size() - (words.size() % seeds.size());
  // Named lanes stay in registers.
  for (std::size_t column = 0; column < whole; column += seeds.size()) {
    first = fingerprint_mix(key, first, words.subspan(column, 1).front());
    second = fingerprint_mix(key, second, words.subspan(column + 1U, 1).front());
    third = fingerprint_mix(key, third, words.subspan(column + 2U, 1).front());
    fourth = fingerprint_mix(key, fourth, words.subspan(column + 3U, 1).front());
  }
  fingerprint.lanes_ = {first, second, third, fourth};
  for (std::size_t column = whole; column < words.size(); ++column) {
    fingerprint.add(column, words.subspan(column, 1).front());
  }
  return fingerprint.finish(words.size(), color_epoch);
}

auto process_fingerprint_key() noexcept -> const FingerprintKey* {
  static const auto key = generate_fingerprint_key();
  return key.has_value() ? &*key : nullptr;
}

} // namespace lemma::vt::detail
