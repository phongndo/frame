#include "frame/terminal/terminal.hpp"
#include "image/png.hpp"

#include "terminal/terminal_impl.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ghostty/vt/kitty_render.h>
#include <optional>
#include <span>

#include <algorithm>
#include <atomic>

namespace frame::vt {
namespace {
struct Pixels {
  const GhosttyAllocator* allocator;
  std::span<std::byte> bytes;
  std::uint8_t* pointer{nullptr};
};
auto allocate_pixels(void* const context, const std::size_t size) noexcept -> std::span<std::byte> {
  auto& pixels = *static_cast<Pixels*>(context);
  pixels.pointer = ghostty_alloc(pixels.allocator, size);
  if (pixels.pointer != nullptr) {
    pixels.bytes = std::as_writable_bytes(std::span(pixels.pointer, size));
  }
  return pixels.bytes;
}
auto decode_png([[maybe_unused]] void* userdata, const GhosttyAllocator* allocator,
                const std::uint8_t* data, const std::size_t length,
                GhosttySysImage* output) noexcept -> bool {
  Pixels pixels{.allocator = allocator, .bytes = {}};
  const auto decoded =
      image::decode_png(std::as_bytes(std::span(data, length)), allocate_pixels, &pixels);
  if (!decoded) {
    ghostty_free(allocator, pixels.pointer, pixels.bytes.size());
    return false;
  }
  *output = {.width = decoded->width,
             .height = decoded->height,
             // The allocator returned the exact raw RGBA buffer adopted by Ghostty.
             // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
             .data = reinterpret_cast<std::uint8_t*>(pixels.bytes.data()),
             .data_len = pixels.bytes.size()};
  return true;
}
} // namespace
namespace detail {
auto register_png_decoder() noexcept -> GhosttyResult {
  // Configured once before the first Terminal can parse input, not during rendering.
  static const auto result =
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, reinterpret_cast<const void*>(&decode_png));
  return result;
}
auto new_graphics_identity() noexcept -> std::uint64_t {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}
} // namespace detail

auto Terminal::graphics_identity() const noexcept -> std::uint64_t {
  return impl_->graphics_identity;
}
auto Terminal::graphics_generation() const noexcept -> std::expected<std::uint64_t, Error> {
  GhosttyKittyGraphics storage = nullptr;
  std::uint64_t generation = 0;
  if (ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,
                           static_cast<void*>(&storage)) != GHOSTTY_SUCCESS ||
      ghostty_kitty_graphics_get(storage, GHOSTTY_KITTY_GRAPHICS_DATA_GENERATION, &generation) !=
          GHOSTTY_SUCCESS) {
    return std::unexpected(Error::invalid_state);
  }
  return generation;
}
// Every borrowed projection is validated before crossing the adapter boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::graphics(const std::span<GraphicPlacement> placements) noexcept
    -> std::expected<std::size_t, Error> {
  GhosttyKittyGraphics storage = nullptr;
  std::uint64_t generation = 0;
  if (ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,
                           static_cast<void*>(&storage)) != GHOSTTY_SUCCESS ||
      ghostty_kitty_graphics_get(storage, GHOSTTY_KITTY_GRAPHICS_DATA_GENERATION, &generation) !=
          GHOSTTY_SUCCESS) {
    return std::unexpected(Error::invalid_state);
  }
  if (generation == 0) {
    return 0;
  }
  std::array<GhosttyKittyRenderPlacement, 256> native{};
  std::size_t native_count = 0;
  const auto result = ghostty_terminal_kitty_render(
      impl_->terminal, native.data(), std::min(native.size(), placements.size()), &native_count);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  std::size_t count = 0;
  for (const auto& info : std::span(native).first(native_count)) {
    const auto* const image = ghostty_kitty_graphics_image(storage, info.image_id);
    if (image == nullptr) {
      return std::unexpected(Error::invalid_state);
    }
    GraphicPlacement placement;
    placement.image_id = info.image_id;
    const std::uint8_t* data = nullptr;
    std::size_t length = 0;
    GhosttyKittyImageFormat format = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
    if (ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_GENERATION,
                                         &placement.image_generation) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,
                                         static_cast<void*>(&data)) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN, &length) !=
            GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_WIDTH,
                                         &placement.image_width) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_HEIGHT,
                                         &placement.image_height) != GHOSTTY_SUCCESS ||
        ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_IMAGE_DATA_FORMAT, &format) !=
            GHOSTTY_SUCCESS) {
      return std::unexpected(Error::invalid_state);
    }
    if (data == nullptr || length == 0) {
      continue;
    }
    switch (format) {
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGB:
      placement.channels = 3;
      break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_RGBA:
      placement.channels = 4;
      break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY:
      placement.channels = 1;
      break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA:
      placement.channels = 2;
      break;
    case GHOSTTY_KITTY_IMAGE_FORMAT_MAX_VALUE:
    case GHOSTTY_KITTY_IMAGE_FORMAT_PNG:
      return std::unexpected(Error::invalid_state);
    }
    if (length != static_cast<std::uint64_t>(placement.image_width) * placement.image_height *
                      placement.channels ||
        length > image::rgba_bytes_max) {
      return std::unexpected(Error::limit_exceeded);
    }
    placement.pixels = std::as_bytes(std::span(data, length));
    // Native placeholder rounding can land on the texture edge. Match the native
    // renderer's clamp-to-edge sampling without exposing an out-of-range pixel view.
    placement.source_x = std::min(info.source_x, placement.image_width - 1U);
    placement.source_y = std::min(info.source_y, placement.image_height - 1U);
    placement.source_width =
        std::min(std::max(1U, info.source_width), placement.image_width - placement.source_x);
    placement.source_height =
        std::min(std::max(1U, info.source_height), placement.image_height - placement.source_y);
    placement.pixel_width = info.pixel_width;
    placement.pixel_height = info.pixel_height;
    placement.column = info.column;
    placement.row = info.row;
    placement.offset_x = info.offset_x;
    placement.offset_y = info.offset_y;
    placement.z = info.z;
    placements.subspan(count++, 1).front() = placement;
  }
  return count;
}
auto Terminal::tick_graphics(const std::uint64_t now_ms) noexcept
    -> std::expected<std::optional<std::uint64_t>, Error> {
  std::uint64_t delay = 0;
  const auto result = ghostty_terminal_kitty_tick(impl_->terminal, now_ms, &delay);
  if (result == GHOSTTY_NO_VALUE) {
    return std::nullopt;
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return delay;
}
} // namespace frame::vt
