#ifndef FRAME_IMAGE_PNG_HPP
#define FRAME_IMAGE_PNG_HPP
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace frame::image {
inline constexpr std::size_t rgba_bytes_max = std::size_t{8} * 1024U * 1024U;
struct Size {
  std::uint32_t width;
  std::uint32_t height;
};
using Allocate = std::span<std::byte> (*)(void*, std::size_t) noexcept;
// Decode only bounded PNGs. Allocate is called at most once after validated dimensions. The
// caller owns its allocation on success AND failure; the decoder retains no data or callbacks.
[[nodiscard]] auto decode_png(std::span<const std::byte> encoded, Allocate allocate,
                              void* context) noexcept -> std::optional<Size>;
} // namespace frame::image
#endif
