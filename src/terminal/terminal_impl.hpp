#ifndef LEMMA_TERMINAL_TERMINAL_IMPL_HPP
#define LEMMA_TERMINAL_TERMINAL_IMPL_HPP

#include "lemma/terminal/terminal.hpp"

#include <ghostty/vt.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace lemma::vt {
namespace detail {

class AnsiWriter;

// Covers allocations routed through Ghostty's C allocator. The render and other adapter-owned
// buffers remain independently bounded by their owning Lemma components.
class QuotaAllocator final {
public:
  explicit QuotaAllocator(std::size_t bytes_max) noexcept;

  [[nodiscard]] auto native() const noexcept -> const GhosttyAllocator*;
  [[nodiscard]] auto stats() const noexcept -> AllocationStats;

private:
  static void* allocate(void* context, std::size_t length, std::uint8_t alignment,
                        std::uintptr_t return_address) noexcept;
  static auto resize(void* context, void* memory, std::size_t memory_length, std::uint8_t alignment,
                     std::size_t new_length, std::uintptr_t return_address) noexcept -> bool;
  static void* remap(void* context, void* memory, std::size_t memory_length, std::uint8_t alignment,
                     std::size_t new_length, std::uintptr_t return_address) noexcept;
  static void deallocate(void* context, void* memory, std::size_t memory_length,
                         std::uint8_t alignment, std::uintptr_t return_address) noexcept;

  void record_allocation(std::size_t length) noexcept;
  void record_failure() noexcept;
  void record_total_allocation() noexcept;
  void assert_allocation(const void* memory, std::size_t memory_length,
                         std::uint8_t alignment) const noexcept;

  [[nodiscard]] static auto allocate_raw(std::size_t length, std::uint8_t alignment) noexcept
      -> void*;
  static void assert_valid_alignment(std::uint8_t alignment) noexcept;

  static const GhosttyAllocatorVtable vtable;

  std::size_t bytes_max_;
  AllocationStats stats_{};
  GhosttyAllocator native_;
};

using CellHashStorage = std::unique_ptr<std::uint64_t[]>; // NOLINT

[[nodiscard]] auto map_error(GhosttyResult result) noexcept -> Error;
[[nodiscard]] auto register_png_decoder() noexcept -> GhosttyResult;
[[nodiscard]] auto new_graphics_identity() noexcept -> std::uint64_t;

} // namespace detail

// This definition is private to lemma_terminal. Ghostty handles never enter Lemma's public headers
// or any target that does not explicitly belong to the terminal adapter.
struct Terminal::Impl final {
  explicit Impl(const TerminalOptions& terminal_options) noexcept;
  ~Impl();

  Impl(const Impl&) = delete;
  auto operator=(const Impl&) -> Impl& = delete;
  Impl(Impl&&) = delete;
  auto operator=(Impl&&) -> Impl& = delete;

  static void write_pty(GhosttyTerminal terminal_handle, void* userdata, const std::uint8_t* data,
                        std::size_t length) noexcept;
  static void bell(GhosttyTerminal terminal_handle, void* userdata) noexcept;
  static void title_changed(GhosttyTerminal terminal_handle, void* userdata) noexcept;
  static void pwd_changed(GhosttyTerminal terminal_handle, void* userdata) noexcept;
  static void desktop_notification(GhosttyTerminal terminal_handle, void* userdata,
                                   const GhosttyTerminalDesktopNotification* notification) noexcept;
  static void progress_report(GhosttyTerminal terminal_handle, void* userdata,
                              const GhosttyTerminalProgressReport* report) noexcept;
  static void semantic_prompt(GhosttyTerminal terminal_handle, void* userdata,
                              const GhosttyTerminalSemanticPrompt* prompt) noexcept;
  static void unknown_sequence(GhosttyTerminal terminal_handle, void* userdata,
                               const GhosttyTerminalUnknownSequence* sequence) noexcept;
  static auto enquiry(GhosttyTerminal terminal_handle, void* userdata) noexcept -> GhosttyString;
  static void clipboard_write(GhosttyTerminal terminal_handle, void* userdata,
                              const GhosttyClipboardWrite* request) noexcept;
  static void clipboard_read(GhosttyTerminal terminal_handle, void* userdata,
                             const GhosttyClipboardRead* request) noexcept;
  struct ClipboardPending final {
    ClipboardPending() = default;
    ClipboardPending(const ClipboardPending&) = delete;
    auto operator=(const ClipboardPending&) -> ClipboardPending& = delete;
    ClipboardPending(ClipboardPending&&) = delete;
    auto operator=(ClipboardPending&&) -> ClipboardPending& = delete;
    const GhosttyClipboardRead* read{nullptr};
    const GhosttyClipboardWrite* write{nullptr};
    std::vector<ClipboardContent> contents;
    ~ClipboardPending();
  };
  static auto color_scheme(GhosttyTerminal terminal_handle, void* userdata,
                           GhosttyColorScheme* output) noexcept -> bool;
  static auto device_attributes(GhosttyTerminal terminal_handle, void* userdata,
                                GhosttyDeviceAttributes* output) noexcept -> bool;
  static auto size_report(GhosttyTerminal terminal_handle, void* userdata,
                          GhosttySizeReportSize* output) noexcept -> bool;
  static auto xtversion(GhosttyTerminal terminal_handle, void* userdata) noexcept -> GhosttyString;

  [[nodiscard]] auto dirty_state() const noexcept -> std::expected<DirtyState, Error>;
  [[nodiscard]] auto set_dirty_state(DirtyState dirty) const noexcept -> std::expected<void, Error>;
  [[nodiscard]] auto populate_render_metadata(RenderUpdate& update) noexcept
      -> std::expected<void, Error>;
  [[nodiscard]] auto dirty_row_count() noexcept -> std::expected<std::size_t, Error>;
  [[nodiscard]] auto calculate_row_hash() noexcept -> std::expected<std::uint64_t, Error>;
  [[nodiscard]] auto detect_scroll() const noexcept -> std::int32_t;
  void apply_physical_scroll(std::int32_t scroll) noexcept;
  [[nodiscard]] auto encode_row(detail::AnsiWriter& writer, std::size_t row_index, bool force,
                                std::uint16_t origin_column, std::uint16_t origin_row,
                                bool erase_line_tail) noexcept -> std::expected<bool, Error>;

  TerminalOptions options;
  TerminalTheme session_theme{};
  detail::QuotaAllocator allocator;
  const std::uint64_t graphics_identity{detail::new_graphics_identity()};
  GhosttyTerminal terminal{nullptr};
  GhosttyKeyEncoder key_encoder{nullptr};
  GhosttyKeyEvent key_event{nullptr};
  GhosttyMouseEncoder mouse_encoder{nullptr};
  GhosttyMouseEvent mouse_event{nullptr};
  GhosttyRenderState render_state{nullptr};
  GhosttyRenderStateRowIterator row_iterator{nullptr};
  GhosttyRenderStateRowCells row_cells{nullptr};
  GhosttySelectionGesture selection_gesture{nullptr};
  std::array<GhosttySelectionGestureEvent, 5> selection_events{};
  GhosttyTrackedGridRef selection_checkpoint_start{nullptr};
  GhosttyTrackedGridRef selection_checkpoint_end{nullptr};
  // Presented physical-row hashes followed by the scroll-detection hashes of the next render.
  [[nodiscard]] auto row_hashes() noexcept -> std::span<std::uint64_t> {
    return std::span(row_hash_storage).first(row_hash_count);
  }
  [[nodiscard]] auto row_hashes() const noexcept -> std::span<const std::uint64_t> {
    return std::span(row_hash_storage).first(row_hash_count);
  }
  [[nodiscard]] auto current_row_hashes() noexcept -> std::span<std::uint64_t> {
    return std::span(row_hash_storage).subspan(row_hash_storage.size() / 2U, row_hash_count);
  }
  [[nodiscard]] auto current_row_hashes() const noexcept -> std::span<const std::uint64_t> {
    return std::span(row_hash_storage).subspan(row_hash_storage.size() / 2U, row_hash_count);
  }

  GhosttyRenderStateColors render_colors{};
  // Two equal halves sized to the current geometry, grown only on resize. Idle panes therefore do
  // not keep hard-maximum row arrays resident.
  std::vector<std::uint64_t> row_hash_storage;
  std::size_t row_hash_count{0};
  detail::CellHashStorage physical_cell_hashes;
  std::size_t physical_cell_count{0};
  std::size_t physical_cell_capacity{0};
  std::array<bool, 12> mirrored_mode_values{};
  GhosttyColorRgb projected_cursor_color{};
  std::uint8_t projected_cursor_code{0};
  GhosttyRenderStateCursorVisualStyle render_cursor_style{
      GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK};
  bool render_cursor_blinking{false};
  bool mirrored_modes_valid{false};
  bool mirrored_mouse_modes_valid{false};
  bool projected_cursor_valid{false};
  bool ansi_physical_valid{false};
  bool selection_checkpoint_rectangle{false};
  // At most limits::terminal_pty_response_bytes_max pending bytes. Capacity grows only when replies
  // need it and is retained after draining, so the bound is not resident for every pane.
  std::vector<std::byte> pty_responses;
  std::size_t pty_response_offset{0};
  EffectBatch effects{};
  TerminalSignals signals{};
  std::unique_ptr<ClipboardPending> clipboard;
  std::vector<std::byte> clipboard_responses;
  std::size_t clipboard_response_offset{0};
  std::uint64_t clipboard_id{0};
  bool clipboard_read_allowed{false};
  bool clipboard_write_allowed{false};
  bool capturing_clipboard_reply{false};
  bool pty_response_integrity_failed{false};
};

} // namespace lemma::vt

#endif // LEMMA_TERMINAL_TERMINAL_IMPL_HPP
