#include "core/client_frame_output.hpp"

#include "diagnostic/latency_trace.hpp"
#include "protocol/attachment.hpp"
#include "render/frame_buffer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace lemma::core {

[[nodiscard]] auto ClientFrameBytes::first(const std::size_t bytes) const noexcept
    -> ClientFrameBytes {
  const auto head_bytes = std::min(bytes, head.size());
  return {.head = head.first(head_bytes),
          .tail = tail.first(std::min(bytes - head_bytes, tail.size()))};
}

[[nodiscard]] auto ClientFrameOutput::begin_queue(const std::size_t bytes, const TimePoint now,
                                                  const std::uint64_t trace_correlation) noexcept
    -> bool {
  if (busy() || bytes == 0) {
    return false;
  }
  size_ = bytes;
  offset_ = 0;
  queued_at_ = now;
  last_progress_at_ = now;
  write_ready_ = true;
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  latency_trace_correlation_ = trace_correlation;
#else
  static_cast<void>(trace_correlation);
#endif
  return true;
}

[[nodiscard]] auto ClientFrameOutput::frame_message_count(const std::size_t frame_bytes) noexcept
    -> std::size_t {
  return frame_bytes == 0 ? 0 : 1U + ((frame_bytes - 1U) / protocol::render_ansi_bytes_max);
}

[[nodiscard]] auto
ClientFrameOutput::queue_frame(const std::size_t frame_bytes, const std::uint32_t sequence,
                               const std::uint32_t full_redraw_generation, const bool full_redraw,
                               const TimePoint now, const std::uint64_t trace_correlation) noexcept
    -> bool {
  const auto messages = frame_message_count(frame_bytes);
  if (frame_bytes == 0 || frame_bytes > render::frame_bytes_max || sequence == 0 ||
      full_redraw_generation == 0 || messages == 0 ||
      messages - 1U > std::numeric_limits<std::uint32_t>::max() - sequence ||
      !begin_queue(frame_bytes + (messages * frame_header_.size()), now, trace_correlation)) {
    return false;
  }
  frame_bytes_ = frame_bytes;
  frame_offset_ = 0;
  frame_message_count_ = messages;
  frame_sequence_ = sequence;
  frame_generation_ = full_redraw_generation;
  source_ = Source::frame;
  prepare_frame_chunk(full_redraw);
  return true;
}

void ClientFrameOutput::prepare_frame_chunk(const bool full_redraw) noexcept {
  frame_chunk_bytes_ = std::min(protocol::render_ansi_bytes_max, frame_bytes_ - frame_offset_);
  frame_chunk_offset_ = 0;
  frame_header_offset_ = 0;
  frame_header_ = protocol::encode_render_frame_header(frame_chunk_bytes_, frame_sequence_,
                                                       frame_generation_, full_redraw);
}

[[nodiscard]] auto ClientFrameOutput::queue_disconnect(const protocol::DisconnectReason reason,
                                                       const std::string_view diagnostic,
                                                       const std::uint32_t sequence,
                                                       const TimePoint now) noexcept -> bool {
  if (diagnostic.size() > protocol::diagnostic_bytes_max || sequence == 0) {
    return false;
  }
  const auto encoded = protocol::encode_disconnect(reason, diagnostic, sequence);
  if (!begin_queue(encoded.bytes().size(), now, 0)) {
    return false;
  }
  std::ranges::copy(encoded.bytes(), inline_message_.begin());
  frame_bytes_ = 0;
  frame_offset_ = 0;
  frame_chunk_bytes_ = 0;
  frame_chunk_offset_ = 0;
  frame_header_offset_ = 0;
  frame_message_count_ = 0;
  frame_sequence_ = 0;
  frame_generation_ = 0;
  source_ = Source::inline_message;
  return true;
}

[[nodiscard]] auto ClientFrameOutput::readable(const render::FrameBuffer& frame) const noexcept
    -> ClientFrameBytes {
  if (offset_ > size_) {
    return {};
  }
  if (source_ == Source::inline_message) {
    return {.head = std::span(inline_message_).first(size_).subspan(offset_), .tail = {}};
  }
  if (source_ != Source::frame) {
    return {};
  }
  const auto bytes = frame.readable(frame_bytes_);
  if (bytes.size() != frame_bytes_ || frame_offset_ > bytes.size() ||
      frame_chunk_offset_ > frame_chunk_bytes_) {
    return {};
  }
  return {.head = std::span(frame_header_).subspan(frame_header_offset_),
          .tail = bytes.subspan(frame_offset_, frame_chunk_bytes_ - frame_chunk_offset_)};
}

#ifdef LEMMA_ENABLE_LATENCY_TRACE
[[nodiscard]] auto ClientFrameOutput::trace_correlation() const noexcept -> std::uint64_t {
  return latency_trace_correlation_;
}
#endif

[[nodiscard]] auto ClientFrameOutput::deadline() const noexcept -> std::optional<TimePoint> {
  if (!busy()) {
    return std::nullopt;
  }
  return std::min(last_progress_at_ + attached_client_no_progress_timeout,
                  queued_at_ + attached_client_frame_total_timeout);
}

[[nodiscard]] auto ClientFrameOutput::expired(const TimePoint now) const noexcept -> bool {
  const auto progress_deadline = deadline();
  return progress_deadline.has_value() && now >= *progress_deadline;
}

void ClientFrameOutput::mark_write_ready() noexcept {
  if (busy()) {
    write_ready_ = true;
  }
}

[[nodiscard]] auto ClientFrameOutput::consume(const std::size_t bytes, const TimePoint now) noexcept
    -> bool {
  if (bytes == 0 || bytes > size_ - offset_) {
    return false;
  }
  // One write covers at most the current message: its header remainder and chunk remainder.
  const auto header_bytes = source_ == Source::frame
                                ? std::min(bytes, frame_header_.size() - frame_header_offset_)
                                : std::size_t{0};
  const auto chunk_bytes = bytes - header_bytes;
  if (source_ == Source::frame && chunk_bytes > frame_chunk_bytes_ - frame_chunk_offset_) {
    return false;
  }
  offset_ += bytes;
  last_progress_at_ = now;
  if (source_ != Source::frame) {
    return true;
  }
  frame_header_offset_ += header_bytes;
  if (chunk_bytes == 0) {
    return true;
  }

  frame_offset_ += chunk_bytes;
  frame_chunk_offset_ += chunk_bytes;
  if (frame_chunk_offset_ == frame_chunk_bytes_ && frame_offset_ < frame_bytes_) {
    ++frame_sequence_;
    prepare_frame_chunk(false);
  }
  return true;
}

void ClientFrameOutput::reset() noexcept {
  size_ = 0;
  frame_bytes_ = 0;
  frame_offset_ = 0;
  frame_chunk_bytes_ = 0;
  frame_chunk_offset_ = 0;
  frame_header_offset_ = 0;
  frame_message_count_ = 0;
  offset_ = 0;
  frame_sequence_ = 0;
  frame_generation_ = 0;
  queued_at_ = {};
  last_progress_at_ = {};
  source_ = Source::none;
  write_ready_ = false;
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  latency_trace_correlation_ = 0;
#endif
}

// The branches are the explicit bounded outcomes of one nonblocking frame flush.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto flush_client_frame(ClientFrameFlushTarget& target, std::size_t& global_budget,
                                      const ClientFrameOutput::TimePoint now) noexcept
    -> ClientFrameFlushStatus {
  auto* const output = target.output;
  if (output == nullptr || target.frame == nullptr) {
    target.status = ClientFrameFlushStatus::hard_error;
    return target.status;
  }
  if (!output->busy()) {
    target.status = ClientFrameFlushStatus::drained;
    return target.status;
  }
  if (output->expired(now)) {
    target.status = ClientFrameFlushStatus::deadline_exceeded;
    return target.status;
  }
  if (!output->write_ready() || target.write == nullptr || global_budget == 0) {
    target.status = ClientFrameFlushStatus::pending;
    return target.status;
  }

  output->clear_write_ready();
  std::size_t budget = std::min(attached_client_write_bytes_per_client_turn_max, global_budget);
  std::size_t attempts = 0;
  while (output->busy() && budget > 0 && attempts < attached_client_write_attempts_per_turn_max) {
    ++attempts;
    const auto readable = output->readable(*target.frame);
    if (readable.empty()) {
      target.status = ClientFrameFlushStatus::hard_error;
      return target.status;
    }
    const auto bytes = readable.first(std::min(readable.size(), budget));
    const auto result = target.write(target.context, bytes);
    if (result.bytes > 0) {
      const auto size = static_cast<std::size_t>(result.bytes);
      if (size > bytes.size() || !output->consume(size, now)) {
        target.status = ClientFrameFlushStatus::hard_error;
        return target.status;
      }
      std::uint64_t trace_correlation = 0;
#ifdef LEMMA_ENABLE_LATENCY_TRACE
      trace_correlation = output->trace_correlation();
#endif
      diagnostic::record_latency_trace(diagnostic::LatencyTraceStage::daemon_socket_write_progress,
                                       static_cast<std::uint32_t>(target.descriptor), size,
                                       trace_correlation);
      budget -= size;
      global_budget -= size;
      continue;
    }
    if (result.bytes == 0) {
      target.status = ClientFrameFlushStatus::hard_error;
      return target.status;
    }
    if (result.error == EINTR) {
      continue;
    }
    if (result.error == EAGAIN || result.error == EWOULDBLOCK) {
      target.status = ClientFrameFlushStatus::blocked;
      return target.status;
    }
    target.status = ClientFrameFlushStatus::hard_error;
    return target.status;
  }
  if (output->busy()) {
    target.status = ClientFrameFlushStatus::pending;
    return target.status;
  }
  output->reset();
  target.status = ClientFrameFlushStatus::drained;
  return target.status;
}

void flush_ready_client_frames(const std::span<ClientFrameFlushTarget> targets, std::size_t& cursor,
                               std::size_t& global_budget,
                               const ClientFrameOutput::TimePoint now) noexcept {
  if (targets.empty()) {
    cursor = 0;
    return;
  }
  cursor %= targets.size();
  std::size_t visited = 0;
  for (; visited < targets.size() && global_budget > 0; ++visited) {
    auto& target = targets.subspan((cursor + visited) % targets.size(), 1).front();
    target.status = ClientFrameFlushStatus::not_attempted;
    if (target.output != nullptr && target.output->busy() &&
        (target.output->write_ready() || target.output->expired(now))) {
      static_cast<void>(flush_client_frame(target, global_budget, now));
    }
  }
  cursor = (cursor + visited) % targets.size();
}

} // namespace lemma::core
