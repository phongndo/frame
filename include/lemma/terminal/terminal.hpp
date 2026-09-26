#ifndef LEMMA_TERMINAL_TERMINAL_HPP
#define LEMMA_TERMINAL_TERMINAL_HPP

#include "lemma/limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace lemma::vt {

// Version of the privately linked terminal engine. The returned view has static lifetime.
[[nodiscard]] auto library_version() noexcept -> std::span<const std::uint8_t>;

enum class Error : std::uint8_t {
  invalid_options,
  out_of_memory,
  invalid_state,
  out_of_space,
  io_error,
  limit_exceeded,
};

enum class BuildOptimization : std::uint8_t {
  debug,
  release_safe,
  release_small,
  release_fast,
};

struct LibraryBuildInfo final {
  std::span<const std::uint8_t> version;
  BuildOptimization optimization{BuildOptimization::debug};
  bool simd{false};
  bool kitty_graphics{false};
  bool tmux_control_mode{false};
};

// Build identity of the privately linked terminal engine. The version view has static lifetime.
[[nodiscard]] auto library_build_info() noexcept -> std::expected<LibraryBuildInfo, Error>;

enum class DirtyState : std::uint8_t {
  clean,
  partial,
  full,
};

enum class ScreenFormat : std::uint8_t {
  plain,
  vt,
  vt_full,
};

struct TerminalSize final {
  std::uint16_t columns{80};
  std::uint16_t rows{24};
  std::uint32_t cell_width_px{8};
  std::uint32_t cell_height_px{16};

  friend constexpr auto operator==(const TerminalSize&, const TerminalSize&) noexcept
      -> bool = default;
};

struct RgbColor final {
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};

  friend constexpr auto operator==(const RgbColor&, const RgbColor&) noexcept -> bool = default;
};

struct TerminalTheme final {
  RgbColor foreground{};
  RgbColor background{};
  RgbColor cursor{};
  std::array<RgbColor, 256> palette{};
  std::optional<RgbColor> selection_foreground;
  std::optional<RgbColor> selection_background;

  friend constexpr auto operator==(const TerminalTheme&, const TerminalTheme&) noexcept
      -> bool = default;
};

// Returns the concrete xterm-compatible theme used when TerminalOptions::theme is unset.
[[nodiscard]] auto default_theme() noexcept -> TerminalTheme;

struct TextMetrics final {
  std::size_t codepoints{0};
  std::size_t graphemes{0};
  std::size_t columns{0};
};

// Validates one printable UTF-8 text run and measures it using Ghostty's canonical grapheme-width
// rules. Control characters, malformed UTF-8, surrogates, and noncharacters are rejected so a
// retained Grid cannot emit terminal control input or escape its resolved rectangle.
[[nodiscard]] auto measure_grid_text(std::string_view text) noexcept
    -> std::expected<TextMetrics, Error>;

struct TerminalOptions final {
  TerminalSize size{};
  // Ghostty prunes scrollback at page granularity, so retained byte and line counts may exceed
  // their estimates. The limits are independent and the first one reached drives pruning.
  std::size_t scrollback_bytes_max{limits::terminal_scrollback_bytes_default};
  std::size_t allocation_bytes_max{limits::terminal_allocation_bytes_default};
  std::optional<TerminalTheme> theme;
  std::optional<std::size_t> scrollback_lines_max;
};

// Covers only allocations routed through Lemma's Ghostty C allocator. Ghostty PagePool storage and
// adapter-owned buffers such as physical cell hashes are excluded.
struct AllocationStats final {
  std::size_t bytes_current{0};
  std::size_t bytes_peak{0};
  std::size_t allocations_current{0};
  std::size_t allocations_total{0};
  std::size_t failures_total{0};
};

struct GraphicPlacement final {
  std::uint64_t image_generation{0};
  std::uint32_t image_id{0};
  std::span<const std::byte> pixels{}; // NOLINT(readability-redundant-member-init)
  std::uint32_t image_width{0}, image_height{0};
  std::uint32_t source_x{0}, source_y{0}, source_width{0}, source_height{0};
  std::uint32_t pixel_width{0}, pixel_height{0};
  std::uint32_t columns{0}, rows{0}, offset_x{0}, offset_y{0};
  std::int32_t column{0}, row{0}, z{0};
  std::uint8_t channels{4};
};

enum class ClipboardStatus : std::uint8_t {
  success,
  denied,
  unsupported,
  busy,
  invalid_data,
  io_error
};

enum class ClipboardProtocol : std::uint8_t { osc52, kitty };

struct ClipboardContent final {
  std::string_view mime;
  std::span<const std::byte> data;
};

// One retained, bounded request. Views remain valid until completion/cancellation or terminal
// destruction, including across subsequent PTY writes. Empty data on a read names a requested MIME.
struct ClipboardRequest final {
  std::uint64_t id{0};
  ClipboardProtocol protocol{ClipboardProtocol::kitty};
  bool read{false};
  bool primary{false};
  bool list{false};
  // Explicit defaults support partial aggregate initialization under Clang's field checks.
  std::string_view name{};                      // NOLINT(readability-redundant-member-init)
  std::span<const ClipboardContent> contents{}; // NOLINT(readability-redundant-member-init)
};

struct EffectBatch final {
  std::uint64_t bells{0};
  std::uint64_t title_changes{0};
  std::uint64_t pwd_changes{0};
  std::uint64_t desktop_notifications{0};
  std::uint64_t progress_reports{0};
  // Callbacks that changed a TerminalSignals value; identical repeated reports do not count.
  std::uint64_t signal_changes{0};
  std::uint64_t clipboard_writes_denied{0};
  std::uint64_t unknown_sequences_dropped{0};
  bool unknown_sequence_truncated{false};
  bool pty_response_overflowed{false};
};

enum class ProgressState : std::uint8_t { none, normal, error, indeterminate, paused };
enum class CommandState : std::uint8_t { none, prompt, running, finished };

// Latest-value attention signals reported by the running application. Updates are O(1) stores
// from terminal callbacks; there is no queue. Counters saturate rather than wrap. Notification
// text is sanitized UTF-8 (invalid bytes and controls become '?'), with title and body together
// bounded by terminal_effect_text_bytes_max.
struct TerminalSignals final {
  static constexpr std::size_t notification_bytes_max = limits::terminal_effect_text_bytes_max;
  static constexpr std::size_t notification_title_bytes_max = notification_bytes_max / 4U;

  std::uint64_t bells{0};
  std::uint64_t notifications{0};
  // Completed commands: OSC 133;D while running (after OSC 133;C); exit_code belongs to the
  // latest one. Other D markers close a prompt and change nothing.
  std::uint64_t commands{0};
  std::uint64_t title_changes{0};
  std::uint64_t cwd_changes{0};
  std::optional<std::int32_t> exit_code;
  CommandState command{CommandState::none};
  ProgressState progress{ProgressState::none};
  std::optional<std::uint8_t> progress_percent;
  std::uint16_t notification_title_bytes{0};
  std::uint16_t notification_body_bytes{0};
  bool notification_truncated{false};
  std::array<char, notification_bytes_max> notification_text{};

  [[nodiscard]] auto notification_title() const noexcept -> std::string_view {
    return {notification_text.data(), notification_title_bytes};
  }
  [[nodiscard]] auto notification_body() const noexcept -> std::string_view {
    return std::string_view(notification_text.data(), notification_text.size())
        .substr(notification_title_bytes, notification_body_bytes);
  }
};

struct AnsiCursorPosition final {
  std::uint16_t column{0};
  std::uint16_t row{0};
};

struct AnsiRenderResult final {
  std::size_t bytes{0};
  // Rows that emitted bytes, and all rows re-encoded against retained physical state.
  std::size_t rows{0};
  std::size_t encoded_rows{0};
  std::int32_t scrolled_rows{0};
  bool full{false};
  // Visible cursor actually presented, including native overrides, in zero-based output
  // coordinates. Composition can arbitrate coverage without querying or rendering again.
  std::optional<AnsiCursorPosition> cursor;
};

// The pinned Ghostty cell retains one base codepoint plus at most 64 grapheme suffix codepoints.
// UTF-8 requires at most four bytes per retained codepoint.
inline constexpr std::size_t pane_grapheme_codepoints_max = 65;
inline constexpr std::size_t pane_ansi_grapheme_bytes_max = pane_grapheme_codepoints_max * 4U;
// Per-cell allocation contract for a composed pane: one maximum grapheme, a 78-byte full SGR
// transition, a conservatively per-cell 14-byte absolute position (normally once per row), the
// 10-byte autowrap boundary for a last-column grapheme, and the 4-byte reset emitted once per
// nonempty pane.
inline constexpr std::size_t pane_ansi_bytes_per_cell_max =
    pane_ansi_grapheme_bytes_max + 78U + 14U + 10U + 4U;

// How the outer terminal may move a pane's already presented rows when its content scrolls.
enum class TerminalScroll : std::uint8_t {
  // Other surfaces share the pane's rows; moved rows are re-encoded.
  none,
  // The pane is the whole outer screen: scroll the screen.
  screen,
  // The pane spans the outer width and alone owns its rows: scroll inside top/bottom margins.
  margins,
};

// Placement and outer-terminal policy for one surface in a composed frame. Coordinates are
// zero-based. The compositor, rather than the pane, owns synchronized-update framing and clearing.
struct PaneRenderOptions final {
  std::uint16_t column{0};
  std::uint16_t row{0};
  std::uint16_t cursor_override_column{0};
  std::uint16_t cursor_override_row{0};
  bool force_full{false};
  bool focused{false};
  bool cursor_override{false};
  TerminalScroll terminal_scroll{TerminalScroll::none};
};

enum class KeyAction : std::uint8_t {
  release,
  press,
  repeat,
};

enum class Key : std::uint8_t {
  unidentified,
  a,
  b,
  c,
  d,
  e,
  f,
  g,
  h,
  i,
  j,
  k,
  l,
  m,
  n,
  o,
  p,
  q,
  r,
  s,
  t,
  u,
  v,
  w,
  x,
  y,
  z,
  enter,
  tab,
  backspace,
  escape,
  space,
  arrow_up,
  arrow_down,
  arrow_left,
  arrow_right,
  home,
  end,
  insert,
  delete_key,
  page_up,
  page_down,
  f1,
  f2,
  f3,
  f4,
  f5,
  f6,
  f7,
  f8,
  f9,
  f10,
  f11,
  f12,
};

inline constexpr std::uint16_t key_modifier_shift = 1U << 0U;
inline constexpr std::uint16_t key_modifier_control = 1U << 1U;
inline constexpr std::uint16_t key_modifier_alt = 1U << 2U;
inline constexpr std::uint16_t key_modifier_super = 1U << 3U;
inline constexpr std::uint16_t key_modifier_caps_lock = 1U << 4U;
inline constexpr std::uint16_t key_modifier_num_lock = 1U << 5U;
inline constexpr std::uint16_t key_modifier_shift_side = 1U << 6U;
inline constexpr std::uint16_t key_modifier_control_side = 1U << 7U;
inline constexpr std::uint16_t key_modifier_alt_side = 1U << 8U;
inline constexpr std::uint16_t key_modifier_super_side = 1U << 9U;

struct KeyEvent final {
  KeyAction action{KeyAction::press};
  Key key{Key::unidentified};
  std::uint16_t modifiers{0};
  std::uint16_t consumed_modifiers{0};
  std::uint32_t unshifted_codepoint{0};
  std::string_view text;
  bool composing{false};
};

enum class FocusEvent : std::uint8_t {
  gained,
  lost,
};

enum class MouseAction : std::uint8_t {
  press,
  release,
  motion,
};

enum class MouseButton : std::uint8_t {
  left,
  right,
  middle,
  four,
  five,
  six,
  seven,
  eight,
  nine,
  ten,
  eleven,
};

struct MouseGeometry final {
  std::uint32_t screen_width{1};
  std::uint32_t screen_height{1};
  std::uint32_t cell_width{1};
  std::uint32_t cell_height{1};
  std::uint32_t padding_top{0};
  std::uint32_t padding_bottom{0};
  std::uint32_t padding_right{0};
  std::uint32_t padding_left{0};
};

struct MouseEvent final {
  MouseAction action{MouseAction::motion};
  std::optional<MouseButton> button;
  std::uint16_t modifiers{0};
  float x{0};
  float y{0};
  MouseGeometry geometry{};
  bool any_button_pressed{false};
};

struct MouseTrackingState final {
  bool enabled{false};
  bool unbuttoned_motion{false};

  friend constexpr auto operator==(const MouseTrackingState&, const MouseTrackingState&) noexcept
      -> bool = default;
};

struct RenderUpdate final {
  DirtyState dirty{DirtyState::clean};
  std::uint16_t columns{0};
  std::uint16_t rows{0};
  std::uint16_t cursor_column{0};
  std::uint16_t cursor_row{0};
  std::size_t dirty_rows{0};
  bool cursor_visible{false};
  bool cursor_in_viewport{false};
};

enum class PointSpace : std::uint8_t {
  active,
  viewport,
  screen,
  history,
};

struct TerminalPoint final {
  PointSpace space{PointSpace::viewport};
  std::uint16_t column{0};
  std::uint32_t row{0};

  friend constexpr auto operator==(const TerminalPoint&, const TerminalPoint&) noexcept
      -> bool = default;
};

enum class SelectionUnit : std::uint8_t {
  cell,
  word,
  line,
  block,
  output,
  all,
};

enum class SelectionAdjustment : std::uint8_t {
  left,
  right,
  up,
  down,
  history_top,
  history_bottom,
  viewport_top,
  viewport_middle,
  viewport_bottom,
  half_page_up,
  half_page_down,
  page_up,
  page_down,
  beginning_of_line,
  first_nonblank,
  end_of_line,
  word_left,
  word_right,
  word_end,
};

enum class SelectionGesturePhase : std::uint8_t {
  press,
  drag,
  release,
  autoscroll_tick,
  deep_press,
};

enum class SelectionAutoscroll : std::uint8_t {
  none,
  up,
  down,
};

struct SelectionGestureEvent final {
  SelectionGesturePhase phase{SelectionGesturePhase::press};
  TerminalPoint point{};
  double pointer_x{0};
  double pointer_y{0};
  std::uint32_t cell_width{1};
  std::uint32_t padding_left{0};
  std::uint32_t screen_height{1};
  std::uint64_t time_ns{0};
  std::uint64_t repeat_interval_ns{0};
  double repeat_distance{0};
  bool has_point{true};
  bool has_pointer_position{false};
  bool rectangle{false};
};

struct SelectionGestureResult final {
  SelectionAutoscroll autoscroll{SelectionAutoscroll::none};
  bool selection_changed{false};
  bool dragged{false};
};

struct SelectionRange final {
  TerminalPoint start{.space = PointSpace::screen};
  TerminalPoint end{.space = PointSpace::screen};
  bool rectangular{false};
};

enum class ViewportScroll : std::uint8_t {
  top,
  bottom,
  delta,
  row,
};

struct ViewportState final {
  std::uint64_t total_rows{0};
  std::uint64_t offset{0};
  std::uint64_t visible_rows{0};
  bool follows_output{true};
};

enum class ActiveScreen : std::uint8_t {
  primary,
  alternate,
};

// Compact, point-in-time terminal metadata for introspection. It contains no terminal text and no
// Ghostty representation. Child-reported strings such as title and PWD remain separate borrowed
// queries so callers must preserve their provenance explicitly.
struct TerminalInspection final {
  ViewportState viewport;
  std::size_t scrollback_rows{0};
  std::uint16_t cursor_column{0};
  std::uint16_t cursor_row{0};
  ActiveScreen active_screen{ActiveScreen::primary};
  bool cursor_visible{false};
  bool cursor_at_prompt{false};
};

enum class CompressionResult : std::uint8_t {
  unsupported,
  pending,
  complete,
};

enum class SearchDirection : std::uint8_t {
  forward,
  backward,
};

struct SearchMatch final {
  TerminalPoint start{.space = PointSpace::screen};
  TerminalPoint end{.space = PointSpace::screen};

  friend constexpr auto operator==(const SearchMatch&, const SearchMatch&) noexcept
      -> bool = default;
};

enum class SearchStepStatus : std::uint8_t {
  found,
  pending,
  not_found,
};

// Caller-owned continuation for a bounded literal-search slice. The additional fields preserve a
// partially matched candidate while the search advances across blank cells and wrapped rows.
struct SearchCursor final {
  TerminalPoint candidate{.space = PointSpace::screen};
  TerminalPoint text{.space = PointSpace::screen};
  TerminalPoint match_end{.space = PointSpace::screen};
  std::size_t query_offset{0};
  bool matching{false};
};

struct SearchStepResult final {
  SearchStepStatus status{SearchStepStatus::not_found};
  SearchMatch match{};
  SearchCursor next{};
};

class Terminal final {
public:
  [[nodiscard]] static auto create(const TerminalOptions& options) noexcept
      -> std::expected<Terminal, Error>;

  Terminal(Terminal&& other) noexcept;
  auto operator=(Terminal&& other) noexcept -> Terminal&;

  Terminal(const Terminal&) = delete;
  auto operator=(const Terminal&) -> Terminal& = delete;

  ~Terminal();

  void write(std::span<const std::byte> bytes) noexcept;

  // Writes bytes exactly once and reports only render damage acquired from this write. Damage
  // already pending in the retained render snapshot is preserved but is not included in the
  // result. An error means damage inspection failed; the bytes have still been parsed.
  [[nodiscard]] auto write_and_report_damage(std::span<const std::byte> bytes) noexcept
      -> std::expected<DirtyState, Error>;

  [[nodiscard]] auto resize(const TerminalSize& size) noexcept -> std::expected<void, Error>;

  [[nodiscard]] auto update_render_state() noexcept -> std::expected<RenderUpdate, Error>;
  [[nodiscard]] auto mark_rendered() noexcept -> std::expected<void, Error>;

  // Incremental ANSI renderer for a terminal occupying the complete outer terminal.
  [[nodiscard]] auto render_ansi(std::span<std::byte> output, bool force_full = false) noexcept
      -> std::expected<AnsiRenderResult, Error>;

  // Incremental pane-surface encoder. It emits absolute positions offset by options, but no
  // synchronized-update boundary or screen clear, so a renderer can compose several panes into one
  // atomic outer-terminal frame.
  [[nodiscard]] auto render_pane_ansi(std::span<std::byte> output,
                                      const PaneRenderOptions& options) noexcept
      -> std::expected<AnsiRenderResult, Error>;

  // Invalidates retained ANSI output state after a composed frame is discarded.
  void invalidate_ansi_render_state() noexcept;

  // Releases the retained render rows and physical presentation shadow of a pane that is no longer
  // presented. The next render rebuilds both and reports full damage, as presenting a newly visible
  // pane requires. Terminal contents are unaffected; allocation failure retains the existing rows.
  void release_render_cache() noexcept;

  // The compositor sometimes overrides child modes or cursor presentation after pane rendering.
  // These targeted invalidations repair only the affected outer-terminal projection on the next
  // frame.
  void invalidate_ansi_mode_projection() noexcept;
  void invalidate_ansi_cursor_projection() noexcept;

  // Encodes normalized input using the pane's active legacy or Kitty keyboard modes.
  [[nodiscard]] auto encode_key(const KeyEvent& event, std::span<std::byte> output) noexcept
      -> std::expected<std::size_t, Error>;

  // Encodes one opaque paste using Ghostty's filtering, newline, and bracketed-paste policy. The
  // input is mutable because Ghostty replaces unsafe control bytes in place.
  [[nodiscard]] auto encode_paste(std::span<std::byte> input, std::span<std::byte> output) noexcept
      -> std::expected<std::size_t, Error>;
  [[nodiscard]] auto paste_is_safe(std::span<const std::byte> input) const noexcept -> bool;
  [[nodiscard]] auto encode_focus(FocusEvent event, std::span<std::byte> output) const noexcept
      -> std::expected<std::size_t, Error>;
  [[nodiscard]] auto encode_mouse(const MouseEvent& event, std::span<std::byte> output) noexcept
      -> std::expected<std::size_t, Error>;
  // Narrow canonical child policy used to route physical mouse input without exposing Ghostty
  // mode values. Outer-terminal capture remains a Lemma presentation concern.
  [[nodiscard]] auto mouse_tracking() const noexcept -> std::expected<MouseTrackingState, Error>;
  // True only for Ghostty's alternate-screen wheel-to-cursor-key condition: alternate screen,
  // alternate-scroll mode enabled, and no explicit mouse reporting mode.
  [[nodiscard]] auto wheel_uses_alternate_scroll() const noexcept -> std::expected<bool, Error>;

  // Canonical child mode used by the pane-owned presentation gate.
  [[nodiscard]] auto synchronized_output() const noexcept -> std::expected<bool, Error>;

  // Diagnostic formatter for tests, demos, full-state fallback, and bounded public capture.
  [[nodiscard]] auto format_screen(ScreenFormat format, std::span<std::byte> output,
                                   bool unwrap = false) noexcept
      -> std::expected<std::size_t, Error>;
  // Formats the last bounded content rows of the canonical viewport. This is the bounded fallback
  // for a visible capture that cannot fit in caller storage.
  [[nodiscard]] auto format_visible_tail(ScreenFormat format, std::size_t rows,
                                         std::span<std::byte> output, bool unwrap = false) noexcept
      -> std::expected<std::size_t, Error>;
  // Formats the last bounded rows of the active screen including retained scrollback without
  // moving the canonical viewport or installing a terminal selection.
  [[nodiscard]] auto format_recent(ScreenFormat format, std::size_t rows,
                                   std::span<std::byte> output, bool unwrap = false) noexcept
      -> std::expected<std::size_t, Error>;
  // Formats the most recent OSC 133-delimited command-output range when Ghostty can derive one.
  [[nodiscard]] auto format_last_command(ScreenFormat format, std::span<std::byte> output,
                                         bool unwrap = true) noexcept
      -> std::expected<std::size_t, Error>;

  // Ghostty owns gesture interpretation and converts installed snapshots to tracked endpoints.
  // Lemma supplies presentation-space input and bounded caller-owned formatting storage.
  [[nodiscard]] auto selection_gesture(const SelectionGestureEvent& event) noexcept
      -> std::expected<SelectionGestureResult, Error>;
  void reset_selection_gesture() noexcept;
  [[nodiscard]] auto select(SelectionUnit unit, TerminalPoint point = {}) noexcept
      -> std::expected<bool, Error>;
  [[nodiscard]] auto selection_adjust(SelectionAdjustment adjustment, bool extend) noexcept
      -> std::expected<bool, Error>;
  [[nodiscard]] auto selection_set_unit(SelectionUnit unit) noexcept -> std::expected<bool, Error>;
  [[nodiscard]] auto selection_normalize_unit(SelectionUnit unit) noexcept
      -> std::expected<bool, Error>;
  [[nodiscard]] auto swap_selection_endpoints() noexcept -> std::expected<bool, Error>;
  [[nodiscard]] auto collapse_selection_to_endpoint() noexcept -> std::expected<bool, Error>;
  [[nodiscard]] auto selection_active() const noexcept -> std::expected<bool, Error>;
  [[nodiscard]] auto selection_endpoint(PointSpace space) const noexcept
      -> std::expected<std::optional<TerminalPoint>, Error>;
  [[nodiscard]] auto selection_range(PointSpace space) const noexcept
      -> std::expected<std::optional<SelectionRange>, Error>;
  // One adapter-owned checkpoint preserves the active tracked range while copy search temporarily
  // presents matches. Creating a new checkpoint replaces the old one transactionally. Its endpoint
  // can seed another search without reinstalling the checkpoint and flashing the original cursor.
  [[nodiscard]] auto checkpoint_selection() noexcept -> std::expected<bool, Error>;
  [[nodiscard]] auto selection_checkpoint_endpoint(PointSpace space) const noexcept
      -> std::expected<std::optional<TerminalPoint>, Error>;
  [[nodiscard]] auto restore_selection_checkpoint() noexcept -> std::expected<bool, Error>;
  void clear_selection_checkpoint() noexcept;
  // Reinstalls the terminal-owned selection from fresh snapshots after operations such as reflow.
  [[nodiscard]] auto refresh_selection() noexcept -> std::expected<bool, Error>;
  void clear_selection() noexcept;
  [[nodiscard]] auto format_selection(ScreenFormat format, std::span<std::byte> output) noexcept
      -> std::expected<std::size_t, Error>;

  void scroll_viewport(ViewportScroll behavior, std::int64_t value = 0) noexcept;
  // Moves a historical viewport to the active area and reports whether it changed.
  [[nodiscard]] auto scroll_viewport_to_bottom() noexcept -> std::expected<bool, Error>;
  [[nodiscard]] auto scroll_selection_into_view() noexcept -> std::expected<bool, Error>;
  [[nodiscard]] auto viewport_state() const noexcept -> std::expected<ViewportState, Error>;
  [[nodiscard]] auto cursor_at_prompt() const noexcept -> std::expected<bool, Error>;
  [[nodiscard]] auto inspection() const noexcept -> std::expected<TerminalInspection, Error>;

  [[nodiscard]] auto compression_activity() const noexcept -> std::expected<std::uint64_t, Error>;
  [[nodiscard]] auto compress_scrollback() noexcept -> std::expected<CompressionResult, Error>;

  // Performs one bounded literal-search slice directly over Ghostty grid references. No terminal
  // text grid or match list is retained. The work limit charges every inspected cell or row, and a
  // pending result carries caller-owned continuation for a partially matched candidate.
  [[nodiscard]] auto
  search_literal_step(std::string_view query, SearchDirection direction,
                      std::optional<SearchCursor> start = std::nullopt,
                      std::size_t work_limit = limits::search_candidates_per_step,
                      std::optional<TerminalPoint> stop_before = std::nullopt) const noexcept
      -> std::expected<SearchStepResult, Error>;
  [[nodiscard]] auto select_search_match(const SearchMatch& match) noexcept
      -> std::expected<void, Error>;

  [[nodiscard]] auto size() const noexcept -> TerminalSize;
  [[nodiscard]] auto theme() const noexcept -> TerminalTheme;
  // Replaces the embedder-owned defaults while preserving application OSC color overrides.
  [[nodiscard]] auto set_theme(const TerminalTheme& theme) noexcept -> std::expected<void, Error>;

  // Borrowed child-reported metadata remains valid only until the next terminal mutation.
  [[nodiscard]] auto title() const noexcept -> std::expected<std::string_view, Error>;
  [[nodiscard]] auto pwd() const noexcept -> std::expected<std::string_view, Error>;
  [[nodiscard]] auto scrollback_rows() const noexcept -> std::expected<std::size_t, Error>;
  [[nodiscard]] auto take_effects() noexcept -> EffectBatch;
  // Cumulative attention signals; the reference remains valid for this terminal's lifetime.
  [[nodiscard]] auto signals() const noexcept -> const TerminalSignals&;

  // Image views are borrowed only until the next terminal mutation. Instance identity never aliases
  // another terminal, including across Pane slot reuse; projection caches must not retain pixels.
  [[nodiscard]] auto graphics_identity() const noexcept -> std::uint64_t;
  [[nodiscard]] auto graphics_generation() const noexcept -> std::expected<std::uint64_t, Error>;
  [[nodiscard]] auto tick_graphics(std::uint64_t now_ms) noexcept
      -> std::expected<std::optional<std::uint64_t>, Error>;
  [[nodiscard]] auto graphics(std::span<GraphicPlacement> placements) noexcept
      -> std::expected<std::size_t, Error>;
  void set_clipboard_access(bool read, bool write) noexcept;
  [[nodiscard]] auto clipboard_request() const noexcept -> std::optional<ClipboardRequest>;
  // Completes only the matching request. No borrowed reply data survives this call. Replies use
  // the ordered PTY response queue; an unavailable clipboard is an error, never a stale cache.
  [[nodiscard]] auto complete_clipboard(std::uint64_t id, ClipboardStatus status,
                                        std::span<const ClipboardContent> contents = {}) noexcept
      -> bool;
  void cancel_clipboard() noexcept;

  [[nodiscard]] auto pending_pty_response_bytes() const noexcept -> std::size_t;
  [[nodiscard]] auto pty_response_overflowed() const noexcept -> bool;
  auto read_pty_responses(std::span<std::byte> output) noexcept -> std::size_t;

  // Sticky terminal-integrity state. A true result means a terminal-owned semantic update or
  // required PTY response may have been lost and the pane must fail closed.
  [[nodiscard]] auto integrity_failed() const noexcept -> bool;
  [[nodiscard]] auto allocation_stats() const noexcept -> AllocationStats;

private:
  struct Impl;

  explicit Terminal(std::unique_ptr<Impl> impl) noexcept;

  [[nodiscard]] auto
  render_ansi_impl(std::span<std::byte> output, bool force_full, std::uint16_t origin_column,
                   std::uint16_t origin_row, bool composed, bool focused, bool cursor_override,
                   std::uint16_t cursor_override_column, std::uint16_t cursor_override_row,
                   TerminalScroll terminal_scroll) noexcept
      -> std::expected<AnsiRenderResult, Error>;

  std::unique_ptr<Impl> impl_;
};

} // namespace lemma::vt

#endif // LEMMA_TERMINAL_TERMINAL_HPP
