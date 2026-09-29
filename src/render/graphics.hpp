#ifndef FRAME_RENDER_GRAPHICS_HPP
#define FRAME_RENDER_GRAPHICS_HPP
#include "render/scene.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

namespace frame::render {
enum class GraphicsError : std::uint8_t { terminal, capacity, allocation };
// Attachment-owned presentation cache. Canonical pixels/placements stay in Terminal; only image
// generations, upload progress, and clipped presentation geometry survive a composition call.
// A frame emits at most 64 KiB of graphics, including bounded direct-medium upload chunks.
class GraphicsProjection final {
public:
  GraphicsProjection() noexcept;
  ~GraphicsProjection();
  GraphicsProjection(GraphicsProjection&&) noexcept;
  auto operator=(GraphicsProjection&&) noexcept -> GraphicsProjection&;
  GraphicsProjection(const GraphicsProjection&) = delete;
  auto operator=(const GraphicsProjection&) -> GraphicsProjection& = delete;
  [[nodiscard]] auto append(Scene scene, std::uint16_t status_rows, std::span<std::byte> output,
                            bool force_full = false, bool repaint = false) noexcept
      -> std::expected<std::size_t, GraphicsError>;
  [[nodiscard]] auto pending() const noexcept -> bool;
  [[nodiscard]] auto deadline() const noexcept
      -> std::optional<std::chrono::steady_clock::time_point>;
  [[nodiscard]] auto wake(std::chrono::steady_clock::time_point now) noexcept -> bool;
  void reset() noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace frame::render
#endif
