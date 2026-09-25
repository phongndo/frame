#include "terminal/terminal_impl.hpp"

#include "lemma/assert.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "lemma/terminal_identity.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lemma::vt {

[[nodiscard]] auto library_build_info() noexcept -> std::expected<LibraryBuildInfo, Error> {
  GhosttyString version{};
  bool simd = false;
  bool kitty_graphics = false;
  bool tmux_control_mode = false;
  GhosttyOptimizeMode optimize = GHOSTTY_OPTIMIZE_DEBUG;

  if (ghostty_build_info(GHOSTTY_BUILD_INFO_VERSION_STRING, &version) != GHOSTTY_SUCCESS ||
      ghostty_build_info(GHOSTTY_BUILD_INFO_SIMD, &simd) != GHOSTTY_SUCCESS ||
      ghostty_build_info(GHOSTTY_BUILD_INFO_KITTY_GRAPHICS, &kitty_graphics) != GHOSTTY_SUCCESS ||
      ghostty_build_info(GHOSTTY_BUILD_INFO_TMUX_CONTROL_MODE, &tmux_control_mode) !=
          GHOSTTY_SUCCESS ||
      ghostty_build_info(GHOSTTY_BUILD_INFO_OPTIMIZE, &optimize) != GHOSTTY_SUCCESS) {
    return std::unexpected(Error::invalid_state);
  }

  const auto optimization = [optimize]() noexcept -> std::expected<BuildOptimization, Error> {
    switch (optimize) {
    case GHOSTTY_OPTIMIZE_DEBUG:
      return BuildOptimization::debug;
    case GHOSTTY_OPTIMIZE_RELEASE_SAFE:
      return BuildOptimization::release_safe;
    case GHOSTTY_OPTIMIZE_RELEASE_SMALL:
      return BuildOptimization::release_small;
    case GHOSTTY_OPTIMIZE_RELEASE_FAST:
      return BuildOptimization::release_fast;
    case GHOSTTY_OPTIMIZE_MODE_MAX_VALUE:
      return std::unexpected(Error::invalid_state);
    }
    return std::unexpected(Error::invalid_state);
  }();
  if (!optimization.has_value()) {
    return std::unexpected(optimization.error());
  }

  return LibraryBuildInfo{
      .version = std::span(version.ptr, version.len),
      .optimization = *optimization,
      .simd = simd,
      .kitty_graphics = kitty_graphics,
      .tmux_control_mode = tmux_control_mode,
  };
}

[[nodiscard]] auto library_version() noexcept -> std::span<const std::uint8_t> {
  const auto info = library_build_info();
  return info.has_value() ? info->version : std::span<const std::uint8_t>{};
}

[[nodiscard]] auto default_theme() noexcept -> TerminalTheme {
  std::array<GhosttyColorRgb, 256> native_palette{};
  ghostty_color_palette_default(native_palette.data());

  TerminalTheme theme;
  for (std::size_t index = 0; index < theme.palette.size(); ++index) {
    const auto color = std::span(native_palette).subspan(index, 1).front();
    std::span(theme.palette).subspan(index, 1).front() = {
        .red = color.r,
        .green = color.g,
        .blue = color.b,
    };
  }
  theme.foreground = std::span(theme.palette).subspan(GHOSTTY_COLOR_NAMED_WHITE, 1).front();
  theme.background = std::span(theme.palette).subspan(GHOSTTY_COLOR_NAMED_BLACK, 1).front();
  theme.cursor = theme.foreground;
  return theme;
}

namespace {

[[nodiscard]] auto ghostty_build_matches_contract() noexcept -> bool {
  const auto info = library_build_info();
  if (!info.has_value()) {
    return false;
  }
  constexpr std::string_view expected_version = LEMMA_GHOSTTY_EXPECT_VERSION;
  const auto version_matches =
      info->version.size() == expected_version.size() &&
      std::memcmp(info->version.data(), expected_version.data(), expected_version.size()) == 0;
  return version_matches && info->simd == (LEMMA_GHOSTTY_EXPECT_SIMD != 0) &&
         info->kitty_graphics == (LEMMA_GHOSTTY_EXPECT_KITTY_GRAPHICS != 0) &&
         info->tmux_control_mode == (LEMMA_GHOSTTY_EXPECT_TMUX_CONTROL_MODE != 0) &&
         info->optimization == static_cast<BuildOptimization>(LEMMA_GHOSTTY_EXPECT_OPTIMIZE);
}

[[nodiscard]] auto valid_size(const TerminalSize& size) noexcept -> bool {
  if (size.columns == 0 || size.rows == 0 || size.columns > limits::terminal_columns_hard_max ||
      size.rows > limits::terminal_rows_hard_max) {
    return false;
  }

  const auto width_max = std::numeric_limits<std::uint32_t>::max() / size.columns;
  const auto height_max = std::numeric_limits<std::uint32_t>::max() / size.rows;
  return size.cell_width_px <= width_max && size.cell_height_px <= height_max;
}

[[nodiscard]] constexpr auto grown_hash_capacity(const std::size_t current,
                                                 const std::size_t required,
                                                 const std::size_t maximum) noexcept
    -> std::size_t {
  LEMMA_ASSERT(current > 0 && required > current && required <= maximum);
  const auto geometric = current + std::max(current / 2U, std::size_t{1});
  return std::min(maximum, std::max(required, geometric));
}

[[nodiscard]] auto valid_options(const TerminalOptions& options) noexcept -> bool {
  if (!valid_size(options.size)) {
    return false;
  }
  if (options.scrollback_bytes_max > limits::terminal_scrollback_bytes_hard_max ||
      (options.scrollback_lines_max.has_value() &&
       *options.scrollback_lines_max > limits::terminal_scrollback_lines_hard_max)) {
    return false;
  }
  return options.allocation_bytes_max > 0 &&
         options.allocation_bytes_max <= limits::terminal_allocation_bytes_hard_max;
}

template <typename Function>
[[nodiscard]] auto callback_pointer(const Function function) noexcept -> const void* {
  static_assert(sizeof(Function) == sizeof(const void*));
  return std::bit_cast<const void*>(function); // NOLINT(bugprone-bitwise-pointer-cast)
}

[[nodiscard]] constexpr auto ghostty_color(const RgbColor color) noexcept -> GhosttyColorRgb {
  return {.r = color.red, .g = color.green, .b = color.blue};
}

[[nodiscard]] auto apply_theme(const GhosttyTerminal terminal, const TerminalTheme& theme) noexcept
    -> GhosttyResult {
  const auto foreground = ghostty_color(theme.foreground);
  const auto background = ghostty_color(theme.background);
  const auto cursor = ghostty_color(theme.cursor);
  std::array<GhosttyColorRgb, 256> palette{};
  for (std::size_t index = 0; index < palette.size(); ++index) {
    std::span(palette).subspan(index, 1).front() =
        ghostty_color(std::span(theme.palette).subspan(index, 1).front());
  }
  auto result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &foreground);
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &background);
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cursor);
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette.data());
  }
  return result;
}

[[nodiscard]] auto configure_graphics(const GhosttyTerminal terminal) noexcept -> GhosttyResult {
  constexpr std::uint64_t storage_limit = std::uint64_t{8} * 1024U * 1024U;
  constexpr bool disabled = false;
  constexpr std::size_t kitty_apc_limit = 8192;
  auto result = detail::register_png_decoder();
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,
                                  &storage_limit);
  }
  if (result == GHOSTTY_SUCCESS) {
    result =
        ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &disabled);
  }
  if (result == GHOSTTY_SUCCESS) {
    result =
        ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE, nullptr);
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM,
                                  &disabled);
  }
  if (result == GHOSTTY_SUCCESS) {
    result =
        ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_APC_MAX_BYTES_KITTY, &kitty_apc_limit);
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_GLYPH_PROTOCOL, &disabled);
  }
  return result;
}

[[nodiscard]] auto configure_terminal(const GhosttyTerminal terminal,
                                      const std::size_t scrollback_bytes_max,
                                      const std::optional<std::size_t> scrollback_lines_max,
                                      const TerminalTheme& theme) noexcept -> GhosttyResult {
  auto result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,
                                     &scrollback_bytes_max);
  if (result == GHOSTTY_SUCCESS) {
    result =
        ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,
                             scrollback_lines_max.has_value() ? &*scrollback_lines_max : nullptr);
  }
  if (result == GHOSTTY_SUCCESS) {
    result = apply_theme(terminal, theme);
  }
  if (result == GHOSTTY_SUCCESS) {
    result = configure_graphics(terminal);
  }
  if (result == GHOSTTY_SUCCESS) {
    constexpr std::size_t unknown_sequence_bytes_max = limits::unknown_sequence_bytes_max;
    result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_UNKNOWN_MAX_BYTES,
                                  &unknown_sequence_bytes_max);
  }
  if (result == GHOSTTY_SUCCESS) {
    // Ghostty's borrowed string representation uses unsigned UTF-8 bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const GhosttyString name{.ptr = reinterpret_cast<const std::uint8_t*>(terminal_name.data()),
                             .len = terminal_name.size()};
    result = ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_TERMINFO_NAME, &name);
  }
  return result;
}

} // namespace

namespace detail {

QuotaAllocator::QuotaAllocator(const std::size_t bytes_max) noexcept
    : bytes_max_(bytes_max), native_{.ctx = this, .vtable = &vtable} {
  LEMMA_ASSERT(bytes_max_ > 0);
  LEMMA_ASSERT(bytes_max_ <= limits::terminal_allocation_bytes_hard_max);
}

[[nodiscard]] auto QuotaAllocator::native() const noexcept -> const GhosttyAllocator* {
  return &native_;
}

[[nodiscard]] auto QuotaAllocator::stats() const noexcept -> AllocationStats { return stats_; }

// The allocator ABI necessarily uses explicit allocation and deallocation.
// NOLINTBEGIN(cppcoreguidelines-no-malloc)
void* QuotaAllocator::allocate(void* context, const std::size_t length,
                               const std::uint8_t alignment,
                               [[maybe_unused]] const std::uintptr_t return_address) noexcept {
  auto& allocator = *static_cast<QuotaAllocator*>(context);
  assert_valid_alignment(alignment);

  if (length == 0 || length > allocator.bytes_max_ - allocator.stats_.bytes_current) {
    allocator.record_failure();
    return nullptr;
  }

  void* const memory = allocate_raw(length, alignment);
  if (memory == nullptr) {
    allocator.record_failure();
    return nullptr;
  }

  allocator.record_allocation(length);
  return memory;
}

auto QuotaAllocator::resize(void* context, void* memory, const std::size_t memory_length,
                            const std::uint8_t alignment, const std::size_t new_length,
                            [[maybe_unused]] const std::uintptr_t return_address) noexcept -> bool {
  auto& allocator = *static_cast<QuotaAllocator*>(context);
  allocator.assert_allocation(memory, memory_length, alignment);
  LEMMA_ASSERT(new_length > 0);

  if (new_length > memory_length) {
    return false;
  }

  allocator.stats_.bytes_current -= memory_length - new_length;
  return true;
}

void* QuotaAllocator::remap(void* context, void* memory, const std::size_t memory_length,
                            const std::uint8_t alignment, const std::size_t new_length,
                            [[maybe_unused]] const std::uintptr_t return_address) noexcept {
  auto& allocator = *static_cast<QuotaAllocator*>(context);
  allocator.assert_allocation(memory, memory_length, alignment);
  LEMMA_ASSERT(new_length > 0);

  if (new_length <= memory_length) {
    allocator.stats_.bytes_current -= memory_length - new_length;
    return memory;
  }

  // Relocation temporarily owns both allocations, so the quota must cover both.
  if (new_length > allocator.bytes_max_ - allocator.stats_.bytes_current) {
    allocator.record_failure();
    return nullptr;
  }

  void* const resized_memory = allocate_raw(new_length, alignment);
  if (resized_memory == nullptr) {
    allocator.record_failure();
    return nullptr;
  }

  const auto bytes_transient = allocator.stats_.bytes_current + new_length;
  allocator.stats_.bytes_peak = std::max(allocator.stats_.bytes_peak, bytes_transient);
  std::memcpy(resized_memory, memory, memory_length);
  std::free(memory);
  allocator.stats_.bytes_current += new_length - memory_length;
  allocator.record_total_allocation();
  return resized_memory;
}

void QuotaAllocator::deallocate(void* context, void* memory, const std::size_t memory_length,
                                const std::uint8_t alignment,
                                [[maybe_unused]] const std::uintptr_t return_address) noexcept {
  auto& allocator = *static_cast<QuotaAllocator*>(context);
  allocator.assert_allocation(memory, memory_length, alignment);

  std::free(memory);
  allocator.stats_.bytes_current -= memory_length;
  --allocator.stats_.allocations_current;
}

void QuotaAllocator::record_allocation(const std::size_t length) noexcept {
  LEMMA_ASSERT(length <= bytes_max_ - stats_.bytes_current);
  LEMMA_ASSERT(stats_.allocations_current < std::numeric_limits<std::size_t>::max());

  stats_.bytes_current += length;
  stats_.bytes_peak = std::max(stats_.bytes_peak, stats_.bytes_current);
  ++stats_.allocations_current;
  record_total_allocation();
}

void QuotaAllocator::record_failure() noexcept {
  if (stats_.failures_total < std::numeric_limits<std::size_t>::max()) {
    ++stats_.failures_total;
  }
}

void QuotaAllocator::record_total_allocation() noexcept {
  if (stats_.allocations_total < std::numeric_limits<std::size_t>::max()) {
    ++stats_.allocations_total;
  }
}

void QuotaAllocator::assert_allocation(const void* memory, const std::size_t memory_length,
                                       const std::uint8_t alignment) const noexcept {
  LEMMA_ASSERT(memory != nullptr);
  LEMMA_ASSERT(memory_length > 0);
  LEMMA_ASSERT(memory_length <= stats_.bytes_current);
  LEMMA_ASSERT(stats_.allocations_current > 0);
  assert_valid_alignment(alignment);
}

[[nodiscard]] auto QuotaAllocator::allocate_raw(const std::size_t length,
                                                const std::uint8_t alignment) noexcept -> void* {
  assert_valid_alignment(alignment);
  const auto alignment_bytes = std::size_t{1} << alignment;
  if (alignment_bytes <= alignof(std::max_align_t)) {
    return std::malloc(length);
  }

  void* memory = nullptr;
  if (posix_memalign(&memory, alignment_bytes, length) != 0) {
    return nullptr;
  }
  return memory;
}

void QuotaAllocator::assert_valid_alignment(const std::uint8_t alignment) noexcept {
  // Ghostty forwards Zig's log2 alignment enum despite the C header describing bytes.
  LEMMA_ASSERT(std::has_single_bit(alignof(std::max_align_t)));
  LEMMA_ASSERT(alignment < std::numeric_limits<std::size_t>::digits);
}

const GhosttyAllocatorVtable QuotaAllocator::vtable{
    .alloc = &QuotaAllocator::allocate,
    .resize = &QuotaAllocator::resize,
    .remap = &QuotaAllocator::remap,
    .free = &QuotaAllocator::deallocate,
};
// NOLINTEND(cppcoreguidelines-no-malloc)

[[nodiscard]] auto map_error(const GhosttyResult result) noexcept -> Error {
  switch (result) {
  case GHOSTTY_OUT_OF_MEMORY:
    return Error::out_of_memory;
  case GHOSTTY_OUT_OF_SPACE:
    return Error::out_of_space;
  case GHOSTTY_IO_ERROR:
    return Error::io_error;
  case GHOSTTY_LIMIT_EXCEEDED:
    return Error::limit_exceeded;
  case GHOSTTY_REJECTED:
  case GHOSTTY_INVALID_VALUE:
  case GHOSTTY_NO_VALUE:
  case GHOSTTY_SUCCESS:
  case GHOSTTY_RESULT_MAX_VALUE:
    return Error::invalid_state;
  }
  return Error::invalid_state;
}

} // namespace detail

Terminal::Impl::Impl(const TerminalOptions& terminal_options) noexcept
    : options(terminal_options), session_theme(terminal_options.theme.value_or(default_theme())),
      allocator(terminal_options.allocation_bytes_max) {
  render_colors.size = sizeof(render_colors);
}

Terminal::Impl::~Impl() {
  clipboard.reset(); // Retained requests borrow the terminal's protocol reply owner.
  ghostty_tracked_grid_ref_free(selection_checkpoint_start);
  ghostty_tracked_grid_ref_free(selection_checkpoint_end);
  for (auto* const event : selection_events) {
    ghostty_selection_gesture_event_free(event);
  }
  ghostty_selection_gesture_free(selection_gesture, terminal);
  ghostty_mouse_event_free(mouse_event);
  ghostty_mouse_encoder_free(mouse_encoder);
  ghostty_key_event_free(key_event);
  ghostty_key_encoder_free(key_encoder);
  ghostty_render_state_row_cells_free(row_cells);
  ghostty_render_state_row_iterator_free(row_iterator);
  ghostty_render_state_free(render_state);
  ghostty_terminal_free(terminal);
  LEMMA_ASSERT(allocator.stats().bytes_current == 0);
  LEMMA_ASSERT(allocator.stats().allocations_current == 0);
}

Terminal::Terminal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
}

Terminal::Terminal(Terminal&& other) noexcept = default;

auto Terminal::operator=(Terminal&& other) noexcept -> Terminal& = default;

Terminal::~Terminal() = default;

// Construction validates and installs each independent Ghostty option before ownership escapes.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::create(const TerminalOptions& options) noexcept -> std::expected<Terminal, Error> {
  if (!ghostty_build_matches_contract()) {
    return std::unexpected(Error::invalid_state);
  }
  if (!valid_options(options)) {
    return std::unexpected(Error::invalid_options);
  }

  std::unique_ptr<Impl> impl;
  try {
    impl = std::make_unique<Impl>(options);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }

  auto result = ghostty_terminal_new(impl->allocator.native(), &impl->terminal,
                                     options.size.columns, options.size.rows);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  result = ghostty_terminal_resize(impl->terminal, options.size.columns, options.size.rows,
                                   options.size.cell_width_px, options.size.cell_height_px);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  result = configure_terminal(impl->terminal, options.scrollback_bytes_max,
                              options.scrollback_lines_max, impl->session_theme);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  result = ghostty_key_encoder_new(impl->allocator.native(), &impl->key_encoder);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  result = ghostty_key_event_new(impl->allocator.native(), &impl->key_event);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  result = ghostty_mouse_encoder_new(impl->allocator.native(), &impl->mouse_encoder);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  result = ghostty_mouse_event_new(impl->allocator.native(), &impl->mouse_event);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  result = ghostty_render_state_new(impl->allocator.native(), &impl->render_state);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  result = ghostty_render_state_row_iterator_new(impl->allocator.native(), &impl->row_iterator);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  result = ghostty_render_state_row_cells_new(impl->allocator.native(), &impl->row_cells);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  impl->row_hash_count = options.size.rows;
  // The physical cell shadow is allocated by the first presentation.
  impl->physical_cell_count = static_cast<std::size_t>(options.size.columns) * options.size.rows;
  try {
    impl->row_hash_storage.resize(impl->row_hash_count * 2U);
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }

  result = ghostty_terminal_set(impl->terminal, GHOSTTY_TERMINAL_OPT_USERDATA, impl.get());
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  result = ghostty_terminal_set(
      impl->terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
      callback_pointer(static_cast<GhosttyTerminalWritePtyFn>(&Impl::write_pty)));
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  result = ghostty_terminal_set(impl->terminal, GHOSTTY_TERMINAL_OPT_BELL,
                                callback_pointer(static_cast<GhosttyTerminalBellFn>(&Impl::bell)));
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  result = ghostty_terminal_set(
      impl->terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
      callback_pointer(static_cast<GhosttyTerminalTitleChangedFn>(&Impl::title_changed)));
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_PWD_CHANGED,
        callback_pointer(static_cast<GhosttyTerminalPwdChangedFn>(&Impl::pwd_changed)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result =
        ghostty_terminal_set(impl->terminal, GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION,
                             callback_pointer(static_cast<GhosttyTerminalDesktopNotificationFn>(
                                 &Impl::desktop_notification)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_PROGRESS_REPORT,
        callback_pointer(static_cast<GhosttyTerminalProgressReportFn>(&Impl::progress_report)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_UNKNOWN_SEQUENCE,
        callback_pointer(static_cast<GhosttyTerminalUnknownSequenceFn>(&Impl::unknown_sequence)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_ENQUIRY,
        callback_pointer(static_cast<GhosttyTerminalEnquiryFn>(&Impl::enquiry)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
        callback_pointer(static_cast<GhosttyTerminalClipboardWriteFn>(&Impl::clipboard_write)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ,
        callback_pointer(static_cast<GhosttyTerminalClipboardReadFn>(&Impl::clipboard_read)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
        callback_pointer(static_cast<GhosttyTerminalColorSchemeFn>(&Impl::color_scheme)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
        callback_pointer(static_cast<GhosttyTerminalDeviceAttributesFn>(&Impl::device_attributes)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_SIZE,
        callback_pointer(static_cast<GhosttyTerminalSizeFn>(&Impl::size_report)));
  }
  if (result == GHOSTTY_SUCCESS) {
    result = ghostty_terminal_set(
        impl->terminal, GHOSTTY_TERMINAL_OPT_XTVERSION,
        callback_pointer(static_cast<GhosttyTerminalXtversionFn>(&Impl::xtversion)));
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  return Terminal(std::move(impl));
}

void Terminal::write(const std::span<const std::byte> bytes) noexcept {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);

  if (!bytes.empty()) {
    // std::byte and uint8_t are both byte views; Ghostty's C ABI uses the latter.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    ghostty_terminal_vt_write(impl_->terminal, data, bytes.size());
  }
}

auto Terminal::write_and_report_damage(const std::span<const std::byte> bytes) noexcept
    -> std::expected<DirtyState, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  LEMMA_ASSERT(impl_->render_state != nullptr);

  auto result = ghostty_render_state_update(impl_->render_state, impl_->terminal);
  if (result != GHOSTTY_SUCCESS) {
    write(bytes);
    return std::unexpected(detail::map_error(result));
  }
  const auto prior_damage = impl_->dirty_state();
  if (!prior_damage.has_value()) {
    write(bytes);
    return std::unexpected(prior_damage.error());
  }
  const auto cleared = impl_->set_dirty_state(DirtyState::clean);
  if (!cleared.has_value()) {
    write(bytes);
    return std::unexpected(cleared.error());
  }

  write(bytes);
  result = ghostty_render_state_update(impl_->render_state, impl_->terminal);
  if (result != GHOSTTY_SUCCESS) {
    const auto restored = impl_->set_dirty_state(*prior_damage);
    if (!restored.has_value()) {
      return std::unexpected(restored.error());
    }
    return std::unexpected(detail::map_error(result));
  }
  const auto acquired_damage = impl_->dirty_state();
  if (!acquired_damage.has_value()) {
    const auto restored = impl_->set_dirty_state(*prior_damage);
    if (!restored.has_value()) {
      return std::unexpected(restored.error());
    }
    return std::unexpected(acquired_damage.error());
  }
  const auto accumulated_damage = std::max(*prior_damage, *acquired_damage);
  const auto restored = impl_->set_dirty_state(accumulated_damage);
  if (!restored.has_value()) {
    return std::unexpected(restored.error());
  }
  return *acquired_damage;
}

auto Terminal::resize(const TerminalSize& size) noexcept -> std::expected<void, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);

  if (!valid_size(size)) {
    return std::unexpected(Error::invalid_options);
  }

  const auto physical_cell_count = static_cast<std::size_t>(size.columns) * size.rows;
  auto target_capacity = impl_->physical_cell_capacity;
  const auto row_capacity = impl_->row_hash_storage.size() / 2U;
  detail::CellHashStorage physical_cell_hashes;
  std::vector<std::uint64_t> row_hash_storage;
  try {
    // An unallocated shadow is sized by the next presentation instead.
    if (impl_->physical_cell_hashes != nullptr && physical_cell_count > target_capacity) {
      constexpr auto cells_max = static_cast<std::size_t>(limits::terminal_columns_hard_max) *
                                 limits::terminal_rows_hard_max;
      target_capacity = grown_hash_capacity(target_capacity, physical_cell_count, cells_max);
      // Runtime-sized cell storage cannot use std::array.
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
      physical_cell_hashes = std::make_unique_for_overwrite<std::uint64_t[]>(target_capacity);
    }
    if (size.rows > row_capacity) {
      row_hash_storage.resize(
          grown_hash_capacity(row_capacity, size.rows, limits::terminal_rows_hard_max) * 2U);
    }
  } catch (const std::bad_alloc&) {
    return std::unexpected(Error::out_of_memory);
  }

  const auto result = ghostty_terminal_resize(impl_->terminal, size.columns, size.rows,
                                              size.cell_width_px, size.cell_height_px);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  if (physical_cell_hashes != nullptr) {
    impl_->physical_cell_hashes = std::move(physical_cell_hashes);
    impl_->physical_cell_capacity = target_capacity;
  }
  if (!row_hash_storage.empty()) {
    impl_->row_hash_storage = std::move(row_hash_storage);
  }
  impl_->physical_cell_count = physical_cell_count;
  LEMMA_ASSERT(impl_->physical_cell_hashes == nullptr ||
               impl_->physical_cell_capacity >= impl_->physical_cell_count);
  impl_->row_hash_count = size.rows;
  LEMMA_ASSERT(impl_->row_hash_storage.size() / 2U >= impl_->row_hash_count);
  std::ranges::fill(impl_->row_hashes(), std::uint64_t{0});
  impl_->mirrored_modes_valid = false;
  impl_->mirrored_mouse_modes_valid = false;
  impl_->ansi_physical_valid = false;
  ghostty_mouse_encoder_reset(impl_->mouse_encoder);
  impl_->options.size = size;
  return {};
}

auto Terminal::size() const noexcept -> TerminalSize {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  return impl_->options.size;
}

auto Terminal::theme() const noexcept -> TerminalTheme {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  return impl_->session_theme;
}

auto Terminal::set_theme(const TerminalTheme& theme) noexcept -> std::expected<void, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  const auto result = apply_theme(impl_->terminal, theme);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  impl_->session_theme = theme;
  impl_->ansi_physical_valid = false;
  return {};
}

auto Terminal::cursor_at_prompt() const noexcept -> std::expected<bool, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  bool value = false;
  const auto result =
      ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_CURSOR_AT_PROMPT, &value);
  return result == GHOSTTY_SUCCESS ? std::expected<bool, Error>{value}
                                   : std::unexpected(detail::map_error(result));
}

auto Terminal::inspection() const noexcept -> std::expected<TerminalInspection, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);

  GhosttyTerminalScrollbar scrollbar{};
  GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
  std::size_t scrollback = 0;
  std::uint16_t cursor_x = 0;
  std::uint16_t cursor_y = 0;
  bool viewport_active = true;
  bool cursor_visible = false;
  bool cursor_at_prompt = false;
  const std::array keys{
      GHOSTTY_TERMINAL_DATA_SCROLLBAR,       GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
      GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, GHOSTTY_TERMINAL_DATA_CURSOR_X,
      GHOSTTY_TERMINAL_DATA_CURSOR_Y,        GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE,
      GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE,  GHOSTTY_TERMINAL_DATA_CURSOR_AT_PROMPT,
  };
  std::array<void*, keys.size()> values{&scrollbar,      &screen,          &scrollback,
                                        &cursor_x,       &cursor_y,        &viewport_active,
                                        &cursor_visible, &cursor_at_prompt};
  std::size_t written = 0;
  const auto result = ghostty_terminal_get_multi(impl_->terminal, keys.size(), keys.data(),
                                                 values.data(), &written);
  if (result != GHOSTTY_SUCCESS || written != keys.size()) {
    return std::unexpected(detail::map_error(result));
  }
  ActiveScreen active_screen{ActiveScreen::primary};
  switch (screen) {
  case GHOSTTY_TERMINAL_SCREEN_PRIMARY:
    active_screen = ActiveScreen::primary;
    break;
  case GHOSTTY_TERMINAL_SCREEN_ALTERNATE:
    active_screen = ActiveScreen::alternate;
    break;
  case GHOSTTY_TERMINAL_SCREEN_MAX_VALUE:
    return std::unexpected(Error::invalid_state);
  }
  return TerminalInspection{
      .viewport = {.total_rows = scrollbar.total,
                   .offset = scrollbar.offset,
                   .visible_rows = scrollbar.len,
                   .follows_output = viewport_active},
      .scrollback_rows = scrollback,
      .cursor_column = cursor_x,
      .cursor_row = cursor_y,
      .active_screen = active_screen,
      .cursor_visible = cursor_visible,
      .cursor_at_prompt = cursor_at_prompt,
  };
}

auto Terminal::scrollback_rows() const noexcept -> std::expected<std::size_t, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);

  std::size_t rows = 0;
  const auto result =
      ghostty_terminal_get(impl_->terminal, GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS, &rows);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return rows;
}

auto Terminal::integrity_failed() const noexcept -> bool {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  if (impl_->pty_response_integrity_failed) {
    return true;
  }
  bool processing_error = true;
  const auto result = ghostty_terminal_get(
      impl_->terminal, GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR, &processing_error);
  return result != GHOSTTY_SUCCESS || processing_error;
}

auto Terminal::allocation_stats() const noexcept -> AllocationStats {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->terminal != nullptr);
  return impl_->allocator.stats();
}

} // namespace lemma::vt
