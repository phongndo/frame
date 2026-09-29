#include "image/png.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <png.h>
#include <span>

namespace frame::image {
namespace {
struct Image final {
  png_image value{};
  Image() noexcept { value.version = PNG_IMAGE_VERSION; }
  ~Image() { png_image_free(&value); }
  Image(const Image&) = delete;
  auto operator=(const Image&) -> Image& = delete;
  Image(Image&&) = delete;
  auto operator=(Image&&) -> Image& = delete;
};
} // namespace
auto decode_png(const std::span<const std::byte> encoded, const Allocate allocate,
                void* const context) noexcept -> std::optional<Size> {
  if (encoded.empty() || encoded.size() > rgba_bytes_max || allocate == nullptr) {
    return std::nullopt;
  }
  Image image;
  if (png_image_begin_read_from_memory(&image.value, encoded.data(), encoded.size()) == 0) {
    return std::nullopt;
  }
  const auto width = image.value.width;
  const auto height = image.value.height;
  const auto bytes = static_cast<std::uint64_t>(width) * height * 4U;
  if (width == 0 || height == 0 || width > 4096 || height > 4096 || bytes > rgba_bytes_max) {
    return std::nullopt;
  }
  image.value.format = PNG_FORMAT_RGBA;
  const auto pixels = allocate(context, static_cast<std::size_t>(bytes));
  if (pixels.size() != bytes ||
      png_image_finish_read(&image.value, nullptr, pixels.data(), 0, nullptr) == 0) {
    return std::nullopt;
  }
  return Size{.width = width, .height = height};
}
} // namespace frame::image
