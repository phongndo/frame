#ifndef FRAME_EXTENSION_PROTOCOL_HPP
#define FRAME_EXTENSION_PROTOCOL_HPP

#include "frame/limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace frame::extension {

inline constexpr std::array<std::byte, 4> protocol_magic{std::byte{0x8a}, std::byte{'L'},
                                                         std::byte{'M'}, std::byte{'E'}};
inline constexpr std::uint8_t protocol_major = 1;
inline constexpr std::uint8_t protocol_minor = 0;
inline constexpr std::size_t protocol_header_bytes = limits::extension_record_header_bytes;
inline constexpr std::string_view protocol_schema = "frame.extension/v1";
inline constexpr std::string_view surface_update_schema = "frame.surface-update/v1";

enum class RecordKind : std::uint8_t {
  hello = 1,
  welcome = 2,
  proc = 3,
  proc_result = 4,
  surface_update = 5,
  event = 6,
  error = 7,
};

struct OutputAccounting final {
  std::size_t reserved_bytes{0};
  std::size_t reserved_records{0};
  std::size_t event_bytes{0};
  std::size_t event_records{0};

  auto operator==(const OutputAccounting&) const -> bool = default;
};

struct Record final {
  std::span<const std::byte> payload;
  std::uint32_t sequence{0};
  RecordKind kind{RecordKind::error};
};

[[nodiscard]] auto encode_header(RecordKind kind, std::size_t payload_bytes,
                                 std::uint32_t sequence) noexcept
    -> std::array<std::byte, protocol_header_bytes>;

// One nonblocking, framed full-duplex extension connection. Payload views borrow input storage
// until consume(); output retains exact partial-write progress. This transport knows no language or
// extension execution model.
class FramedPeer final {
public:
  explicit FramedPeer(int descriptor = -1) noexcept;
  FramedPeer(const FramedPeer&) = delete;
  auto operator=(const FramedPeer&) -> FramedPeer& = delete;
  FramedPeer(FramedPeer&& other) noexcept;
  auto operator=(FramedPeer&& other) noexcept -> FramedPeer&;
  ~FramedPeer();

  [[nodiscard]] auto descriptor() const noexcept -> int { return descriptor_; }
  [[nodiscard]] auto events() const noexcept -> short;
  [[nodiscard]] auto connected() const noexcept -> bool { return descriptor_ >= 0; }
  [[nodiscard]] auto input_bytes() const noexcept -> std::size_t {
    return input_.size() - input_offset_;
  }
  [[nodiscard]] auto output_bytes() const noexcept -> std::size_t {
    return output_.size() - output_offset_;
  }

  [[nodiscard]] auto output_accounting() const noexcept -> OutputAccounting { return accounting_; }
  // The contiguous queue has no separate record-slot allocation. Reserve storage before admission;
  // every producer preserves it until the owner converts or cancels its reservation.
  [[nodiscard]] auto reserve_output(std::size_t framed_bytes) noexcept -> bool;
  void release_output(std::size_t framed_bytes) noexcept;

  // Setup already consumed the protocol discriminator. Prime it before the first read.
  [[nodiscard]] auto prime(std::byte first) noexcept -> bool;
  [[nodiscard]] auto
  read_ready(std::size_t bytes_max = limits::extension_io_bytes_per_turn_max) noexcept
      -> std::size_t;
  [[nodiscard]] auto buffered_record() const noexcept -> bool;
  [[nodiscard]] auto buffered_record_bytes() const noexcept -> std::optional<std::size_t>;
  auto write_ready(std::size_t bytes_max = limits::extension_io_bytes_per_turn_max) noexcept
      -> std::size_t;
  [[nodiscard]] auto receive() noexcept -> std::optional<Record>;
  void consume() noexcept;
  [[nodiscard]] auto send(RecordKind kind, std::uint32_t sequence,
                          std::span<const std::byte> payload) noexcept -> bool;
  [[nodiscard]] auto send_json(RecordKind kind, std::uint32_t sequence,
                               std::string_view payload) noexcept -> bool;
  [[nodiscard]] auto release_descriptor() noexcept -> int;
  void disconnect() noexcept;

private:
  [[nodiscard]] auto compact_input() noexcept -> bool;
  [[nodiscard]] auto complete_record() noexcept -> std::optional<Record>;
  void compact_output() noexcept;
  void grow_output(std::size_t required);
  void consume_output(std::size_t bytes) noexcept;

  int descriptor_{-1};
  std::vector<std::byte> input_;
  std::size_t input_offset_{0};
  std::size_t record_bytes_{0};
  std::vector<std::byte> output_;
  std::size_t output_offset_{0};
  OutputAccounting accounting_;
  std::size_t output_record_remaining_{0};
  RecordKind output_record_kind_{RecordKind::error};
};

} // namespace frame::extension

#endif // FRAME_EXTENSION_PROTOCOL_HPP
