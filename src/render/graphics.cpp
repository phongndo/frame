#include "render/graphics.hpp"
#include "frame/assert.hpp"
#include "frame/terminal/terminal.hpp"
#include "render/scene.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <new>

namespace frame::render {
namespace {
constexpr std::size_t image_limit = 256;
constexpr std::size_t raster_bytes_limit = std::size_t{32} * 1024U * 1024U;
// Only non-cell-aligned scaling needs an attachment-local raster. Direct and
// naturally sized images keep their canonical bytes, shared by every placement.
struct Raster {
  std::uint32_t x{0}, y{0}, width{0}, height{0};
  std::uint32_t scaled_width{0}, scaled_height{0}, left{0}, top{0};
  auto operator==(const Raster&) const noexcept -> bool = default;
};
struct ImageKey {
  std::uint64_t generation{0};
  Raster raster{};
  std::uint32_t width{0}, height{0};
  auto operator==(const ImageKey&) const noexcept -> bool = default;
};
constexpr std::size_t placement_limit = 256;
constexpr std::size_t frame_limit = std::size_t{64} * 1024U;
struct Rectangle {
  std::int64_t left, top, right, bottom;
};
// Visit only painted cells overlapping the image (or a frozen Pane). Returning false stops early,
// either because the caller found occlusion or exhausted its bounded clipping storage.
template <typename Visit>
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto visit_coverage(const std::span<const GridSurface> grids, const Rectangle bounds,
                    Visit visit) noexcept -> bool {
  for (const auto& grid : grids) {
    const auto& rectangle = grid.rectangle;
    const Rectangle overlap{
        .left = std::max<std::int64_t>(bounds.left, rectangle.column),
        .top = std::max<std::int64_t>(bounds.top, rectangle.row),
        .right = std::min<std::int64_t>(bounds.right, rectangle.column + rectangle.columns),
        .bottom = std::min<std::int64_t>(bounds.bottom, rectangle.row + rectangle.rows)};
    if (overlap.left >= overlap.right || overlap.top >= overlap.bottom) {
      continue;
    }
    if (grid.opaque) {
      if (!visit(overlap)) {
        return false;
      }
      continue;
    }
    const auto end = std::min<std::int64_t>(overlap.bottom - rectangle.row, grid.grid->rows());
    for (auto row = overlap.top - rectangle.row; row < end; ++row) {
      for (const auto& run : grid.grid->row_runs(static_cast<std::uint16_t>(row))) {
        const auto left = std::max<std::int64_t>(overlap.left, rectangle.column + run.column);
        const auto right =
            std::min<std::int64_t>(overlap.right, rectangle.column + run.column + run.columns);
        if (left < right && !visit(Rectangle{.left = left,
                                             .top = rectangle.row + row,
                                             .right = right,
                                             .bottom = rectangle.row + row + 1})) {
          return false;
        }
      }
    }
  }
  return true;
}
struct Placement {
  std::uint64_t owner{0};
  std::uint32_t image_id{0};
  ImageKey key{};
  std::uint32_t column{0}, row{0}, x{0}, y{0}, width{0}, height{0};
  std::uint32_t columns{0}, rows{0}, offset_x{0}, offset_y{0};
  std::int32_t z{0};
  auto operator==(const Placement&) const noexcept -> bool = default;
};
struct Image {
  ImageKey key{};
  std::span<const std::byte> pixels{}; // NOLINT(readability-redundant-member-init)
  std::uint32_t source_width{0};
  std::uint8_t channels{4};
};
struct Resident {
  ImageKey key{};
  std::uint32_t id{0}, width{0}, height{0};
  std::size_t sent{0};
  std::uint8_t channels{4};
  bool complete{false};
};
class Writer final {
public:
  explicit Writer(const std::span<std::byte> output)
      : output_(output.first(std::min(output.size(), frame_limit))) {}
  auto room(const std::size_t bytes) noexcept -> bool {
    if (output_.size() - used_ < bytes + 4U) {
      return false;
    }
    if (used_ == 0) {
      text("\x1b"
           "7");
    }
    return true;
  }
  void text(const std::string_view value) noexcept {
    FRAME_ASSERT(value.size() <= output_.size() - used_);
    std::memcpy(output_.subspan(used_).data(), value.data(), value.size());
    used_ += value.size();
  }
  template <typename T> void number(const T value) noexcept {
    std::array<char, 32> buffer{};
    const auto encoded = std::to_chars(buffer.begin(), buffer.end(), value);
    FRAME_ASSERT(encoded.ec == std::errc{});
    text({buffer.data(), static_cast<std::size_t>(encoded.ptr - buffer.data())});
  }
  void erase(const std::uint32_t id, const bool data) noexcept {
    text(data ? "\x1b_Ga=d,d=I,q=2,i=" : "\x1b_Ga=d,d=i,q=2,i=");
    number(id);
    text("\x1b\\");
  }
  void place(const Placement& placement, const std::uint32_t image, const std::size_t id) noexcept {
    text("\x1b[");
    number(placement.row + 1U);
    text(";");
    number(placement.column + 1U);
    text("H");
    text("\x1b_Ga=p,q=2,C=1,i=");
    number(image);
    text(",p=");
    number(id + 1U);
    text(",x=");
    number(placement.x);
    text(",y=");
    number(placement.y);
    text(",w=");
    number(placement.width);
    text(",h=");
    number(placement.height);
    text(",c=");
    number(placement.columns);
    text(",r=");
    number(placement.rows);
    text(",X=");
    number(placement.offset_x);
    text(",Y=");
    number(placement.offset_y);
    text(",z=");
    // Preserve native z/image-ID ordering without exposing child IDs in the outer namespace.
    const auto rank = static_cast<std::int32_t>(id);
    auto z = rank;
    if (placement.z < std::numeric_limits<std::int32_t>::min() / 2) {
      z = std::numeric_limits<std::int32_t>::min() + rank;
    } else if (placement.z < 0) {
      z = -1024 + rank;
    }
    number(z);
    text("\x1b\\");
  }
  // One packet owns format conversion, sampling, and multipart publication.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void upload(Resident& resident, const Image& image) noexcept {
    constexpr std::string_view digits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto channels = image.channels < 3 ? std::uint8_t{4} : image.channels;
    const auto total = static_cast<std::size_t>(image.key.width) * image.key.height * channels;
    const auto count = std::min(std::size_t{3072}, total - resident.sent);
    if (resident.sent == 0) {
      text("\x1b_Ga=t,q=2,f=");
      number(channels == 3 ? 24 : 32);
      text(",s=");
      number(image.key.width);
      text(",v=");
      number(image.key.height);
      text(",i=");
      number(resident.id);
      text(",m=");
    } else {
      text("\x1b_Gq=2,m=");
    }
    const bool last = resident.sent + count == total;
    text(last ? "0;" : "1;");
    const auto byte = [&](const std::size_t offset) -> std::uint32_t {
      auto pixel = offset / channels;
      const auto channel = offset % channels;
      const auto& raster = image.key.raster;
      if (raster.scaled_width != 0) {
        // Sample at pixel centers; each bounded upload packet does its own work.
        // No resampled pixel buffer or borrowed source survives this frame.
        const auto x = raster.x + (((((pixel % image.key.width) + raster.left) * 2U) + 1U) *
                                   raster.width / (2ULL * raster.scaled_width));
        const auto y = raster.y + (((((pixel / image.key.width) + raster.top) * 2U) + 1U) *
                                   raster.height / (2ULL * raster.scaled_height));
        pixel = (y * image.source_width) + x;
      }
      if (image.channels >= 3) {
        return std::to_integer<std::uint32_t>(
            image.pixels.subspan((pixel * channels) + channel, 1).front());
      }
      if (channel == 3U) {
        return image.channels == 1 ? 255U
                                   : std::to_integer<std::uint32_t>(
                                         image.pixels.subspan((pixel * 2U) + 1U, 1).front());
      }
      return std::to_integer<std::uint32_t>(
          image.pixels.subspan(pixel * image.channels, 1).front());
    };
    for (std::size_t i = 0; i < count; i += 3U) {
      const auto remaining = count - i;
      const auto value = (byte(resident.sent + i) << 16U) |
                         (remaining > 1U ? byte(resident.sent + i + 1U) << 8U : 0U) |
                         (remaining > 2U ? byte(resident.sent + i + 2U) : 0U);
      const std::array encoded{digits.at((value >> 18U) & 63U), digits.at((value >> 12U) & 63U),
                               remaining > 1U ? digits.at((value >> 6U) & 63U) : '=',
                               remaining > 2U ? digits.at(value & 63U) : '='};
      text({encoded.data(), encoded.size()});
    }
    text("\x1b\\");
    resident.sent += count;
    resident.complete = last;
    if (last) {
      // An invisible reference keeps pixels resident across compositor ED/EL operations.
      text("\x1b_Ga=p,U=1,q=2,i=");
      number(resident.id);
      text(",p=4294967295,c=1,r=1\x1b\\");
    }
  }
  auto finish() noexcept -> std::size_t {
    if (used_ != 0) {
      text("\x1b"
           "8");
    }
    return used_;
  }

private:
  std::span<std::byte> output_;
  std::size_t used_{0};
};
} // namespace

struct GraphicsProjection::State {
  std::array<Resident, image_limit> residents{};
  std::array<Placement, placement_limit> plan{}, next{};
  // Scratch belongs to this composition call; BorrowGuard clears every pixel view on all exits.
  std::array<Image, image_limit> images{};
  std::array<vt::GraphicPlacement, placement_limit> snapshot{};
  std::array<Rectangle, placement_limit> coverage{};
  std::size_t plan_size{0}, next_size{0}, image_count{0}, clear_cursor{image_limit},
      place_cursor{0};
  std::uint32_t next_id{0x464D0000U};
  bool pending{false};
  std::optional<std::chrono::steady_clock::time_point> animation_deadline;

  auto resident(const ImageKey& key) noexcept -> Resident* {
    for (auto& item : residents) {
      if (item.key == key) {
        return &item;
      }
    }
    return nullptr;
  }
  auto image(const ImageKey& key) noexcept -> const Image* {
    for (const auto& item : std::span(images).first(image_count)) {
      if (item.key == key) {
        return &item;
      }
    }
    return nullptr;
  }
  auto add_image(const Image& value) noexcept -> bool {
    if (image(value.key) != nullptr) {
      return true;
    }
    std::uint64_t bytes = static_cast<std::uint64_t>(value.key.width) * value.key.height * 4U;
    for (const auto& item : std::span(images).first(image_count)) {
      bytes += static_cast<std::uint64_t>(item.key.width) * item.key.height * 4U;
    }
    if (image_count == images.size() || bytes > raster_bytes_limit) {
      return false;
    }
    images.at(image_count++) = value;
    return true;
  }
  auto add_placement(const Placement& value) noexcept -> bool {
    if (next_size == next.size()) {
      return false;
    }
    next.at(next_size++) = value;
    return true;
  }
  // Fixed-capacity coverage subtraction and projection have one capacity rejection boundary.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  auto collect(const Scene scene, const std::uint16_t status_rows) noexcept
      -> std::expected<void, GraphicsError> {
    next_size = 0;
    image_count = 0;
    for (const auto& pane : scene.panes) {
      const auto owner = pane.terminal->graphics_identity();
      if (pane.presentation_suppressed) {
        if (std::ranges::none_of(std::span(plan).first(plan_size),
                                 [&](const Placement& old) { return old.owner == owner; })) {
          continue;
        }
        const Rectangle bounds{.left = pane.rectangle.column,
                               .top = pane.rectangle.row,
                               .right = pane.rectangle.column + pane.rectangle.columns,
                               .bottom = pane.rectangle.row + pane.rectangle.rows};
        if (!visit_coverage(scene.grids, bounds, [](const Rectangle&) { return false; })) {
          continue;
        }
        for (const auto& old : std::span(plan).first(plan_size)) {
          if (old.owner != owner) {
            continue;
          }
          const auto* held = resident(old.key);
          if (held == nullptr || !add_placement(old) ||
              !add_image({.key = held->key, .channels = held->channels})) {
            return std::unexpected(GraphicsError::capacity);
          }
        }
        continue;
      }
      const auto count = pane.terminal->graphics(snapshot);
      if (!count) {
        return std::unexpected(GraphicsError::terminal);
      }
      const auto cell = pane.terminal->size();
      if (cell.cell_width_px == 0 || cell.cell_height_px == 0) {
        continue;
      }
      const auto cw = static_cast<std::int64_t>(cell.cell_width_px);
      const auto ch = static_cast<std::int64_t>(cell.cell_height_px);
      for (const auto& source : std::span(snapshot).first(*count)) {
        const auto left = (static_cast<std::int64_t>(source.column) * cw) + source.offset_x;
        const auto top = (static_cast<std::int64_t>(source.row) * ch) + source.offset_y;
        const Rectangle bounds{
            .left = std::max(std::int64_t{0}, left),
            .top = std::max(std::int64_t{0}, top),
            .right = std::min(static_cast<std::int64_t>(pane.rectangle.columns) * cw,
                              left + source.pixel_width),
            .bottom = std::min(static_cast<std::int64_t>(pane.rectangle.rows) * ch,
                               top + source.pixel_height)};
        if (bounds.left >= bounds.right || bounds.top >= bounds.bottom) {
          continue;
        }
        // Coverage is scratch for this visible image, never a limit on unrelated Surface text.
        const Rectangle cells{.left = pane.rectangle.column + (bounds.left / cw),
                              .top = pane.rectangle.row + (bounds.top / ch),
                              .right = pane.rectangle.column + ((bounds.right + cw - 1) / cw),
                              .bottom = pane.rectangle.row + ((bounds.bottom + ch - 1) / ch)};
        std::size_t covered = 0;
        if (!visit_coverage(scene.grids, cells, [&](const Rectangle area) {
              if (covered == coverage.size()) {
                return false;
              }
              coverage.at(covered++) = area;
              return true;
            })) {
          return std::unexpected(GraphicsError::capacity);
        }
        std::array<Rectangle, placement_limit> fragments{};
        fragments.at(0) = bounds;
        std::size_t fragment_count = 1;
        // Opaque rectangles and painted transparent runs own their presentation area.
        for (const auto& area : std::span(coverage).first(covered)) {
          const Rectangle cut{.left = (area.left - pane.rectangle.column) * cw,
                              .top = (area.top - pane.rectangle.row) * ch,
                              .right = (area.right - pane.rectangle.column) * cw,
                              .bottom = (area.bottom - pane.rectangle.row) * ch};
          const auto before = fragment_count;
          for (std::size_t i = 0; i < before; ++i) {
            const auto original = fragments.at(i);
            const Rectangle overlap{.left = std::max(original.left, cut.left),
                                    .top = std::max(original.top, cut.top),
                                    .right = std::min(original.right, cut.right),
                                    .bottom = std::min(original.bottom, cut.bottom)};
            if (overlap.left >= overlap.right || overlap.top >= overlap.bottom) {
              continue;
            }
            fragments.at(i) = {.left = 0, .top = 0, .right = 0, .bottom = 0};
            for (const auto piece : {Rectangle{.left = original.left,
                                               .top = original.top,
                                               .right = original.right,
                                               .bottom = overlap.top},
                                     Rectangle{.left = original.left,
                                               .top = overlap.bottom,
                                               .right = original.right,
                                               .bottom = original.bottom},
                                     Rectangle{.left = original.left,
                                               .top = overlap.top,
                                               .right = overlap.left,
                                               .bottom = overlap.bottom},
                                     Rectangle{.left = overlap.right,
                                               .top = overlap.top,
                                               .right = original.right,
                                               .bottom = overlap.bottom}}) {
              if (piece.left >= piece.right || piece.top >= piece.bottom) {
                continue;
              }
              if (fragment_count == fragments.size()) {
                return std::unexpected(GraphicsError::capacity);
              }
              fragments.at(fragment_count++) = piece;
            }
          }
          // Reclaim removed fragments before considering another Surface.
          std::size_t retained = 0;
          for (const auto piece : std::span(fragments).first(fragment_count)) {
            if (piece.left < piece.right && piece.top < piece.bottom) {
              fragments.at(retained++) = piece;
            }
          }
          fragment_count = retained;
        }
        if (fragment_count == 0) {
          continue;
        }
        for (const auto piece : std::span(fragments).first(fragment_count)) {
          const auto sx = static_cast<std::uint64_t>(piece.left - left) * source.source_width /
                          source.pixel_width;
          const auto sy = static_cast<std::uint64_t>(piece.top - top) * source.source_height /
                          source.pixel_height;
          const auto ex = ((static_cast<std::uint64_t>(piece.right - left) * source.source_width) +
                           source.pixel_width - 1U) /
                          source.pixel_width;
          const auto ey = ((static_cast<std::uint64_t>(piece.bottom - top) * source.source_height) +
                           source.pixel_height - 1U) /
                          source.pixel_height;
          Placement placed;
          placed.owner = owner;
          placed.image_id = source.image_id;
          placed.key = {.generation = source.image_generation,
                        .width = source.image_width,
                        .height = source.image_height};
          placed.column = pane.rectangle.column + static_cast<std::uint32_t>(piece.left / cw);
          placed.row =
              pane.rectangle.row + status_rows + static_cast<std::uint32_t>(piece.top / ch);
          placed.offset_x = static_cast<std::uint32_t>(piece.left % cw);
          placed.offset_y = static_cast<std::uint32_t>(piece.top % ch);
          placed.x = source.source_x + static_cast<std::uint32_t>(sx);
          placed.y = source.source_y + static_cast<std::uint32_t>(sy);
          placed.width = static_cast<std::uint32_t>(std::max(std::uint64_t{1}, ex - sx));
          placed.height = static_cast<std::uint32_t>(std::max(std::uint64_t{1}, ey - sy));
          const auto width = static_cast<std::uint32_t>(piece.right - piece.left);
          const auto height = static_cast<std::uint32_t>(piece.bottom - piece.top);
          const bool scaled = source.pixel_width != source.source_width ||
                              source.pixel_height != source.source_height;
          if (scaled && (piece.right % cw != 0 || piece.bottom % ch != 0)) {
            placed.key.raster = {.x = source.source_x,
                                 .y = source.source_y,
                                 .width = source.source_width,
                                 .height = source.source_height,
                                 .scaled_width = source.pixel_width,
                                 .scaled_height = source.pixel_height,
                                 .left = static_cast<std::uint32_t>(piece.left - left),
                                 .top = static_cast<std::uint32_t>(piece.top - top)};
            placed.key.width = width;
            placed.key.height = height;
            placed.x = 0;
            placed.y = 0;
            placed.width = width;
            placed.height = height;
          } else if (scaled) {
            placed.columns = (width + placed.offset_x) / static_cast<std::uint32_t>(cw);
            placed.rows = (height + placed.offset_y) / static_cast<std::uint32_t>(ch);
          }
          if (!add_image({.key = placed.key,
                          .pixels = source.pixels,
                          .source_width = source.image_width,
                          .channels = source.channels})) {
            return std::unexpected(GraphicsError::capacity);
          }
          placed.z = source.z;
          if (!add_placement(placed)) {
            return std::unexpected(GraphicsError::capacity);
          }
        }
      }
    }
    return {};
  }
};

GraphicsProjection::GraphicsProjection() noexcept = default;
GraphicsProjection::~GraphicsProjection() = default;
GraphicsProjection::GraphicsProjection(GraphicsProjection&&) noexcept = default;
auto GraphicsProjection::operator=(GraphicsProjection&&) noexcept -> GraphicsProjection& = default;
void GraphicsProjection::reset() noexcept { state_.reset(); }
auto GraphicsProjection::pending() const noexcept -> bool {
  return state_ != nullptr && state_->pending;
}
auto GraphicsProjection::deadline() const noexcept
    -> std::optional<std::chrono::steady_clock::time_point> {
  return state_ != nullptr && !state_->pending ? state_->animation_deadline : std::nullopt;
}
auto GraphicsProjection::wake(const std::chrono::steady_clock::time_point now) noexcept -> bool {
  const auto next = deadline();
  if (!next || now < *next) {
    return false;
  }
  state_->animation_deadline.reset();
  return true;
}

// Keep resumable erase/upload/place publication together; all borrowed spans end on return.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto GraphicsProjection::append(const Scene scene, const std::uint16_t status_rows,
                                const std::span<std::byte> output, const bool force_full,
                                const bool repaint) noexcept
    -> std::expected<std::size_t, GraphicsError> {
  if (state_ == nullptr) {
    bool used = false;
    for (const auto& pane : scene.panes) {
      const auto generation = pane.terminal->graphics_generation();
      if (!generation) {
        return std::unexpected(GraphicsError::terminal);
      }
      used = used || *generation != 0;
    }
    if (!used) {
      return 0;
    }
    try {
      state_ = std::make_unique<State>();
    } catch (const std::bad_alloc&) {
      // Composition boundary: cache allocation failure rejects the frame with a typed error.
      return std::unexpected(GraphicsError::allocation);
    }
  }
  auto& state = *state_;
  struct BorrowGuard {
    explicit BorrowGuard(State& value) noexcept : state(&value) {}
    BorrowGuard(const BorrowGuard&) = delete;
    auto operator=(const BorrowGuard&) -> BorrowGuard& = delete;
    BorrowGuard(BorrowGuard&&) = delete;
    auto operator=(BorrowGuard&&) -> BorrowGuard& = delete;
    State* state;
    ~BorrowGuard() {
      for (auto& item : state->images) {
        item.pixels = {};
      }
      for (auto& item : state->snapshot) {
        item.pixels = {};
      }
    }
  } guard{state};
  if (!state.pending) {
    state.animation_deadline.reset();
    const auto now = std::chrono::steady_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    for (const auto& pane : scene.panes) {
      if (pane.presentation_suppressed) {
        continue;
      }
      const auto delay = pane.terminal->tick_graphics(static_cast<std::uint64_t>(milliseconds));
      if (!delay) {
        return std::unexpected(GraphicsError::terminal);
      }
      if (!*delay) {
        continue;
      }
      const auto next = now + std::chrono::milliseconds(
                                  std::clamp(**delay, std::uint64_t{16}, std::uint64_t{60'000}));
      if (!state.animation_deadline || next < *state.animation_deadline) {
        state.animation_deadline = next;
      }
    }
  }
  const auto collected = state.collect(scene, status_rows);
  if (!collected) {
    return std::unexpected(collected.error());
  }
  std::ranges::sort(std::span(state.next).first(state.next_size),
                    [](const Placement& left, const Placement& right) {
                      if (left.z != right.z) {
                        return left.z < right.z;
                      }
                      if (left.image_id != right.image_id) {
                        return left.image_id < right.image_id;
                      }
                      return left.owner < right.owner;
                    });
  Writer writer(output);
  if (repaint) {
    state.place_cursor = 0;
  }
  state.pending = true;
  const auto finish = [&]() -> std::expected<std::size_t, GraphicsError> {
    return writer.finish();
  };
  const bool changed =
      force_full || !std::ranges::equal(std::span(state.plan).first(state.plan_size),
                                        std::span(state.next).first(state.next_size));
  // Data chunks belong to a single upload. Placement/deletion commands are independent, but
  // retiring its source must terminate the upload before another image starts.
  for (auto& item : state.residents) {
    if (item.key.generation == 0) {
      continue;
    }
    const bool removed = state.image(item.key) == nullptr;
    if (item.sent > 0 && !item.complete && removed) {
      if (!writer.room(128)) {
        return finish();
      }
      writer.text("\x1b_Gq=2,m=0;\x1b\\");
      writer.erase(item.id, true);
      item.sent = 0;
    }
    if (removed) {
      if (!writer.room(64)) {
        return finish();
      }
      writer.erase(item.id, true);
      item = {};
    }
  }
  for (const auto& image : std::span(state.images).first(state.image_count)) {
    if (state.resident(image.key) != nullptr) {
      continue;
    }
    auto* empty = std::ranges::find_if(
        state.residents, [](const Resident& item) { return item.key.generation == 0; });
    if (empty == state.residents.end() ||
        state.next_id == std::numeric_limits<std::uint32_t>::max()) {
      return std::unexpected(GraphicsError::capacity);
    }
    *empty = {.key = image.key,
              .id = state.next_id++,
              .width = image.key.width,
              .height = image.key.height,
              .channels = image.channels};
  }
  if (changed) {
    std::ranges::copy(std::span(state.next).first(state.next_size), state.plan.begin());
    state.plan_size = state.next_size;
    state.clear_cursor = 0;
    state.place_cursor = 0;
  }
  if (state.clear_cursor == 0) {
    if (!writer.room(64)) {
      return finish();
    }
    // Frame owns the alternate screen. Keep invisible references while replacing visible images.
    writer.text("\x1b_Ga=d,d=a,q=2\x1b\\");
    state.clear_cursor = image_limit;
  }
  for (; state.place_cursor < state.plan_size; ++state.place_cursor) {
    const auto& placed = state.plan.at(state.place_cursor);
    const auto* item = state.resident(placed.key);
    FRAME_ASSERT(item != nullptr);
    if (!item->complete) {
      continue;
    }
    if (!writer.room(256)) {
      return finish();
    }
    writer.place(placed, item->id, state.place_cursor);
  }
  const auto upload_and_place = [&](Resident& item) {
    const auto* image = state.image(item.key);
    FRAME_ASSERT(image != nullptr);
    if (image->pixels.empty()) {
      state.pending = false;
      return false;
    }
    while (!item.complete) {
      if (!writer.room(4352)) {
        return false;
      }
      writer.upload(item, *image);
    }
    // Publish newly ready placements in this frame, not a later empty transition frame. Already
    // resident placements were handled above and need not be replayed during upload progress.
    for (std::size_t index = 0; index < state.plan_size; ++index) {
      const auto& placed = state.plan.at(index);
      if (placed.key != item.key) {
        continue;
      }
      if (!writer.room(256)) {
        state.place_cursor = 0; // The upload is complete; resume ordinary placement publication.
        return false;
      }
      writer.place(placed, item.id, index);
    }
    return true;
  };
  // Finish the current data stream first. Presentation commands above remain responsive while
  // image bytes arrive; only another image's transmission must wait.
  for (auto& item : state.residents) {
    if (item.sent > 0 && !item.complete && !upload_and_place(item)) {
      return finish();
    }
  }
  for (auto& item : state.residents) {
    if (item.key.generation != 0 && !item.complete && !upload_and_place(item)) {
      return finish();
    }
  }
  state.pending = state.place_cursor < state.plan_size;
  return finish();
}
} // namespace frame::render
