#include "core/input.hpp"

#include "lemma/assert.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "protocol/attachment.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace lemma::core {

std::atomic_size_t PanePtyWriteQueue::allocated_bytes{0};

[[nodiscard]] auto PanePtyWriteQueue::allocated_bytes_current() noexcept -> std::size_t {
  return allocated_bytes.load(std::memory_order_relaxed);
}

PanePtyWriteQueue::~PanePtyWriteQueue() { release_storage(); }

auto PanePtyWriteQueue::readable_span() const noexcept -> std::span<const std::byte> {
  LEMMA_ASSERT(size_ <= capacity());
  if (size_ == 0) {
    return {};
  }
  LEMMA_ASSERT(storage_ != nullptr);
  LEMMA_ASSERT(read_offset_ <= storage_capacity_);
  LEMMA_ASSERT(size_ <= storage_capacity_ - read_offset_);
  return std::span(storage_.get(), storage_capacity_).subspan(read_offset_, size_);
}

auto PanePtyWriteQueue::consume(const std::size_t bytes) noexcept -> bool {
  LEMMA_ASSERT(size_ <= capacity());
  if (bytes > size_) {
    return false;
  }
  read_offset_ += bytes;
  size_ -= bytes;
  if (size_ == 0) {
    // Keep this pane-owned capacity for the next input/terminal-response packet. Lifecycle clear or
    // destruction releases it; ordinary flush progress never re-enters the allocator.
    read_offset_ = 0;
  }
  return true;
}

auto PanePtyWriteQueue::reserve(const std::size_t bytes) noexcept -> bool {
  if (bytes > remaining()) {
    return false;
  }
  return ensure_capacity(size_ + bytes);
}

auto PanePtyWriteQueue::writable_span() noexcept -> std::span<std::byte> {
  if (storage_ == nullptr) {
    return {};
  }
  const auto write_offset = read_offset_ + size_;
  LEMMA_ASSERT(write_offset <= storage_capacity_);
  return std::span(storage_.get(), storage_capacity_).subspan(write_offset);
}

auto PanePtyWriteQueue::commit_write(const std::size_t bytes) noexcept -> bool {
  if (bytes > remaining() || bytes > writable_span().size()) {
    return false;
  }
  size_ += bytes;
  return true;
}

auto PanePtyWriteQueue::append(const std::span<const std::byte> input) noexcept -> bool {
  if (!reserve(input.size())) {
    return false;
  }
  if (input.empty()) {
    return true;
  }
  const auto write_offset = read_offset_ + size_;
  LEMMA_ASSERT(write_offset <= storage_capacity_);
  LEMMA_ASSERT(input.size() <= storage_capacity_ - write_offset);
  auto storage = std::span(storage_.get(), storage_capacity_);
  std::memcpy(storage.subspan(write_offset, input.size()).data(), input.data(), input.size());
  size_ += input.size();
  return true;
}

auto PanePtyWriteQueue::read(const std::span<std::byte> output) noexcept -> std::size_t {
  const auto readable = readable_span();
  const auto bytes = std::min(output.size(), readable.size());
  if (bytes == 0) {
    return 0;
  }
  std::memcpy(output.data(), readable.data(), bytes);
  const bool consumed = consume(bytes);
  LEMMA_ASSERT(consumed);
  return bytes;
}

void PanePtyWriteQueue::clear() noexcept { release_storage(); }

auto PanePtyWriteQueue::ensure_capacity(const std::size_t required) noexcept -> bool {
  if (required > capacity()) {
    return false;
  }
  if (required <= storage_capacity_) {
    if (read_offset_ > storage_capacity_ - required) {
      LEMMA_ASSERT(storage_ != nullptr);
      const auto storage = std::span(storage_.get(), storage_capacity_);
      std::memmove(storage_.get(), storage.subspan(read_offset_, size_).data(), size_);
      read_offset_ = 0;
    }
    return true;
  }

  constexpr std::size_t allocation_granularity = std::size_t{4} * 1'024U;
  const auto ceiling = required <= limits::pane_pty_input_backlog_bytes_max
                           ? limits::pane_pty_input_backlog_bytes_max
                           : capacity();
  const auto doubled = std::min(ceiling, storage_capacity_ * 2U);
  const auto preferred = std::min(ceiling, std::max({required, allocation_granularity, doubled}));
  if (replace_storage(preferred)) {
    return true;
  }
  return preferred != required && replace_storage(required);
}

auto PanePtyWriteQueue::replace_storage(const std::size_t new_capacity) noexcept -> bool {
  LEMMA_ASSERT(new_capacity > storage_capacity_);
  LEMMA_ASSERT(new_capacity >= size_);
  LEMMA_ASSERT(new_capacity <= capacity());
  if (!acquire_allocation(new_capacity)) {
    return false;
  }
  // Runtime-sized non-throwing storage cannot use std::array.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  std::unique_ptr<std::byte[]> replacement;
  try {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    replacement = std::make_unique_for_overwrite<std::byte[]>(new_capacity);
  } catch (const std::bad_alloc&) {
    release_allocation(new_capacity);
    return false;
  }
  if (size_ > 0) {
    LEMMA_ASSERT(storage_ != nullptr);
    const auto storage = std::span(storage_.get(), storage_capacity_);
    std::memcpy(replacement.get(), storage.subspan(read_offset_, size_).data(), size_);
  }

  const auto previous_capacity = storage_capacity_;
  storage_ = std::move(replacement);
  storage_capacity_ = new_capacity;
  read_offset_ = 0;
  release_allocation(previous_capacity);
  return true;
}

auto PanePtyWriteQueue::acquire_allocation(const std::size_t bytes) noexcept -> bool {
  auto allocated = allocated_bytes.load(std::memory_order_relaxed);
  const auto maximum = limits::pane_pty_write_queue_bytes_aggregate_max;
  while (allocated <= maximum && bytes <= maximum - allocated) {
    if (allocated_bytes.compare_exchange_weak(allocated, allocated + bytes,
                                              std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

void PanePtyWriteQueue::release_allocation(const std::size_t bytes) noexcept {
  if (bytes == 0) {
    return;
  }
  const auto previous = allocated_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  LEMMA_ASSERT(previous >= bytes);
}

void PanePtyWriteQueue::release_storage() noexcept {
  const auto released = storage_capacity_;
  storage_.reset();
  storage_capacity_ = 0;
  read_offset_ = 0;
  size_ = 0;
  release_allocation(released);
}

[[nodiscard]] auto queue_terminal_responses(PanePtyWriteQueue& queue,
                                            vt::Terminal& terminal) noexcept -> bool {
  if (terminal.integrity_failed()) {
    return false;
  }
  const auto pending_bytes = terminal.pending_pty_response_bytes();
  if (pending_bytes > queue.remaining() || !queue.reserve(pending_bytes)) {
    return false;
  }
  std::array<std::byte, std::size_t{4} * 1'024U> response{};
  while (terminal.pending_pty_response_bytes() > 0) {
    const auto size = terminal.read_pty_responses(response);
    if (size == 0 || !queue.append(std::span(response).first(size))) {
      return false;
    }
  }
  return true;
}

namespace {

constexpr std::size_t key_encoding_bytes_max = 128;
static_assert(limits::normalized_client_input_bytes_max ==
              protocol::input_bytes_max * 2U * key_encoding_bytes_max);
static_assert(limits::pane_pty_write_queue_bytes_max >=
              limits::terminal_pty_response_bytes_max + limits::normalized_client_input_bytes_max);

[[nodiscard]] constexpr auto control_key(const std::byte byte) noexcept -> vt::Key {
  const auto value = std::to_integer<std::uint8_t>(byte);
  LEMMA_ASSERT(value >= 1 && value <= 26);
  return static_cast<vt::Key>(static_cast<std::uint8_t>(vt::Key::a) + value - 1U);
}

[[nodiscard]] constexpr auto arrow_key(const std::byte final) noexcept -> vt::Key {
  switch (std::to_integer<char>(final)) {
  case 'A':
    return vt::Key::arrow_up;
  case 'B':
    return vt::Key::arrow_down;
  case 'C':
    return vt::Key::arrow_right;
  case 'D':
    return vt::Key::arrow_left;
  case 'F':
    return vt::Key::end;
  case 'H':
    return vt::Key::home;
  default:
    return vt::Key::unidentified;
  }
}

[[nodiscard]] constexpr auto terminal_key_modifiers(const std::uint16_t modifiers) noexcept
    -> std::uint16_t {
  std::uint16_t result = 0;
  result |= (modifiers & protocol::key_input_modifier_shift) != 0 ? vt::key_modifier_shift : 0U;
  result |= (modifiers & protocol::key_input_modifier_control) != 0 ? vt::key_modifier_control : 0U;
  result |= (modifiers & protocol::key_input_modifier_alt) != 0 ? vt::key_modifier_alt : 0U;
  result |= (modifiers & protocol::key_input_modifier_super) != 0 ? vt::key_modifier_super : 0U;
  result |=
      (modifiers & protocol::key_input_modifier_caps_lock) != 0 ? vt::key_modifier_caps_lock : 0U;
  result |=
      (modifiers & protocol::key_input_modifier_num_lock) != 0 ? vt::key_modifier_num_lock : 0U;
  return result;
}

[[nodiscard]] constexpr auto terminal_key_action(const protocol::KeyInputAction action) noexcept
    -> vt::KeyAction {
  switch (action) {
  case protocol::KeyInputAction::release:
    return vt::KeyAction::release;
  case protocol::KeyInputAction::press:
    return vt::KeyAction::press;
  case protocol::KeyInputAction::repeat:
    return vt::KeyAction::repeat;
  }
  return vt::KeyAction::press;
}

[[nodiscard]] auto queue_encoded(PanePtyWriteQueue& queue,
                                 const std::span<const std::byte> encoded) noexcept
    -> InputQueueResult {
  if (encoded.size() > queue.input_remaining() || !queue.reserve(encoded.size())) {
    return InputQueueResult::full;
  }
  return queue.append(encoded) ? InputQueueResult::queued : InputQueueResult::encoding_failed;
}

template <typename Visitor>
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto visit_normalized_input(vt::Terminal& terminal,
                                          const std::span<const std::byte> input,
                                          Visitor visitor) noexcept -> bool {
  // Printable bytes and UTF-8 pass through unchanged. Visiting a whole run at once keeps measuring
  // and queueing proportional to the special keys in it; a backpressured retry re-measures its
  // complete held message.
  const auto passes_through = [](const std::byte byte) noexcept {
    const auto value = std::to_integer<std::uint8_t>(byte);
    return value >= 0x20U && value != 0x7FU;
  };
  std::size_t input_offset = 0;
  while (input_offset < input.size()) {
    if (passes_through(input.subspan(input_offset, 1).front())) {
      std::size_t run = 1;
      while (input_offset + run < input.size() &&
             passes_through(input.subspan(input_offset + run, 1).front())) {
        ++run;
      }
      if (!visitor(input.subspan(input_offset, run))) {
        return false;
      }
      input_offset += run;
      continue;
    }
    vt::KeyEvent event{};
    std::array<char, 1> event_text{};
    std::size_t consumed = 0;
    const auto byte = input.subspan(input_offset, 1).front();
    const auto value = std::to_integer<std::uint8_t>(byte);
    if (byte == std::byte{0x0D}) {
      // Enter and Ctrl-M share the legacy byte. Applications using Kitty modes receive Enter.
      event.key = vt::Key::enter;
      consumed = 1;
    } else if (byte == std::byte{0x09}) {
      event.key = vt::Key::tab;
      consumed = 1;
    } else if (byte == std::byte{0x7F}) {
      event.key = vt::Key::backspace;
      consumed = 1;
    } else if (value >= 1 && value <= 26) {
      event.key = control_key(byte);
      event.modifiers = vt::key_modifier_control;
      event.unshifted_codepoint = static_cast<std::uint32_t>('a' + value - 1U);
      event_text.front() = static_cast<char>('a' + value - 1U);
      event.text = std::string_view(event_text.data(), event_text.size());
      consumed = 1;
    } else if (input.size() - input_offset >= 3 && byte == std::byte{0x1B} &&
               (input.subspan(input_offset + 1, 1).front() == std::byte{'['} ||
                input.subspan(input_offset + 1, 1).front() == std::byte{'O'})) {
      event.key = arrow_key(input.subspan(input_offset + 2, 1).front());
      consumed = event.key == vt::Key::unidentified ? 0 : 3;
    }

    if (consumed == 0) {
      if (!visitor(input.subspan(input_offset, 1))) {
        return false;
      }
      ++input_offset;
      continue;
    }

    std::array<std::byte, key_encoding_bytes_max> encoded{};
    const auto key_bytes = terminal.encode_key(event, encoded);
    if (!key_bytes.has_value() || *key_bytes > encoded.size() ||
        !visitor(std::span(encoded).first(*key_bytes))) {
      return false;
    }
    input_offset += consumed;
  }
  return true;
}

} // namespace

[[nodiscard]] auto queue_normalized_input(PanePtyWriteQueue& queue, vt::Terminal& terminal,
                                          const std::span<const std::byte> input) noexcept
    -> InputQueueResult {
  if (terminal.integrity_failed() || input.size() > protocol::input_bytes_max * 2U) {
    return InputQueueResult::encoding_failed;
  }

  std::size_t encoded_size = 0;
  const bool measured =
      visit_normalized_input(terminal, input, [&](const std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > std::numeric_limits<std::size_t>::max() - encoded_size) {
          return false;
        }
        encoded_size += bytes.size();
        return encoded_size <= limits::normalized_client_input_bytes_max;
      });
  if (!measured) {
    return InputQueueResult::encoding_failed;
  }
  if (encoded_size > queue.input_remaining() || !queue.reserve(encoded_size)) {
    return InputQueueResult::full;
  }

  const bool appended =
      visit_normalized_input(terminal, input, [&](const std::span<const std::byte> bytes) noexcept {
        return queue.append(bytes);
      });
  LEMMA_ASSERT(appended);
  return appended ? InputQueueResult::queued : InputQueueResult::encoding_failed;
}

namespace {

[[nodiscard]] auto encode_protocol_key(vt::Terminal& terminal, const protocol::KeyInput& key,
                                       const std::span<const std::byte> text,
                                       const std::span<std::byte> output) noexcept
    -> std::optional<std::size_t> {
  constexpr std::array key_map{
      vt::Key::unidentified,
      vt::Key::a,
      vt::Key::b,
      vt::Key::c,
      vt::Key::d,
      vt::Key::e,
      vt::Key::f,
      vt::Key::g,
      vt::Key::h,
      vt::Key::i,
      vt::Key::j,
      vt::Key::k,
      vt::Key::l,
      vt::Key::m,
      vt::Key::n,
      vt::Key::o,
      vt::Key::p,
      vt::Key::q,
      vt::Key::r,
      vt::Key::s,
      vt::Key::t,
      vt::Key::u,
      vt::Key::v,
      vt::Key::w,
      vt::Key::x,
      vt::Key::y,
      vt::Key::z,
      vt::Key::enter,
      vt::Key::tab,
      vt::Key::backspace,
      vt::Key::escape,
      vt::Key::space,
      vt::Key::arrow_up,
      vt::Key::arrow_down,
      vt::Key::arrow_left,
      vt::Key::arrow_right,
      vt::Key::home,
      vt::Key::end,
      vt::Key::insert,
      vt::Key::delete_key,
      vt::Key::page_up,
      vt::Key::page_down,
      vt::Key::f1,
      vt::Key::f2,
      vt::Key::f3,
      vt::Key::f4,
      vt::Key::f5,
      vt::Key::f6,
      vt::Key::f7,
      vt::Key::f8,
      vt::Key::f9,
      vt::Key::f10,
      vt::Key::f11,
      vt::Key::f12,
  };
  const auto key_index = static_cast<std::size_t>(key.key);
  if (key_index >= key_map.size()) {
    return std::nullopt;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view encoded_text(reinterpret_cast<const char*>(text.data()), text.size());
  const vt::KeyEvent event{
      .action = terminal_key_action(key.action),
      .key = std::span(key_map).subspan(key_index, 1).front(),
      .modifiers = terminal_key_modifiers(key.modifiers),
      .consumed_modifiers = terminal_key_modifiers(key.consumed_modifiers),
      .unshifted_codepoint = key.unshifted_codepoint,
      .text = encoded_text,
      .composing = key.composing,
  };
  const auto encoded = terminal.encode_key(event, output);
  return encoded.has_value() ? std::optional<std::size_t>{*encoded} : std::nullopt;
}

} // namespace

[[nodiscard]] auto queue_key_input(PanePtyWriteQueue& queue, vt::Terminal& terminal,
                                   const protocol::KeyInput& key,
                                   const std::span<const std::byte> text) noexcept
    -> InputQueueResult {
  if (terminal.integrity_failed()) {
    return InputQueueResult::encoding_failed;
  }
  std::array<std::byte, key_encoding_bytes_max> encoded{};
  const auto result = encode_protocol_key(terminal, key, text, encoded);
  return result.has_value() ? queue_encoded(queue, std::span(encoded).first(*result))
                            : InputQueueResult::encoding_failed;
}

auto queue_prefixed_key_input(PanePtyWriteQueue& queue, vt::Terminal& terminal,
                              const std::span<const std::byte> prefix,
                              const protocol::KeyInput& key,
                              const std::span<const std::byte> text) noexcept -> InputQueueResult {
  constexpr std::size_t prefix_bytes_max = 4;
  std::array<std::byte, (prefix_bytes_max + 1U) * key_encoding_bytes_max> encoded{};
  if (terminal.integrity_failed() || prefix.empty() || prefix.size() > prefix_bytes_max) {
    return InputQueueResult::encoding_failed;
  }
  std::size_t encoded_size = 0;
  const auto prefix_encoded = visit_normalized_input(
      terminal, prefix, [&](const std::span<const std::byte> bytes) noexcept {
        if (bytes.size() > encoded.size() - encoded_size) {
          return false;
        }
        std::ranges::copy(bytes, std::span(encoded).subspan(encoded_size, bytes.size()).begin());
        encoded_size += bytes.size();
        return true;
      });
  if (!prefix_encoded) {
    return InputQueueResult::encoding_failed;
  }
  const auto key_size =
      encode_protocol_key(terminal, key, text, std::span(encoded).subspan(encoded_size));
  if (!key_size.has_value()) {
    return InputQueueResult::encoding_failed;
  }
  encoded_size += *key_size;
  return queue_encoded(queue, std::span(encoded).first(encoded_size));
}

[[nodiscard]] auto queue_paste_input(PanePtyWriteQueue& queue, vt::Terminal& terminal,
                                     const std::span<std::byte> input) noexcept
    -> InputQueueResult {
  constexpr std::size_t paste_encoding_overhead_max = 16;
  if (terminal.integrity_failed()) {
    return InputQueueResult::encoding_failed;
  }
  if (input.empty() || input.size() > protocol::input_message_bytes_max) {
    return InputQueueResult::encoding_failed;
  }
  if (input.size() > queue.input_remaining() ||
      paste_encoding_overhead_max > queue.input_remaining() - input.size()) {
    return InputQueueResult::full;
  }
  const auto reserved = input.size() + paste_encoding_overhead_max;
  if (!queue.reserve(reserved)) {
    return InputQueueResult::full;
  }
  const auto result = terminal.encode_paste(input, queue.writable_span());
  if (!result.has_value() || !queue.commit_write(*result)) {
    return InputQueueResult::encoding_failed;
  }
  return InputQueueResult::queued;
}

[[nodiscard]] auto queue_focus_input(PanePtyWriteQueue& queue, const vt::Terminal& terminal,
                                     const vt::FocusEvent event) noexcept -> InputQueueResult {
  if (terminal.integrity_failed()) {
    return InputQueueResult::encoding_failed;
  }
  std::array<std::byte, 8> encoded{};
  const auto result = terminal.encode_focus(event, encoded);
  if (!result.has_value()) {
    return InputQueueResult::encoding_failed;
  }
  return queue_encoded(queue, std::span(encoded).first(*result));
}

[[nodiscard]] auto queue_mouse_input(PanePtyWriteQueue& queue, vt::Terminal& terminal,
                                     const vt::MouseEvent& event) noexcept -> InputQueueResult {
  if (terminal.integrity_failed()) {
    return InputQueueResult::encoding_failed;
  }
  std::array<std::byte, limits::pixel_mouse_report_bytes_max> encoded{};
  const auto result = terminal.encode_mouse(event, encoded);
  if (!result.has_value()) {
    return InputQueueResult::encoding_failed;
  }
  return queue_encoded(queue, std::span(encoded).first(*result));
}

[[nodiscard]] auto queue_alternate_scroll_input(PanePtyWriteQueue& queue, vt::Terminal& terminal,
                                                const bool upward) noexcept -> InputQueueResult {
  if (terminal.integrity_failed()) {
    return InputQueueResult::encoding_failed;
  }
  const vt::KeyEvent event{
      .action = vt::KeyAction::press,
      .key = upward ? vt::Key::arrow_up : vt::Key::arrow_down,
      .modifiers = 0,
      .consumed_modifiers = 0,
      .unshifted_codepoint = 0,
      .text = {},
      .composing = false,
  };
  std::array<std::byte, 128> encoded{};
  const auto result = terminal.encode_key(event, encoded);
  return result.has_value() ? queue_encoded(queue, std::span(encoded).first(*result))
                            : InputQueueResult::encoding_failed;
}

} // namespace lemma::core
