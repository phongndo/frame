#include "protocol/attachment.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace {

void fuzz_client_decoder(const std::span<const std::byte> input, const std::size_t chunk_max) {
  frame::protocol::ClientDecoder decoder;
  if (!decoder.prepare().has_value()) {
    return;
  }
  std::size_t offset = 0;
  while (offset < input.size()) {
    auto writable = decoder.writable_bytes();
    if (writable.empty()) {
      return;
    }
    const auto copied = std::min({chunk_max, writable.size(), input.size() - offset});
    std::ranges::copy(input.subspan(offset, copied), writable.begin());
    if (!decoder.commit(copied).has_value()) {
      return;
    }
    offset += copied;
    while (true) {
      const auto decoded = decoder.next();
      if (!decoded.has_value()) {
        return;
      }
      if (!decoded->has_value()) {
        break;
      }
      decoder.consume();
    }
  }
  [[maybe_unused]] const auto final = decoder.next();
}

void fuzz_server_decoder(const std::span<const std::byte> input, const std::size_t chunk_max) {
  frame::protocol::ServerDecoder decoder;
  if (!decoder.prepare().has_value()) {
    return;
  }
  decoder.reset(1, false);
  std::size_t offset = 0;
  while (offset < input.size()) {
    auto writable = decoder.writable_bytes();
    if (writable.empty()) {
      return;
    }
    const auto copied = std::min({chunk_max, writable.size(), input.size() - offset});
    std::ranges::copy(input.subspan(offset, copied), writable.begin());
    if (!decoder.commit(copied).has_value()) {
      return;
    }
    offset += copied;
    while (true) {
      const auto decoded = decoder.next();
      if (!decoded.has_value()) {
        return;
      }
      if (!decoded->has_value()) {
        break;
      }
      decoder.consume();
    }
  }
  [[maybe_unused]] const auto final = decoder.next();
}

} // namespace

// libFuzzer owns this ABI name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
  if (data == nullptr || size < 2 ||
      size >
          frame::protocol::server_decoder_bytes_max + frame::protocol::client_decoder_bytes_max) {
    return 0;
  }
  const auto source = std::span(data, size);
  const auto bytes = std::as_bytes(source);
  const auto chunk_max = 1U + (static_cast<std::size_t>(source.subspan(1, 1).front()) % 64U);
  if ((source.front() & 1U) == 0) {
    fuzz_client_decoder(bytes.subspan(2), chunk_max);
  } else {
    fuzz_server_decoder(bytes.subspan(2), chunk_max);
  }
  return 0;
}
