#ifndef LEMMA_RENDER_FRAME_BUFFER_HPP
#define LEMMA_RENDER_FRAME_BUFFER_HPP

#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "render/pane_composition.hpp"
#include "render/scene.hpp"
#include "render/status_line.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace lemma::render {

inline constexpr std::size_t frame_bytes_max = limits::frame_transaction_bytes_max;
inline constexpr std::size_t frame_bytes_min = std::size_t{64} * 1'024U;
inline constexpr std::size_t frame_bytes_per_viewport_cell = vt::pane_ansi_bytes_per_cell_max;
// Covers outer synchronized-update framing, focused cursor/mode state, compositor control bytes,
// and one outer window title appended after composition, so a worst-case frame still has room for
// it.
inline constexpr std::size_t frame_fixed_overhead_bytes =
    (std::size_t{4} * 1'024U) + limits::outer_title_frame_bytes_max;

static_assert(frame_bytes_min < frame_bytes_max);
static_assert(frame_fixed_overhead_bytes < frame_bytes_min);

enum class FrameCapacityError : unsigned char {
  invalid_viewport,
  arithmetic_overflow,
  transaction_too_large,
};

[[nodiscard]] auto frame_capacity_for_viewport(Viewport viewport) noexcept
    -> std::expected<std::size_t, FrameCapacityError>;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
using FrameStorage = std::unique_ptr<std::byte[]>;
using FrameAllocationOperation = FrameStorage (*)(void* context, std::size_t bytes) noexcept;

// One daemon-owned budget bounds the sum of retained backing storage across attached and
// pending-attached sessions. The reactor is the sole caller, so accounting needs no
// synchronization.
class FrameCapacityBudget final {
public:
  explicit FrameCapacityBudget(
      std::size_t bytes_max = limits::frame_retained_bytes_aggregate_max) noexcept
      : bytes_max_(bytes_max) {}

  FrameCapacityBudget(const FrameCapacityBudget&) = delete;
  auto operator=(const FrameCapacityBudget&) -> FrameCapacityBudget& = delete;
  FrameCapacityBudget(FrameCapacityBudget&&) = delete;
  auto operator=(FrameCapacityBudget&&) -> FrameCapacityBudget& = delete;
  ~FrameCapacityBudget() = default;

  [[nodiscard]] auto reserve(std::size_t bytes) noexcept -> bool;
  void release(std::size_t bytes) noexcept;

  [[nodiscard]] auto used() const noexcept -> std::size_t { return used_; }
  [[nodiscard]] auto maximum() const noexcept -> std::size_t { return bytes_max_; }

private:
  std::size_t bytes_max_;
  std::size_t used_{0};
};

// One explicit RAII owner for attached-client frame bytes. Capacity can change only when the core
// calls prepare() at attach or resize; composition and flushing receive non-owning spans.
class FrameBuffer final {
public:
  explicit FrameBuffer(FrameAllocationOperation allocate = nullptr,
                       void* allocation_context = nullptr) noexcept;

  FrameBuffer(const FrameBuffer&) = delete;
  auto operator=(const FrameBuffer&) -> FrameBuffer& = delete;
  FrameBuffer(FrameBuffer&&) = delete;
  auto operator=(FrameBuffer&&) -> FrameBuffer& = delete;
  ~FrameBuffer();

  // A budget must be bound before the first prepare and remains bound for this buffer's lifetime.
  void bind_capacity_budget(FrameCapacityBudget& budget) noexcept;
  [[nodiscard]] auto prepare(Viewport viewport, std::size_t preserve_bytes = 0) noexcept -> bool;
  void release() noexcept;

  [[nodiscard]] auto writable() noexcept -> std::span<std::byte>;
  [[nodiscard]] auto readable(std::size_t bytes) const noexcept -> std::span<const std::byte>;
  [[nodiscard]] auto capacity() const noexcept -> std::size_t { return capacity_; }

private:
  FrameStorage storage_;
  std::size_t capacity_{0};
  FrameAllocationOperation allocate_{nullptr};
  void* allocation_context_{nullptr};
  FrameCapacityBudget* capacity_budget_{nullptr};
};

static_assert(sizeof(FrameBuffer) <= 5U * sizeof(void*));

// Composition only fills one retained bounded frame. Descriptor progress is owned by the core.
[[nodiscard]] auto compose_retained_scene(
    Scene scene, Viewport viewport, FrameBuffer& frame, bool force_full, StatusLine status = {},
    std::optional<OuterModeProjection> previous_outer_modes = std::nullopt,
    MessageView message_view = {}, GraphicsProjection* graphics = nullptr) noexcept
    -> std::expected<CompositionResult, CompositionError>;

[[nodiscard]] auto compose_retained_frame(
    std::span<const PaneSurface> panes, Viewport viewport, FrameBuffer& frame, bool force_full,
    StatusLine status = {}, std::optional<OuterModeProjection> previous_outer_modes = std::nullopt,
    MessageView message_view = {}) noexcept -> std::expected<CompositionResult, CompositionError>;

[[nodiscard]] auto compose_retained_single_pane(vt::Terminal& terminal, FrameBuffer& frame,
                                                bool force_full = false) noexcept
    -> std::expected<CompositionResult, CompositionError>;

} // namespace lemma::render

#endif // LEMMA_RENDER_FRAME_BUFFER_HPP
