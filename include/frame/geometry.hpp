#ifndef FRAME_GEOMETRY_HPP
#define FRAME_GEOMETRY_HPP

#include <cstdint>

namespace frame {

struct PaneRectangle final {
  std::uint16_t column{0};
  std::uint16_t row{0};
  std::uint16_t columns{0};
  std::uint16_t rows{0};

  friend constexpr auto operator==(const PaneRectangle&, const PaneRectangle&) noexcept
      -> bool = default;
};

} // namespace frame

#endif // FRAME_GEOMETRY_HPP
