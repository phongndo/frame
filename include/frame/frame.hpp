#ifndef FRAME_FRAME_HPP
#define FRAME_FRAME_HPP

#include <cstdint>
#include <span>
#include <string_view>

namespace frame {

[[nodiscard]] auto greeting() noexcept -> std::string_view;
[[nodiscard]] auto ghostty_version() noexcept -> std::span<const std::uint8_t>;
[[nodiscard]] auto zstd_version() noexcept -> std::string_view;

} // namespace frame

#endif // FRAME_FRAME_HPP
