#ifndef LEMMA_CORE_CLIENT_FRAME_OUTPUT_HPP
#define LEMMA_CORE_CLIENT_FRAME_OUTPUT_HPP

#include "lemma/limits.hpp"
#include "protocol/attachment.hpp"
#include "render/frame_buffer.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace lemma::core {

inline constexpr std::size_t attached_client_write_bytes_per_client_turn_max =
    std::size_t{64} * 1'024U;
inline constexpr std::size_t attached_client_write_bytes_per_turn_max = std::size_t{256} * 1'024U;
inline constexpr std::size_t attached_client_write_attempts_per_turn_max = 32;
inline constexpr auto attached_client_no_progress_timeout =
    limits::frame_transaction_progress_deadline;
inline constexpr auto attached_client_frame_total_timeout =
    limits::frame_transaction_total_deadline;

// The unsent bytes of one queued message: its header remainder followed by its payload remainder.
// One gather write can send both, so a small message reaches the client in one syscall and the
// client never wakes for a header alone. Either part may be empty.
struct ClientFrameBytes final {
  std::span<const std::byte> head;
  std::span<const std::byte> tail;

  [[nodiscard]] auto size() const noexcept -> std::size_t { return head.size() + tail.size(); }
  [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }
  // The leading `bytes` bytes, bounded by size().
  [[nodiscard]] auto first(std::size_t bytes) const noexcept -> ClientFrameBytes;
};

class ClientFrameOutput final {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  [[nodiscard]] static auto frame_message_count(std::size_t frame_bytes) noexcept -> std::size_t;
  [[nodiscard]] auto queue_frame(std::size_t frame_bytes, std::uint32_t sequence,
                                 std::uint32_t full_redraw_generation, bool full_redraw,
                                 TimePoint now, std::uint64_t trace_correlation = 0) noexcept
      -> bool;
  [[nodiscard]] auto queue_disconnect(protocol::DisconnectReason reason,
                                      std::string_view diagnostic, std::uint32_t sequence,
                                      TimePoint now) noexcept -> bool;
  [[nodiscard]] auto readable(const render::FrameBuffer& frame) const noexcept -> ClientFrameBytes;
  [[nodiscard]] auto busy() const noexcept -> bool { return offset_ < size_; }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto frame_bytes() const noexcept -> std::size_t { return frame_bytes_; }
  [[nodiscard]] auto queued_message_count() const noexcept -> std::size_t {
    return frame_message_count_;
  }
  [[nodiscard]] auto offset() const noexcept -> std::size_t { return offset_; }
  [[nodiscard]] auto write_ready() const noexcept -> bool { return write_ready_; }
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  [[nodiscard]] auto trace_correlation() const noexcept -> std::uint64_t;
#endif
  [[nodiscard]] auto deadline() const noexcept -> std::optional<TimePoint>;
  [[nodiscard]] auto expired(TimePoint now) const noexcept -> bool;

  void mark_write_ready() noexcept;
  void clear_write_ready() noexcept { write_ready_ = false; }
  [[nodiscard]] auto consume(std::size_t bytes, TimePoint now) noexcept -> bool;
  void reset() noexcept;

private:
  enum class Source : std::uint8_t {
    none,
    frame,
    inline_message,
  };

  [[nodiscard]] auto begin_queue(std::size_t bytes, TimePoint now,
                                 std::uint64_t trace_correlation) noexcept -> bool;
  void prepare_frame_chunk(bool full_redraw) noexcept;

  std::array<std::byte, protocol::attach_header_bytes + protocol::render_generation_bytes>
      frame_header_{};
  std::array<std::byte, protocol::small_message_bytes_max> inline_message_{};
  std::size_t size_{0};
  std::size_t frame_bytes_{0};
  std::size_t frame_offset_{0};
  std::size_t frame_chunk_bytes_{0};
  std::size_t frame_chunk_offset_{0};
  std::size_t frame_header_offset_{0};
  std::size_t frame_message_count_{0};
  std::size_t offset_{0};
  std::uint32_t frame_sequence_{0};
  std::uint32_t frame_generation_{0};
  TimePoint queued_at_;
  TimePoint last_progress_at_;
  Source source_{Source::none};
  bool write_ready_{false};
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  std::uint64_t latency_trace_correlation_{0};
#endif
};

struct ClientFrameWriteAttempt final {
  std::ptrdiff_t bytes{0};
  int error{0};
};

using ClientFrameWriteOperation = ClientFrameWriteAttempt (*)(void* context,
                                                              ClientFrameBytes bytes) noexcept;

enum class ClientFrameFlushStatus : std::uint8_t {
  not_attempted,
  drained,
  pending,
  blocked,
  deadline_exceeded,
  hard_error,
};

struct ClientFrameFlushTarget final {
  int descriptor{-1};
  const render::FrameBuffer* frame{nullptr};
  ClientFrameOutput* output{nullptr};
  ClientFrameWriteOperation write{nullptr};
  void* context{nullptr};
  ClientFrameFlushStatus status{ClientFrameFlushStatus::not_attempted};
};

// Flushes one retained frame without consuming bytes until the injected writer reports progress.
// The daemon-wide budget is reduced only by bytes actually written.
[[nodiscard]] auto flush_client_frame(ClientFrameFlushTarget& target, std::size_t& global_budget,
                                      ClientFrameOutput::TimePoint now) noexcept
    -> ClientFrameFlushStatus;

// Visits attached clients from a persistent round-robin cursor. A target can consume at most its
// per-client budget; all targets together share global_budget for this reactor turn.
void flush_ready_client_frames(std::span<ClientFrameFlushTarget> targets, std::size_t& cursor,
                               std::size_t& global_budget,
                               ClientFrameOutput::TimePoint now) noexcept;

} // namespace lemma::core

#endif // LEMMA_CORE_CLIENT_FRAME_OUTPUT_HPP
