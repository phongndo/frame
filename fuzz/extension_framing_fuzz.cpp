#include "extension/protocol.hpp"
#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include <sys/socket.h>

namespace {

void require(const bool condition) {
  if (!condition) {
    std::abort();
  }
}

void drain_records(frame::extension::FramedPeer& peer) {
  while (peer.connected()) {
    const auto buffered = peer.buffered_record();
    const auto record = peer.receive();
    if (!record.has_value()) {
      return;
    }
    require(buffered);
    require(record->payload.size() <= frame::limits::extension_record_bytes_max);
    // receive claims one borrowed record; readiness inspection must not claim it again.
    require(peer.buffered_record());
    require(!peer.receive().has_value());
    peer.consume();
  }
}

} // namespace

// libFuzzer owns this ABI name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
  using namespace frame::extension;
  if (data == nullptr || size < 2U || size > frame::limits::extension_record_bytes_max * 2U) {
    return 0;
  }
  std::array<int, 2> sockets{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0) {
    return 0;
  }
  FramedPeer reader(sockets.front());
  FramedPeer writer(sockets.back());
  const auto source = std::as_bytes(std::span(data, size));
  const auto small_chunk = 1U + std::to_integer<std::size_t>(source.front());
  const auto input = source.subspan(1);
  std::size_t offset = 0;
  while (offset < input.size() && reader.connected() && writer.connected()) {
    // Fragment the first headers at arbitrary boundaries without making maximum-size inputs
    // require millions of syscalls. Later reads still exercise partial payloads and compaction.
    const auto chunk = offset < 64U ? small_chunk : frame::limits::extension_io_bytes_per_turn_max;
    const auto bytes = input.subspan(offset).first(std::min(chunk, input.size() - offset));
    const auto sent = ::send(writer.descriptor(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (sent <= 0) {
      break;
    }
    offset += static_cast<std::size_t>(sent);
    static_cast<void>(reader.read_ready());
    drain_records(reader);
  }
  static_cast<void>(::shutdown(writer.descriptor(), SHUT_WR));
  static_cast<void>(reader.read_ready());
  drain_records(reader);
  reader.disconnect();
  require(reader.output_accounting() == OutputAccounting{});
  return 0;
}
