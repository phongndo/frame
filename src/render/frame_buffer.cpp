#include "render/frame_buffer.hpp"

#include "render/pane_composition.hpp"
#include "render/scene.hpp"
#include "render/status_line.hpp"

#include "frame/assert.hpp"
#include "frame/limits.hpp"
#include "frame/terminal/terminal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>

namespace frame::render {
namespace {

[[nodiscard]] auto allocate_frame_storage(void* const /*context*/, const std::size_t bytes) noexcept
    -> FrameStorage {
  try {
    // Runtime-sized frame storage cannot use std::array.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    return std::make_unique_for_overwrite<std::byte[]>(bytes);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

} // namespace

[[nodiscard]] auto frame_capacity_for_viewport(const Viewport viewport) noexcept
    -> std::expected<std::size_t, FrameCapacityError> {
  if (viewport.columns == 0 || viewport.rows == 0 ||
      viewport.columns > limits::terminal_columns_hard_max ||
      viewport.rows > limits::terminal_rows_hard_max) {
    return std::unexpected(FrameCapacityError::invalid_viewport);
  }
  const auto columns = static_cast<std::size_t>(viewport.columns);
  const auto rows = static_cast<std::size_t>(viewport.rows);
  if (columns > std::numeric_limits<std::size_t>::max() / rows) {
    return std::unexpected(FrameCapacityError::arithmetic_overflow);
  }
  const auto cells = columns * rows;
  constexpr auto variable_bytes_max =
      std::numeric_limits<std::size_t>::max() - frame_fixed_overhead_bytes;
  if (cells > variable_bytes_max / frame_bytes_per_viewport_cell) {
    return std::unexpected(FrameCapacityError::arithmetic_overflow);
  }
  const auto calculated = frame_fixed_overhead_bytes + (cells * frame_bytes_per_viewport_cell);
  if (calculated > frame_bytes_max) {
    return std::unexpected(FrameCapacityError::transaction_too_large);
  }
  return std::max(frame_bytes_min, calculated);
}

[[nodiscard]] auto FrameCapacityBudget::reserve(const std::size_t bytes) noexcept -> bool {
  if (bytes > bytes_max_ - used_) {
    return false;
  }
  used_ += bytes;
  return true;
}

void FrameCapacityBudget::release(const std::size_t bytes) noexcept {
  FRAME_ASSERT(bytes <= used_);
  used_ -= bytes;
}

FrameBuffer::FrameBuffer(const FrameAllocationOperation allocate,
                         void* const allocation_context) noexcept
    : allocate_(allocate == nullptr ? &allocate_frame_storage : allocate),
      allocation_context_(allocation_context) {}

FrameBuffer::~FrameBuffer() { release(); }

void FrameBuffer::bind_capacity_budget(FrameCapacityBudget& budget) noexcept {
  FRAME_ASSERT(capacity_ == 0);
  FRAME_ASSERT(capacity_budget_ == nullptr);
  capacity_budget_ = &budget;
}

[[nodiscard]] auto FrameBuffer::prepare(const Viewport viewport,
                                        const std::size_t preserve_bytes) noexcept -> bool {
  const auto required = frame_capacity_for_viewport(viewport);
  if (!required.has_value() || preserve_bytes > capacity_) {
    return false;
  }
  if (*required <= capacity_) {
    return true;
  }
  // Reserve the complete replacement while the old backing is still live. This keeps the quota
  // true even during allocation; the old reservation is released only after the swap commits.
  if (capacity_budget_ != nullptr && !capacity_budget_->reserve(*required)) {
    return false;
  }
  auto replacement = allocate_(allocation_context_, *required);
  if (replacement == nullptr) {
    if (capacity_budget_ != nullptr) {
      capacity_budget_->release(*required);
    }
    return false;
  }
  if (preserve_bytes > 0) {
    std::memcpy(replacement.get(), storage_.get(), preserve_bytes);
  }
  const auto previous_capacity = capacity_;
  storage_ = std::move(replacement);
  capacity_ = *required;
  if (capacity_budget_ != nullptr) {
    capacity_budget_->release(previous_capacity);
  }
  return true;
}

void FrameBuffer::release() noexcept {
  storage_.reset();
  if (capacity_budget_ != nullptr) {
    capacity_budget_->release(capacity_);
  }
  capacity_ = 0;
}

[[nodiscard]] auto FrameBuffer::writable() noexcept -> std::span<std::byte> {
  return storage_ == nullptr ? std::span<std::byte>{}
                             : std::span<std::byte>(storage_.get(), capacity_);
}

[[nodiscard]] auto FrameBuffer::readable(const std::size_t bytes) const noexcept
    -> std::span<const std::byte> {
  if (storage_ == nullptr || bytes > capacity_) {
    return {};
  }
  return std::span<const std::byte>(storage_.get(), capacity_).first(bytes);
}

[[nodiscard]] auto
compose_retained_scene(const Scene scene, const Viewport viewport, FrameBuffer& frame,
                       const bool force_full, const StatusLine status,
                       const std::optional<OuterModeProjection> previous_outer_modes,
                       const MessageView message_view, GraphicsProjection* const graphics) noexcept
    -> std::expected<CompositionResult, CompositionError> {
  return compose_scene(scene, viewport, frame.writable(), force_full, status, previous_outer_modes,
                       message_view, graphics);
}

[[nodiscard]] auto
compose_retained_frame(const std::span<const PaneSurface> panes, const Viewport viewport,
                       FrameBuffer& frame, const bool force_full, const StatusLine status,
                       const std::optional<OuterModeProjection> previous_outer_modes,
                       const MessageView message_view) noexcept
    -> std::expected<CompositionResult, CompositionError> {
  return compose_retained_scene(Scene{.panes = panes, .grids = {}}, viewport, frame, force_full,
                                status, previous_outer_modes, message_view);
}

[[nodiscard]] auto compose_retained_single_pane(vt::Terminal& terminal, FrameBuffer& frame,
                                                const bool force_full) noexcept
    -> std::expected<CompositionResult, CompositionError> {
  const auto size = terminal.size();
  const PaneSurface pane{
      .terminal = &terminal,
      .rectangle = {.columns = size.columns, .rows = size.rows},
      .focused = true,
  };
  return compose_retained_frame(std::span(&pane, 1), {.columns = size.columns, .rows = size.rows},
                                frame, force_full);
}

} // namespace frame::render
