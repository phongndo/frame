#ifndef FRAME_ID_HPP
#define FRAME_ID_HPP

#include "frame/assert.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace frame {

template <typename Tag> class GenerationalId final {
public:
  constexpr GenerationalId() noexcept = default;

  [[nodiscard]] static constexpr std::optional<GenerationalId>
  try_from_parts(const std::uint32_t slot, const std::uint32_t generation) noexcept {
    if (slot == invalid_slot || generation == 0) {
      return std::nullopt;
    }
    return GenerationalId(slot, generation);
  }

  [[nodiscard]] static constexpr GenerationalId
  from_parts(const std::uint32_t slot, const std::uint32_t generation) noexcept {
    const std::optional<GenerationalId> id = try_from_parts(slot, generation);
    FRAME_ASSERT(id.has_value());
    return *id;
  }

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return slot_ != invalid_slot && generation_ != 0;
  }

  [[nodiscard]] constexpr std::uint32_t slot() const noexcept { return slot_; }
  [[nodiscard]] constexpr std::uint32_t generation() const noexcept { return generation_; }

  friend constexpr bool operator==(const GenerationalId&, const GenerationalId&) noexcept = default;

private:
  static constexpr std::uint32_t invalid_slot = std::numeric_limits<std::uint32_t>::max();

  constexpr GenerationalId(const std::uint32_t slot, const std::uint32_t generation) noexcept
      : slot_(slot), generation_(generation) {}

  std::uint32_t slot_{invalid_slot};
  std::uint32_t generation_{0};
};

struct SessionIdTag final {};
struct TabIdTag final {};
struct PaneIdTag final {};
struct AttachmentIdTag final {};
struct ConnectionIdTag final {};
struct ExtensionGenerationIdTag final {};
struct SurfaceIdTag final {};

using SessionId = GenerationalId<SessionIdTag>;
using TabId = GenerationalId<TabIdTag>;
using PaneId = GenerationalId<PaneIdTag>;
using AttachmentId = GenerationalId<AttachmentIdTag>;
using ConnectionId = GenerationalId<ConnectionIdTag>;
using ExtensionGenerationId = GenerationalId<ExtensionGenerationIdTag>;
using SurfaceId = GenerationalId<SurfaceIdTag>;

} // namespace frame

#endif // FRAME_ID_HPP
