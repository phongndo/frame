#include "terminal/terminal_impl.hpp"

#include "diagnostic/latency_trace.hpp"
#include "lemma/assert.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "terminal/fingerprint.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace lemma::vt {
namespace detail {

class AnsiWriter final {
public:
  explicit AnsiWriter(const std::span<std::byte> output) noexcept : output_(output) {}

  [[nodiscard]] auto append(const std::string_view text) noexcept -> bool {
    return append(std::as_bytes(std::span(text.data(), text.size())));
  }

  [[nodiscard]] auto append(const std::span<const std::byte> bytes) noexcept -> bool {
    if (bytes.size() > output_.size() - used_) {
      return false;
    }
    if (!bytes.empty()) {
      std::memcpy(output_.subspan(used_, bytes.size()).data(), bytes.data(), bytes.size());
      used_ += bytes.size();
    }
    return true;
  }

  template <typename Integer>
  [[nodiscard]] auto append_integer(const Integer value) noexcept -> bool {
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.begin(), buffer.end(), value);
    if (result.ec != std::errc{}) {
      return false;
    }
    const auto size = static_cast<std::size_t>(std::distance(buffer.begin(), result.ptr));
    return append(std::string_view(buffer.data(), size));
  }

  [[nodiscard]] auto append_hex_byte(const std::uint8_t value) noexcept -> bool {
    constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const std::array encoded{
        std::span(digits).subspan(static_cast<std::size_t>(value >> 4U), 1).front(),
        std::span(digits).subspan(static_cast<std::size_t>(value & 0x0FU), 1).front(),
    };
    return append(std::string_view(encoded.data(), encoded.size()));
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return used_; }
  void rewind(const std::size_t size) noexcept {
    LEMMA_ASSERT(size <= used_);
    used_ = size;
  }

private:
  std::span<std::byte> output_;
  std::size_t used_{0};
};

} // namespace detail

namespace {

using detail::AnsiWriter;

enum class AnsiColorTag : std::uint8_t {
  none,
  palette,
  rgb,
};

struct AnsiColor final {
  AnsiColorTag tag{AnsiColorTag::none};
  std::uint8_t index{0};
  std::uint8_t red{0};
  std::uint8_t green{0};
  std::uint8_t blue{0};

  friend constexpr auto operator==(const AnsiColor&, const AnsiColor&) noexcept -> bool = default;
};

struct AnsiStyle final {
  AnsiColor foreground{};
  AnsiColor background{};
  AnsiColor underline_color{};
  std::uint8_t underline{0};
  bool bold{false};
  bool italic{false};
  bool faint{false};
  bool blink{false};
  bool inverse{false};
  bool invisible{false};
  bool strikethrough{false};
  bool overline{false};

  friend constexpr auto operator==(const AnsiStyle&, const AnsiStyle&) noexcept -> bool = default;
};

[[nodiscard]] constexpr auto ansi_rgb(const GhosttyColorRgb color) noexcept -> AnsiColor {
  return {
      .tag = AnsiColorTag::rgb,
      .red = color.r,
      .green = color.g,
      .blue = color.b,
  };
}

[[nodiscard]] constexpr auto ansi_rgb(const RgbColor color) noexcept -> AnsiColor {
  return {
      .tag = AnsiColorTag::rgb,
      .red = color.red,
      .green = color.green,
      .blue = color.blue,
  };
}

[[nodiscard]] constexpr auto mix_channel(const std::uint8_t from,
                                         const std::uint8_t toward) noexcept -> std::uint8_t {
  constexpr int weight = 72;
  const auto delta = static_cast<int>(toward) - static_cast<int>(from);
  return static_cast<std::uint8_t>(static_cast<int>(from) + ((delta * weight) / 256));
}

[[nodiscard]] constexpr auto derived_selection_background(const TerminalTheme& theme) noexcept
    -> RgbColor {
  return {
      .red = mix_channel(theme.background.red, theme.foreground.red),
      .green = mix_channel(theme.background.green, theme.foreground.green),
      .blue = mix_channel(theme.background.blue, theme.foreground.blue),
  };
}

void apply_selection_highlight(AnsiStyle& style, const bool selected,
                               const TerminalTheme& theme) noexcept {
  if (!selected) {
    return;
  }
  if (style.inverse) {
    std::swap(style.foreground, style.background);
    style.inverse = false;
  }
  style.background =
      ansi_rgb(theme.selection_background.value_or(derived_selection_background(theme)));
  if (theme.selection_foreground.has_value()) {
    style.foreground = ansi_rgb(*theme.selection_foreground);
  }
}

[[nodiscard]] constexpr auto ansi_palette(const GhosttyColorPaletteIndex index) noexcept
    -> AnsiColor {
  return {
      .tag = AnsiColorTag::palette,
      .index = index,
  };
}

[[nodiscard]] constexpr auto same_color(const GhosttyColorRgb native,
                                        const RgbColor configured) noexcept -> bool {
  return native.r == configured.red && native.g == configured.green && native.b == configured.blue;
}

[[nodiscard]] auto palette_color(const GhosttyColorPaletteIndex index,
                                 const GhosttyRenderStateColors& colors,
                                 const TerminalTheme& theme) noexcept -> AnsiColor {
  const auto current = std::span(colors.palette).subspan(index, 1).front();
  const auto configured = std::span(theme.palette).subspan(index, 1).front();
  // Attach queries establish equivalence only for the configurable ANSI colors. Extended palette
  // entries may differ in the outer terminal, so preserve their canonical RGB rather than relying
  // on an unverified physical index.
  constexpr GhosttyColorPaletteIndex host_palette_colors_queried = 16;
  if (index >= host_palette_colors_queried) {
    return ansi_rgb(current);
  }
  // An OSC 4 override is pane-local and must not mutate the outer terminal's global palette.
  // Preserve the index only while Ghostty's effective entry still equals its configured default.
  return same_color(current, configured) ? ansi_palette(index) : ansi_rgb(current);
}

[[nodiscard]] constexpr auto default_color(const GhosttyColorRgb current,
                                           const RgbColor configured) noexcept -> AnsiColor {
  // A missing color after SGR 0 means the outer terminal's default. Only a pane-local OSC 10/11
  // override requires an explicit RGB projection.
  return same_color(current, configured) ? AnsiColor{} : ansi_rgb(current);
}

[[nodiscard]] auto style_color(const GhosttyStyleColor color,
                               const GhosttyRenderStateColors& colors, const TerminalTheme& theme,
                               const AnsiColor fallback = {}) noexcept -> AnsiColor {
  switch (color.tag) {
  case GHOSTTY_STYLE_COLOR_NONE:
    return fallback;
  case GHOSTTY_STYLE_COLOR_PALETTE:
    return palette_color(color.value.palette, colors, theme);
  case GHOSTTY_STYLE_COLOR_RGB:
    return ansi_rgb(color.value.rgb);
  case GHOSTTY_STYLE_COLOR_TAG_MAX_VALUE:
    return fallback;
  }
  return fallback;
}

[[nodiscard]] auto ansi_style(const GhosttyCell raw_cell, const GhosttyCellContentTag content_tag,
                              const GhosttyStyle& style, const GhosttyRenderStateColors& colors,
                              const TerminalTheme& theme) noexcept
    -> std::expected<AnsiStyle, Error> {
  const auto foreground = style_color(style.fg_color, colors, theme,
                                      default_color(colors.foreground, theme.foreground));
  auto background = style_color(style.bg_color, colors, theme,
                                default_color(colors.background, theme.background));

  GhosttyResult result = GHOSTTY_SUCCESS;
  if (content_tag == GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE) {
    GhosttyColorPaletteIndex index = 0;
    result = ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_COLOR_PALETTE, &index);
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
    background = palette_color(index, colors, theme);
  } else if (content_tag == GHOSTTY_CELL_CONTENT_BG_COLOR_RGB) {
    GhosttyColorRgb color{};
    result = ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_COLOR_RGB, &color);
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
    background = ansi_rgb(color);
  }

  const auto underline = style.underline_color.tag == GHOSTTY_STYLE_COLOR_NONE
                             ? AnsiColor{}
                             : style_color(style.underline_color, colors, theme);
  return AnsiStyle{
      .foreground = foreground,
      .background = background,
      .underline_color = underline,
      .underline = static_cast<std::uint8_t>(style.underline),
      .bold = style.bold,
      .italic = style.italic,
      .faint = style.faint,
      .blink = style.blink,
      .inverse = style.inverse,
      .invisible = style.invisible,
      .strikethrough = style.strikethrough,
      .overline = style.overline,
  };
}

// Positions Ghostty's per-cell accessor only for values that the raw cell cannot supply.
class RowCellCursor final {
public:
  void reset(const GhosttyRenderStateRowCells cells) noexcept {
    cells_ = cells;
    column_ = std::numeric_limits<std::size_t>::max();
  }

  [[nodiscard]] auto at(const std::size_t column) noexcept
      -> std::expected<GhosttyRenderStateRowCells, Error> {
    if (column != column_) {
      LEMMA_ASSERT(column <= std::numeric_limits<std::uint16_t>::max());
      const auto result =
          ghostty_render_state_row_cells_select(cells_, static_cast<std::uint16_t>(column));
      if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(detail::map_error(result));
      }
      column_ = column;
    }
    return cells_;
  }

private:
  GhosttyRenderStateRowCells cells_{nullptr};
  std::size_t column_{std::numeric_limits<std::size_t>::max()};
};

// Style IDs are page-local. Reuse a projection only within this row traversal, never across
// rows or render updates (which may change the page, palette, theme or style-ID allocation).
// Background-only cells carry their color in the cell itself; selection is applied by the caller
// to a copy, so neither can contaminate the retained text style.
class RowStyleProjection final {
public:
  [[nodiscard]] auto resolve(const GhosttyCell raw_cell, const GhosttyCellContentTag content_tag,
                             const GhosttyStyleId id, RowCellCursor& cursor,
                             const std::size_t column, const GhosttyRenderStateColors& colors,
                             const TerminalTheme& theme) noexcept
      -> std::expected<const AnsiStyle*, Error> {
    // The result borrows this projection until the next resolve; a hit copies nothing.
    const bool text_style = content_tag == GHOSTTY_CELL_CONTENT_CODEPOINT ||
                            content_tag == GHOSTTY_CELL_CONTENT_CODEPOINT_GRAPHEME;
    hit_ = text_style && valid_ && id == id_;
    if (hit_) {
      return &style_;
    }
    const auto cells = cursor.at(column);
    if (!cells.has_value()) {
      return std::unexpected(cells.error());
    }
    GhosttyStyle native{};
    native.size = sizeof(native);
    const auto result = ghostty_render_state_row_cells_get(
        *cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &native);
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
    const auto projected = ansi_style(raw_cell, content_tag, native, colors, theme);
    if (!projected.has_value()) {
      valid_ = false;
      return std::unexpected(projected.error());
    }
    default_ = ghostty_style_is_default(&native);
    valid_ = text_style;
    id_ = id;
    style_ = *projected;
    return &style_;
  }

  [[nodiscard]] auto native_default() const noexcept -> bool { return default_; }
  // The last resolve returned the projection of the previous resolve.
  [[nodiscard]] auto hit() const noexcept -> bool { return hit_; }

private:
  AnsiStyle style_{};
  GhosttyStyleId id_{0};
  bool valid_{false};
  bool default_{false};
  bool hit_{false};
};

struct SelectedColumns final {
  std::size_t begin{0};
  std::size_t end{0};

  [[nodiscard]] auto contains(const std::size_t column) const noexcept -> bool {
    return column >= begin && column < end;
  }
};

[[nodiscard]] auto selected_columns(const GhosttyRenderStateRowIterator row) noexcept
    -> std::expected<SelectedColumns, Error> {
  GhosttyRenderStateRowSelection selection = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
  const auto result =
      ghostty_render_state_row_get(row, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &selection);
  if (result == GHOSTTY_NO_VALUE) {
    return SelectedColumns{};
  }
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return SelectedColumns{
      .begin = selection.start_x,
      .end = static_cast<std::size_t>(selection.end_x) + 1U,
  };
}

[[nodiscard]] auto append_color(AnsiWriter& writer, const AnsiColor color,
                                const std::string_view prefix) noexcept -> bool {
  if (color.tag == AnsiColorTag::none) {
    return true;
  }
  if (!writer.append(";") || !writer.append(prefix)) {
    return false;
  }
  if (color.tag == AnsiColorTag::palette) {
    return writer.append(";5;") && writer.append_integer(color.index);
  }
  return writer.append(";2;") && writer.append_integer(color.red) && writer.append(";") &&
         writer.append_integer(color.green) && writer.append(";") &&
         writer.append_integer(color.blue);
}

constexpr std::uint8_t decscusr_steady_block = 2;

// DECSCUSR pairs each shape as blinking/steady: block 1/2, underline 3/4, bar 5/6. A hollow block
// is an emulator default rather than a DECSCUSR shape; present it as the nearest outer block.
[[nodiscard]] constexpr auto decscusr_code(const GhosttyRenderStateCursorVisualStyle style,
                                           const bool blinking) noexcept -> std::uint8_t {
  const auto steady = [style]() noexcept -> std::uint8_t {
    switch (style) {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
      return 4;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
      return 6;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK:
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_MAX_VALUE:
      break;
    }
    return decscusr_steady_block;
  }();
  return blinking ? static_cast<std::uint8_t>(steady - 1U) : steady;
}

[[nodiscard]] auto append_cursor_color(AnsiWriter& writer, const GhosttyColorRgb color,
                                       const RgbColor configured) noexcept -> bool {
  if (same_color(color, configured)) {
    return writer.append("\x1B]112\x1B\\");
  }
  return writer.append("\x1B]12;#") && writer.append_hex_byte(color.r) &&
         writer.append_hex_byte(color.g) && writer.append_hex_byte(color.b) &&
         writer.append("\x1B\\");
}

[[nodiscard]] auto append_style(AnsiWriter& writer, const AnsiStyle& style) noexcept -> bool {
  if (!writer.append("\x1B[0")) {
    return false;
  }
  const auto append_flag = [&writer](const bool enabled, const std::string_view code) noexcept {
    return !enabled || (writer.append(";") && writer.append(code));
  };
  if (!append_flag(style.bold, "1") || !append_flag(style.faint, "2") ||
      !append_flag(style.italic, "3") || !append_flag(style.blink, "5") ||
      !append_flag(style.inverse, "7") || !append_flag(style.invisible, "8") ||
      !append_flag(style.strikethrough, "9") || !append_flag(style.overline, "53")) {
    return false;
  }
  if (style.underline != 0 && (!writer.append(";4:") || !writer.append_integer(style.underline))) {
    return false;
  }
  return append_color(writer, style.foreground, "38") &&
         append_color(writer, style.background, "48") &&
         append_color(writer, style.underline_color, "58") && writer.append("m");
}

constexpr std::string_view hyperlink_open_prefix = "\x1B]8;id=lemma-";
constexpr std::string_view hyperlink_close = "\x1B]8;;\x1B\\";
// Every byte of an open and its close except the URI: the prefix, a 64-bit decimal scope, `;`, and
// the ST terminator.
constexpr std::size_t hyperlink_sequence_bytes_max = hyperlink_open_prefix.size() +
                                                     std::numeric_limits<std::uint64_t>::digits10 +
                                                     1U + 1U + 2U + hyperlink_close.size();

// Moves the outer terminal's active OSC 8 link from `active` to `link` (0 for none). The ID is the
// never-aliasing terminal instance scope, so links from different Panes never join in the outer
// terminal; OSC 8 joins cells only when both ID and URI match. An open that would exceed the render
// pass's allowance presents the cell as plain text instead.
[[nodiscard, gnu::noinline]] auto present_link(AnsiWriter& writer, std::uint64_t& active,
                                               const std::uint64_t link,
                                               const std::span<const std::uint8_t> uri,
                                               const std::uint64_t scope,
                                               std::size_t& allowance) noexcept -> bool {
  if (active != 0 && !writer.append(hyperlink_close)) {
    return false;
  }
  active = 0;
  const auto cost = hyperlink_sequence_bytes_max + uri.size();
  if (link == 0 || cost > allowance) {
    return true;
  }
  allowance -= cost;
  active = link;
  return writer.append(hyperlink_open_prefix) && writer.append_integer(scope) &&
         writer.append(";") && writer.append(std::as_bytes(uri)) && writer.append("\x1B\\");
}

[[nodiscard]] auto terminal_mode_enabled(const GhosttyTerminal terminal,
                                         const GhosttyMode mode) noexcept
    -> std::expected<bool, Error> {
  GhosttyTerminalModeConfig config{.mode = mode, .value = false};
  const auto result = ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE, &config);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(Error::invalid_state);
  }
  return config.value;
}

[[nodiscard]] constexpr auto utf8_codepoint_bytes(const std::uint8_t first) noexcept
    -> std::size_t {
  if ((first & 0x80U) == 0) {
    return 1;
  }
  if ((first & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((first & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((first & 0xF8U) == 0xF0U) {
    return 4;
  }
  return 1;
}

using detail::fingerprint_mix;
using detail::FingerprintKey;
using detail::RowFingerprint;

[[nodiscard]] constexpr auto color_word(const AnsiColor& color) noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>(color.tag) | (static_cast<std::uint64_t>(color.index) << 8U) |
         (static_cast<std::uint64_t>(color.red) << 16U) |
         (static_cast<std::uint64_t>(color.green) << 24U) |
         (static_cast<std::uint64_t>(color.blue) << 32U);
}

[[nodiscard]] auto hash_style(const FingerprintKey& key, const AnsiStyle& style) noexcept
    -> std::uint64_t {
  const std::array flags{style.bold,    style.italic,    style.faint,         style.blink,
                         style.inverse, style.invisible, style.strikethrough, style.overline};
  std::uint64_t attributes = style.underline;
  for (std::size_t index = 0; index < flags.size(); ++index) {
    attributes |= static_cast<std::uint64_t>(std::span(flags).subspan(index, 1).front())
                  << (8U + index);
  }
  // Each color occupies 40 bits; attributes fill the remaining high bits of the first word.
  auto hash = fingerprint_mix(key, key.initial, color_word(style.foreground) | (attributes << 40U));
  hash = fingerprint_mix(key, hash, color_word(style.background));
  return fingerprint_mix(key, hash, color_word(style.underline_color));
}

// Row-local memoization of the existing hash prefix, not a second terminal/style authority.
// Adjacent cells commonly share a projected style. Compare the complete value (after selection
// and palette projection), then hash width and graphemes independently.
class RenderedCellHasher final {
public:
  // Copies the key: a row-scoped member stays loadable without chasing a pointer after every
  // grapheme or cell store.
  explicit RenderedCellHasher(const FingerprintKey& key) noexcept : key_(key) {}

  // style_repeated promises that style equals the style of the previous call.
  [[nodiscard]] auto hash(const AnsiStyle& style, const bool style_repeated,
                          const GhosttyCellWide wide,
                          const std::span<const std::uint8_t> grapheme) noexcept -> std::uint64_t {
    if (!style_repeated && (!cached_valid_ || cached_.style != style)) {
      cached_ = HashedStyle{.style = style, .hash = hash_style(key_, style)};
      cached_valid_ = true;
    }
    static_assert(pane_ansi_grapheme_bytes_max <= std::numeric_limits<std::uint16_t>::max());
    // The length prefix makes the word sequence unambiguous. A single codepoint of up to five
    // UTF-8 bytes, the common case, needs one fold together with the cell width.
    constexpr std::size_t prefix_bytes = 5;
    constexpr std::size_t word_bytes = 8;
    auto word =
        static_cast<std::uint64_t>(wide) | (static_cast<std::uint64_t>(grapheme.size()) << 8U);
    const auto prefix = grapheme.first(std::min(grapheme.size(), prefix_bytes));
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      word |= static_cast<std::uint64_t>(prefix.subspan(index, 1).front()) << (24U + (8U * index));
    }
    auto result = fingerprint_mix(key_, cached_.hash, word);
    auto remaining = grapheme.subspan(prefix.size());
    while (!remaining.empty()) {
      const auto chunk = remaining.first(std::min(remaining.size(), word_bytes));
      word = 0;
      for (std::size_t index = 0; index < chunk.size(); ++index) {
        word |= static_cast<std::uint64_t>(chunk.subspan(index, 1).front()) << (8U * index);
      }
      result = fingerprint_mix(key_, result, word);
      remaining = remaining.subspan(chunk.size());
    }
    return result;
  }

  // Folds a presented hyperlink identity into a cell fingerprint.
  [[nodiscard]] auto with_link(const std::uint64_t hash, const std::uint64_t link) const noexcept
      -> std::uint64_t {
    return fingerprint_mix(key_, hash, link);
  }

private:
  struct HashedStyle final {
    AnsiStyle style;
    std::uint64_t hash;
  };
  FingerprintKey key_;
  HashedStyle cached_{};
  bool cached_valid_{false};
};

[[nodiscard]] auto hash_bytes(const FingerprintKey& key,
                              std::span<const std::uint8_t> bytes) noexcept -> std::uint64_t {
  auto hash = fingerprint_mix(key, key.initial, bytes.size());
  while (!bytes.empty()) {
    const auto chunk = bytes.first(std::min(bytes.size(), sizeof(std::uint64_t)));
    std::uint64_t word = 0;
    std::memcpy(&word, chunk.data(), chunk.size());
    hash = fingerprint_mix(key, hash, word);
    bytes = bytes.subspan(chunk.size());
  }
  return hash;
}

// Resolves the OSC 8 link of a cell whose raw value carries Ghostty's hyperlink flag. The render
// state copies only that flag, so the URI comes from the terminal page through an untracked grid
// reference; no terminal mutation separates the render-state update from encoding, so its viewport
// rows are the render state's. Ghostty's own link ID is not exposed, so the URI is the identity.
class RowLinks final {
public:
  RowLinks(const GhosttyTerminal terminal, const FingerprintKey& key) noexcept
      : terminal_(terminal), key_(&key) {}

  void reset(const std::uint16_t row, const std::size_t columns) noexcept {
    row_ = row;
    columns_ = columns;
    row_resolved_ = false;
  }

  // The nonzero keyed fingerprint of the cell's forwardable URI, or 0 when it has none that may
  // be forwarded. The URI stays readable through uri() until the next resolve.
  [[nodiscard]] auto resolve(const std::size_t column) noexcept -> std::uint64_t {
    LEMMA_ASSERT(column < columns_);
    if (!row_resolved_) {
      // Resolving a point walks the page list, so do it once per row. A grid reference is a page
      // position (node, x, y) and every cell of a row shares its node and y. Resolving the last
      // column also proves that the row's page spans every column.
      const GhosttyPoint point{
          .tag = GHOSTTY_POINT_TAG_VIEWPORT,
          .value = {.coordinate = {.x = static_cast<std::uint16_t>(columns_ - 1U), .y = row_}},
      };
      row_ref_ = GHOSTTY_INIT_SIZED(GhosttyGridRef);
      row_valid_ = ghostty_terminal_grid_ref(terminal_, point, &row_ref_) == GHOSTTY_SUCCESS;
      row_resolved_ = true;
    }
    if (!row_valid_) {
      return 0;
    }
    auto ref = row_ref_;
    ref.x = static_cast<std::uint16_t>(column);
    std::size_t length = 0;
    auto& next = std::span(uri_scratch()).subspan(1U - current_, 1).front();
    // A URI longer than the buffer reports GHOSTTY_OUT_OF_SPACE and is not forwarded.
    if (ghostty_grid_ref_hyperlink_uri(&ref, next.data(), next.size(), &length) !=
            GHOSTTY_SUCCESS ||
        length == 0) {
      return 0;
    }
    // Adjacent linked cells usually share one URI: compare it with the last one, and fingerprint
    // and validate it only when it changes.
    const auto fetched = std::span<const std::uint8_t>(next).first(length);
    if (last_ == 0 || !std::ranges::equal(fetched, uri())) {
      current_ = 1U - current_;
      size_ = length;
      last_ = std::max(hash_bytes(*key_, fetched), std::uint64_t{1});
      last_forwardable_ = outer_hyperlink_uri_forwardable(fetched);
    }
    return last_forwardable_ ? last_ : 0;
  }

  // The URI of the last link resolve returned, readable until the next resolve.
  [[nodiscard]] auto uri() const noexcept -> std::span<const std::uint8_t> {
    return std::span<const std::uint8_t>(std::span(uri_scratch()).subspan(current_, 1).front())
        .first(size_);
  }

private:
  using UriScratch = std::array<std::uint8_t, limits::outer_hyperlink_uri_bytes_max>;

  // The last URI and the next one, per thread. Kept off the per-row decoder so link-free rows
  // neither clear nor stack-probe them.
  [[nodiscard]] static auto uri_scratch() noexcept -> std::array<UriScratch, 2>& {
    thread_local std::array<UriScratch, 2> scratch{};
    return scratch;
  }

  GhosttyTerminal terminal_;
  const FingerprintKey* key_;
  std::uint16_t row_{0};
  std::size_t columns_{0};
  GhosttyGridRef row_ref_{};
  bool row_resolved_{false};
  bool row_valid_{false};
  std::size_t current_{0};
  std::size_t size_{0};
  // Nonzero fingerprint of the URI in the current buffer, 0 before the row's first link.
  std::uint64_t last_{0};
  bool last_forwardable_{false};
};

// The projected presentation of one cell. Style includes selection highlighting; grapheme bytes
// are borrowed from the owning RowCellDecoder until its next decode. Link state stays in the
// decoder: every decode assigns this value, which must stay small enough to copy inline.
struct DecodedCell final {
  AnsiStyle style{};
  std::uint64_t hash{0};
  std::span<const std::uint8_t> grapheme;
  GhosttyCellWide wide{GHOSTTY_CELL_WIDE_NARROW};
  GhosttyCellContentTag content_tag{GHOSTTY_CELL_CONTENT_CODEPOINT};
  bool selected{false};
  bool native_default{false};
};
static_assert(sizeof(DecodedCell) <= 64);

// One row traversal over Ghostty's render state. The bulk raw-cell view supplies every column with
// one call. Within a row, equal raw values share page, style ID, width and content, so an adjacent
// repeat reuses the previous decode unless it refers to extra grapheme storage or its selection
// differs. Only styles that miss the row projection and non-ASCII text use per-cell accessors.
class RowCellDecoder final {
public:
  // Hyperlinks are resolved only when presented; otherwise linked cells project as plain text.
  RowCellDecoder(const GhosttyRenderStateColors& colors, const TerminalTheme& theme,
                 const FingerprintKey& key, const GhosttyTerminal terminal,
                 const bool hyperlinks) noexcept
      : colors_(&colors), theme_(&theme), hasher_(key), links_(terminal, key),
        hyperlinks_(hyperlinks) {}

  [[nodiscard]] auto open(const GhosttyRenderStateRowIterator row,
                          GhosttyRenderStateRowCells& cells, const std::size_t row_index) noexcept
      -> std::expected<void, Error> {
    auto result = ghostty_render_state_row_get(row, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                               static_cast<void*>(&cells));
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
    GhosttyCellsView view{};
    result = ghostty_render_state_row_get(row, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS_RAW, &view);
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
    const auto selection = selected_columns(row);
    if (!selection.has_value()) {
      return std::unexpected(selection.error());
    }
    const auto flags = row_flags(row, *selection, hyperlinks_);
    if (!flags.has_value()) {
      return std::unexpected(flags.error());
    }
    raw_ = std::span(view.ptr, view.len);
    selection_ = *selection;
    plain_ = flags->plain;
    linked_row_ = flags->linked;
    cell_linked_ = false;
    link_ = 0;
    LEMMA_ASSERT(row_index <= std::numeric_limits<std::uint16_t>::max());
    links_.reset(static_cast<std::uint16_t>(row_index), raw_.size());
    cursor_.reset(cells);
    // Style IDs are page-local; never carry a projection into another row.
    styles_ = {};
    decoded_ = false;
    return {};
  }

  [[nodiscard]] auto columns() const noexcept -> std::size_t { return raw_.size(); }
  // The last decode returned the previous column's decode unchanged, so it has the same style.
  [[nodiscard]] auto repeated() const noexcept -> bool { return repeated_; }
  [[nodiscard]] auto raw() const noexcept -> std::span<const GhosttyCell> { return raw_; }
  // Ghostty's row flags have no false negatives: without styled or grapheme cells, every cell uses
  // style ID 0 and keeps its complete content in the raw value. Without selection or presented
  // hyperlinks (whose URIs are outside the raw value), nothing else row-specific enters its
  // projection.
  [[nodiscard]] auto plain() const noexcept -> bool { return plain_; }
  // The row may carry hyperlinks that are presented; otherwise link() is always 0.
  [[nodiscard]] auto linked_row() const noexcept -> bool { return linked_row_; }
  // Keyed identity of the last decoded cell's forwardable OSC 8 link, 0 for none.
  [[nodiscard]] auto link() const noexcept -> std::uint64_t { return link_; }
  // URI of that link, borrowed until the next decode.
  [[nodiscard]] auto link_uri() const noexcept -> std::span<const std::uint8_t> {
    return links_.uri();
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto decode(const std::size_t column) noexcept
      -> std::expected<const DecodedCell*, Error> {
    LEMMA_ASSERT(column < raw_.size());
    const auto raw_cell = raw_.subspan(column, 1).front();
    const bool selected = selection_.contains(column);
    repeated_ = decoded_ && raw_cell == raw_cell_ && selected == cell_.selected &&
                cell_.content_tag != GHOSTTY_CELL_CONTENT_CODEPOINT_GRAPHEME;
    if (repeated_) {
      return &cell_;
    }
    const bool follows_decode = decoded_;
    const bool previous_selected = cell_.selected;
    decoded_ = false;

    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
    GhosttyCellContentTag content_tag = GHOSTTY_CELL_CONTENT_CODEPOINT;
    GhosttyStyleId style_id = 0;
    std::uint32_t codepoint = 0;
    const std::array keys{GHOSTTY_CELL_DATA_WIDE, GHOSTTY_CELL_DATA_CONTENT_TAG,
                          GHOSTTY_CELL_DATA_STYLE_ID, GHOSTTY_CELL_DATA_CODEPOINT};
    std::array<void*, keys.size()> values{&wide, &content_tag, &style_id, &codepoint};
    std::size_t written = 0;
    const auto result =
        ghostty_cell_get_multi(raw_cell, keys.size(), keys.data(), values.data(), &written);
    if (result != GHOSTTY_SUCCESS || written != keys.size()) {
      return std::unexpected(detail::map_error(result));
    }
    auto style =
        styles_.resolve(raw_cell, content_tag, style_id, cursor_, column, *colors_, *theme_);
    if (!style.has_value()) {
      return std::unexpected(style.error());
    }
    const AnsiStyle* presented = *style;
    if (selected) {
      selected_style_ = *presented;
      apply_selection_highlight(selected_style_, true, *theme_);
      presented = &selected_style_;
    }
    const auto grapheme = graphemes(column, content_tag, codepoint);
    if (!grapheme.has_value()) {
      return std::unexpected(grapheme.error());
    }
    // The previous decode resolved the same projection with the same selection, so the hasher's
    // cached style is this style; skip comparing it field by field.
    const bool style_repeated = follows_decode && styles_.hit() && selected == previous_selected;
    cell_ = {
        .style = *presented,
        .hash = hasher_.hash(*presented, style_repeated, wide, *grapheme),
        .grapheme = *grapheme,
        .wide = wide,
        .content_tag = content_tag,
        .selected = selected,
        .native_default = styles_.native_default(),
    };
    raw_cell_ = raw_cell;
    decoded_ = true;
    return &cell_;
  }

  // Folds the OSC 8 link of the cell the last decode returned into its fingerprint. Only rows that
  // present links call it, so decoding link-free rows is unchanged. A repeated decode shares the
  // raw hyperlink flag with the previous cell, but not necessarily its link.
  [[nodiscard, gnu::noinline]] auto link_cell(const std::size_t column) noexcept
      -> std::expected<void, Error> {
    LEMMA_ASSERT(linked_row_ && decoded_);
    if (!repeated_) {
      const auto result =
          ghostty_cell_get(raw_cell_, GHOSTTY_CELL_DATA_HAS_HYPERLINK, &cell_linked_);
      if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(detail::map_error(result));
      }
      unlinked_hash_ = cell_.hash;
      link_ = 0;
    }
    if (cell_linked_) {
      const auto link = links_.resolve(column);
      if (link != link_) {
        link_ = link;
        cell_.hash = link == 0 ? unlinked_hash_ : hasher_.with_link(unlinked_hash_, link);
      }
    }
    return {};
  }

private:
  struct RowFlags final {
    bool plain{false};
    bool linked{false};
  };

  [[nodiscard]] static auto row_flags(const GhosttyRenderStateRowIterator row,
                                      const SelectedColumns selection,
                                      const bool hyperlinks) noexcept
      -> std::expected<RowFlags, Error> {
    const bool selected = selection.begin != selection.end;
    if (selected && !hyperlinks) {
      return RowFlags{};
    }
    GhosttyRow raw_row = 0;
    auto result = ghostty_render_state_row_get(row, GHOSTTY_RENDER_STATE_ROW_DATA_RAW, &raw_row);
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
    // The hyperlink flag may be a false positive, whose cells then resolve no link.
    bool linked = false;
    if (hyperlinks) {
      result = ghostty_row_get(raw_row, GHOSTTY_ROW_DATA_HYPERLINK, &linked);
      if (result != GHOSTTY_SUCCESS) {
        return std::unexpected(detail::map_error(result));
      }
    }
    if (selected || linked) {
      return RowFlags{.plain = false, .linked = linked};
    }
    // Most redrawn rows of styled applications are styled; ask for graphemes only when needed.
    bool flag = true;
    result = ghostty_row_get(raw_row, GHOSTTY_ROW_DATA_STYLED, &flag);
    if (result == GHOSTTY_SUCCESS && !flag) {
      result = ghostty_row_get(raw_row, GHOSTTY_ROW_DATA_GRAPHEME, &flag);
    }
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
    return RowFlags{.plain = !flag, .linked = false};
  }

  [[nodiscard]] auto graphemes(const std::size_t column, const GhosttyCellContentTag content_tag,
                               const std::uint32_t codepoint) noexcept
      -> std::expected<std::span<const std::uint8_t>, Error> {
    const bool text = content_tag == GHOSTTY_CELL_CONTENT_CODEPOINT ||
                      content_tag == GHOSTTY_CELL_CONTENT_CODEPOINT_GRAPHEME;
    if (!text || codepoint == 0) {
      return std::span<const std::uint8_t>{};
    }
    // Ghostty's own UTF-8 fast path: a lone ASCII codepoint is its single byte.
    constexpr std::uint32_t ascii_end = 0x80;
    if (content_tag == GHOSTTY_CELL_CONTENT_CODEPOINT && codepoint < ascii_end) {
      grapheme_.front() = static_cast<std::uint8_t>(codepoint);
      return std::span<const std::uint8_t>(grapheme_).first(1);
    }
    const auto cells = cursor_.at(column);
    if (!cells.has_value()) {
      return std::unexpected(cells.error());
    }
    // The API writes the returned length; bytes beyond it are never read. Reuse row-local
    // scratch instead of clearing the maximum-size grapheme buffer for every cell.
    GhosttyBuffer buffer{.ptr = grapheme_.data(), .cap = grapheme_.size(), .len = 0};
    const auto result = ghostty_render_state_row_cells_get(
        *cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buffer);
    if (result == GHOSTTY_OUT_OF_SPACE) {
      return std::unexpected(Error::limit_exceeded);
    }
    if (result != GHOSTTY_SUCCESS) {
      return std::unexpected(detail::map_error(result));
    }
    return std::span<const std::uint8_t>(grapheme_).first(buffer.len);
  }

  const GhosttyRenderStateColors* colors_;
  const TerminalTheme* theme_;
  std::span<const GhosttyCell> raw_;
  SelectedColumns selection_{};
  bool plain_{false};
  bool linked_row_{false};
  RowCellCursor cursor_;
  RowStyleProjection styles_;
  RenderedCellHasher hasher_;
  RowLinks links_;
  bool hyperlinks_;
  DecodedCell cell_{};
  // The last decoded cell carries Ghostty's hyperlink flag, its presented link, and its fingerprint
  // before that link was folded in. Maintained only in rows that present links.
  bool cell_linked_{false};
  std::uint64_t link_{0};
  std::uint64_t unlinked_hash_{0};
  GhosttyCell raw_cell_{0};
  bool decoded_{false};
  bool repeated_{false};
  AnsiStyle selected_style_{};
  std::array<std::uint8_t, pane_ansi_grapheme_bytes_max> grapheme_{};
};

} // namespace

[[nodiscard]] auto Terminal::Impl::dirty_state() const noexcept
    -> std::expected<DirtyState, Error> {
  GhosttyRenderStateDirty ghostty_dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
  const auto result =
      ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_DIRTY, &ghostty_dirty);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  switch (ghostty_dirty) {
  case GHOSTTY_RENDER_STATE_DIRTY_FALSE:
    return DirtyState::clean;
  case GHOSTTY_RENDER_STATE_DIRTY_PARTIAL:
    return DirtyState::partial;
  case GHOSTTY_RENDER_STATE_DIRTY_FULL:
    return DirtyState::full;
  case GHOSTTY_RENDER_STATE_DIRTY_MAX_VALUE:
    return std::unexpected(Error::invalid_state);
  }
  return std::unexpected(Error::invalid_state);
}

[[nodiscard]] auto Terminal::Impl::set_dirty_state(const DirtyState dirty) const noexcept
    -> std::expected<void, Error> {
  auto ghostty_dirty = [dirty]() noexcept {
    switch (dirty) {
    case DirtyState::clean:
      return GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    case DirtyState::partial:
      return GHOSTTY_RENDER_STATE_DIRTY_PARTIAL;
    case DirtyState::full:
      return GHOSTTY_RENDER_STATE_DIRTY_FULL;
    }
    return GHOSTTY_RENDER_STATE_DIRTY_FALSE;
  }();
  const auto result =
      ghostty_render_state_set(render_state, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &ghostty_dirty);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return {};
}

[[nodiscard]] auto Terminal::Impl::populate_render_metadata(RenderUpdate& update) noexcept
    -> std::expected<void, Error> {
  const std::array keys{
      GHOSTTY_RENDER_STATE_DATA_COLS,
      GHOSTTY_RENDER_STATE_DATA_ROWS,
  };
  std::array<void*, keys.size()> values{&update.columns, &update.rows};
  std::size_t values_written = 0;
  auto result = ghostty_render_state_get_multi(render_state, keys.size(), keys.data(),
                                               values.data(), &values_written);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  if (values_written != keys.size()) {
    return std::unexpected(Error::invalid_state);
  }

  GhosttyRenderStateCursor cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
  result = ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  update.cursor_visible = cursor.visible;
  update.cursor_in_viewport = cursor.viewport_has_value;
  update.cursor_column = cursor.viewport_has_value ? cursor.viewport_x : std::uint16_t{0};
  update.cursor_row = cursor.viewport_has_value ? cursor.viewport_y : std::uint16_t{0};
  render_cursor_style = cursor.visual_style;
  render_cursor_blinking = cursor.blinking;
  return {};
}

[[nodiscard]] auto Terminal::Impl::dirty_row_count() noexcept -> std::expected<std::size_t, Error> {
  auto result = ghostty_render_state_get(render_state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                         static_cast<void*>(&row_iterator));
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  std::size_t count = 0;
  std::uint16_t row_y = 0;
  while (ghostty_render_state_row_iterator_next_dirty(row_iterator, &row_y)) {
    ++count;
  }
  return count;
}

// Grapheme/style hashing is intentionally explicit so unsafe scroll equivalence is never inferred.
[[nodiscard]] auto Terminal::Impl::calculate_row_hash(const std::size_t row_index) noexcept
    -> std::expected<std::uint64_t, Error> {
  return calculate_row_hash_as<false>(row_index);
}

[[nodiscard]] auto Terminal::Impl::calculate_linked_row_hash(const std::size_t row_index) noexcept
    -> std::expected<std::uint64_t, Error> {
  return calculate_row_hash_as<true>(row_index);
}

template <bool Links>
[[nodiscard]] auto Terminal::Impl::calculate_row_hash_as(const std::size_t row_index) noexcept
    -> std::expected<std::uint64_t, Error> {
  RowCellDecoder decoder(render_colors, session_theme, fingerprint_key, terminal, ansi_hyperlinks);
  const auto opened = decoder.open(row_iterator, row_cells, row_index);
  if (!opened.has_value()) {
    return std::unexpected(opened.error());
  }
  if constexpr (!Links) {
    if (decoder.linked_row()) {
      return calculate_linked_row_hash(row_index);
    }
  }
  LEMMA_ASSERT(decoder.columns() == options.size.columns);
  if (decoder.plain()) {
    return RowFingerprint::of(fingerprint_key, fingerprint_key.plain_lanes, decoder.raw(),
                              plain_row_color_epoch);
  }
  // Decoded cell fingerprints already cover the projected colors.
  std::uint64_t row_hash = fingerprint_key.initial;
  for (std::size_t column = 0; column < decoder.columns(); ++column) {
    const auto decoded = decoder.decode(column);
    if (!decoded.has_value()) {
      return std::unexpected(decoded.error());
    }
    if constexpr (Links) {
      const auto linked = decoder.link_cell(column);
      if (!linked.has_value()) {
        return std::unexpected(linked.error());
      }
    }
    row_hash = fingerprint_mix(fingerprint_key, row_hash, (*decoded)->hash);
  }
  return row_hash;
}

[[nodiscard]] auto Terminal::Impl::detect_scroll() const noexcept -> std::int32_t {
  if (row_hash_count < 3) {
    return 0;
  }
  const auto previous = row_hashes();
  const auto current = current_row_hashes();
  // Rows near the edges usually decide a candidate: a frame that scrolled nothing typically differs
  // only in its last rows. Compare both ends before the whole overlap so rejecting every amount
  // stays linear in the row count instead of comparing nearly every row for each amount.
  const auto aligned = [](const std::span<const std::uint64_t> moved,
                          const std::span<const std::uint64_t> retained) noexcept {
    return moved.back() == retained.back() && moved.front() == retained.front() &&
           std::ranges::equal(moved, retained);
  };
  for (std::size_t amount = 1; amount + 1 < row_hash_count; ++amount) {
    const auto overlap = row_hash_count - amount;
    if (aligned(current.first(overlap), previous.subspan(amount))) {
      return static_cast<std::int32_t>(amount);
    }
    if (aligned(current.subspan(amount), previous.first(overlap))) {
      return -static_cast<std::int32_t>(amount);
    }
  }
  // Output usually writes the cursor row before a newline scrolls it into the overlap, so the
  // overlap row nearest the rows scrolled in may differ; it is re-encoded like any changed row.
  // Such a scroll must still keep more rows than presenting in place, and the kept rows only
  // shrink as the amount grows.
  std::size_t in_place = 0;
  for (std::size_t row = 0; row < row_hash_count; ++row) {
    in_place += static_cast<std::size_t>(current.subspan(row, 1).front() ==
                                         previous.subspan(row, 1).front());
  }
  for (std::size_t amount = 1; amount + 1 < row_hash_count; ++amount) {
    const auto kept = row_hash_count - amount - 1U;
    if (kept <= in_place) {
      break;
    }
    if (aligned(current.first(kept), previous.subspan(amount).first(kept))) {
      return static_cast<std::int32_t>(amount);
    }
    if (aligned(current.subspan(amount + 1U), previous.subspan(1, kept))) {
      return -static_cast<std::int32_t>(amount);
    }
  }
  return 0;
}

void Terminal::Impl::apply_physical_scroll(const std::int32_t scroll) noexcept {
  LEMMA_ASSERT(scroll != 0);
  const auto amount = static_cast<std::size_t>(scroll > 0 ? scroll : -scroll);
  const auto columns = static_cast<std::size_t>(options.size.columns);
  const auto shifted_cells = amount * columns;
  auto cells = std::span(physical_cell_hashes.get(), physical_cell_count);
  auto hashes = row_hashes();
  if (scroll > 0) {
    std::memmove(cells.data(), cells.subspan(shifted_cells).data(),
                 (cells.size() - shifted_cells) * sizeof(std::uint64_t));
    std::fill(cells.end() - static_cast<std::ptrdiff_t>(shifted_cells), cells.end(), 0);
    std::memmove(hashes.data(), hashes.subspan(amount).data(),
                 (hashes.size() - amount) * sizeof(std::uint64_t));
    std::fill(hashes.end() - static_cast<std::ptrdiff_t>(amount), hashes.end(), 0);
    return;
  }
  std::memmove(cells.subspan(shifted_cells).data(), cells.data(),
               (cells.size() - shifted_cells) * sizeof(std::uint64_t));
  std::fill(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(shifted_cells), 0);
  std::memmove(hashes.subspan(amount).data(), hashes.data(),
               (hashes.size() - amount) * sizeof(std::uint64_t));
  std::fill(hashes.begin(), hashes.begin() + static_cast<std::ptrdiff_t>(amount), 0);
}

[[nodiscard]] auto
Terminal::Impl::encode_row(AnsiWriter& writer, const std::size_t row_index, const bool force,
                           const bool probe_unchanged, const std::uint16_t origin_column,
                           const std::uint16_t origin_row, const bool erase_line_tail) noexcept
    -> std::expected<detail::RowEncoding, Error> {
  return encode_row_as<false>(writer, row_index, force, probe_unchanged, origin_column, origin_row,
                              erase_line_tail);
}

[[nodiscard]] auto Terminal::Impl::encode_linked_row(
    AnsiWriter& writer, const std::size_t row_index, const bool force, const bool probe_unchanged,
    const std::uint16_t origin_column, const std::uint16_t origin_row,
    const bool erase_line_tail) noexcept -> std::expected<detail::RowEncoding, Error> {
  return encode_row_as<true>(writer, row_index, force, probe_unchanged, origin_column, origin_row,
                             erase_line_tail);
}

// Encode the minimal prefix/suffix-differing span while refreshing bounded physical state.
template <bool Links>
[[nodiscard]] auto
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Terminal::Impl::encode_row_as(AnsiWriter& writer, const std::size_t row_index, const bool force,
                              const bool probe_unchanged, const std::uint16_t origin_column,
                              const std::uint16_t origin_row, const bool erase_line_tail) noexcept
    -> std::expected<detail::RowEncoding, Error> {
  LEMMA_ASSERT(row_index < row_hash_count);
  LEMMA_ASSERT(physical_cell_hashes != nullptr);
  const auto checkpoint = writer.size();
  RowCellDecoder decoder(render_colors, session_theme, fingerprint_key, terminal, ansi_hyperlinks);
  const auto opened = decoder.open(row_iterator, row_cells, row_index);
  if (!opened.has_value()) {
    return std::unexpected(opened.error());
  }
  if constexpr (!Links) {
    if (decoder.linked_row()) {
      return encode_linked_row(writer, row_index, force, probe_unchanged, origin_column, origin_row,
                               erase_line_tail);
    }
  }
  LEMMA_ASSERT(decoder.columns() == options.size.columns);

  const bool plain = decoder.plain();
  // A redraw marks every row dirty even when most are unchanged, for example when a pane that does
  // not fill the viewport scrolls identical lines. A plain row proves that without its cells.
  std::optional<std::uint64_t> probed;
  if (plain && probe_unchanged && !force) {
    probed = RowFingerprint::of(fingerprint_key, fingerprint_key.plain_lanes, decoder.raw(),
                                plain_row_color_epoch);
    if (*probed == row_hashes().subspan(row_index, 1).front()) {
      return detail::RowEncoding::matched;
    }
  }
  std::uint64_t row_hash = fingerprint_key.initial;
  RowFingerprint plain_fingerprint(fingerprint_key, fingerprint_key.plain_lanes);
  AnsiStyle active_style{};
  bool active_style_valid = false;
  bool span_started = false;
  // The outer terminal's open OSC 8 link, and whether one is open at changed_end. Every row closes
  // its link, so none leaks into later positioning, other Panes, or composition.
  std::uint64_t active_link = 0;
  bool link_open_at_changed_end = false;
  std::size_t changed_end = checkpoint;
  std::size_t trailing_blank_start = std::numeric_limits<std::size_t>::max();
  std::size_t trailing_blank_content_start = 0;
  std::size_t trailing_blank_column = 0;
  AnsiStyle trailing_blank_style{};
  bool trailing_blank_changed = false;
  for (std::size_t cell_count = 0; cell_count < decoder.columns(); ++cell_count) {
    const auto decoded = decoder.decode(cell_count);
    if (!decoded.has_value()) {
      return std::unexpected(decoded.error());
    }
    if constexpr (Links) {
      const auto linked = decoder.link_cell(cell_count);
      if (!linked.has_value()) {
        return std::unexpected(linked.error());
      }
    }
    const auto& cell = **decoded;
    const auto& style = cell.style;
    const auto wide = cell.wide;
    const auto content_tag = cell.content_tag;
    const auto grapheme_bytes = cell.grapheme;
    const auto cell_hash = cell.hash;
    if (!plain) {
      row_hash = fingerprint_mix(fingerprint_key, row_hash, cell_hash);
    } else if (!probed.has_value()) {
      plain_fingerprint.add(cell_count, decoder.raw().subspan(cell_count, 1).front());
    }
    const auto physical_index = (row_index * options.size.columns) + cell_count;
    LEMMA_ASSERT(physical_index < physical_cell_count);
    auto physical_cells = std::span(physical_cell_hashes.get(), physical_cell_count);
    auto& physical_hash = physical_cells.subspan(physical_index, 1).front();
    const bool changed = force || !ansi_physical_valid || physical_hash != cell_hash;
    physical_hash = cell_hash;
    if (span_started || changed) {
      // Within a span the previous column set the active style; a repeated decode shares it.
      const bool style_active = span_started && decoder.repeated();
      if (!span_started) {
        if (!writer.append("\x1B[") ||
            !writer.append_integer(static_cast<std::size_t>(origin_row) + row_index + 1U) ||
            !writer.append(";") ||
            !writer.append_integer(static_cast<std::size_t>(origin_column) + cell_count + 1U) ||
            !writer.append("H")) {
          return std::unexpected(Error::out_of_space);
        }
        span_started = true;
      }
      // The outer terminal links a wide character's spacer tail with its head. Link changes precede
      // the cell checkpoint, so a blank tail (never linked) always begins with no link open.
      if constexpr (Links) {
        if (decoder.link() != active_link && wide != GHOSTTY_CELL_WIDE_SPACER_TAIL &&
            !present_link(writer, active_link, decoder.link(), decoder.link_uri(),
                          graphics_identity, hyperlink_bytes_remaining)) {
          return std::unexpected(Error::out_of_space);
        }
      }

      const auto cell_checkpoint = writer.size();
      if (!style_active) {
        if ((!active_style_valid || style != active_style) && !append_style(writer, style)) {
          return std::unexpected(Error::out_of_space);
        }
        active_style = style;
        active_style_valid = true;
      }

      const bool default_blank = !cell.selected && grapheme_bytes.empty() &&
                                 (!Links || decoder.link() == 0) &&
                                 wide != GHOSTTY_CELL_WIDE_SPACER_TAIL && cell.native_default &&
                                 content_tag != GHOSTTY_CELL_CONTENT_BG_COLOR_PALETTE &&
                                 content_tag != GHOSTTY_CELL_CONTENT_BG_COLOR_RGB;
      if (default_blank) {
        if (trailing_blank_start == std::numeric_limits<std::size_t>::max()) {
          trailing_blank_start = cell_checkpoint;
          trailing_blank_content_start = writer.size();
          trailing_blank_column = cell_count;
          trailing_blank_changed = false;
        }
        trailing_blank_style = style;
        trailing_blank_changed = trailing_blank_changed || changed;
      } else {
        trailing_blank_start = std::numeric_limits<std::size_t>::max();
        trailing_blank_changed = false;
      }

      // Project placeholders natively; outer image IDs belong to the attachment,
      // not this Pane. Never let the outer terminal interpret these cells again.
      constexpr std::array<std::uint8_t, 4> placeholder{0xF4, 0x8E, 0xBB, 0xAE};
      const bool graphics_placeholder =
          grapheme_bytes.size() >= placeholder.size() &&
          std::ranges::equal(grapheme_bytes.first(placeholder.size()), placeholder);
      if (grapheme_bytes.empty() || graphics_placeholder) {
        if (wide != GHOSTTY_CELL_WIDE_SPACER_TAIL && !writer.append(" ")) {
          return std::unexpected(Error::out_of_space);
        }
      } else if (wide != GHOSTTY_CELL_WIDE_SPACER_TAIL) {
        const auto base_bytes = utf8_codepoint_bytes(grapheme_bytes.front());
        const bool last_column_with_suffix =
            cell_count + 1U == options.size.columns && base_bytes < grapheme_bytes.size();
        if (!last_column_with_suffix) {
          if (!writer.append(std::as_bytes(grapheme_bytes))) {
            return std::unexpected(Error::out_of_space);
          }
        } else {
          // The compositor normally disables autowrap. At the final column, Ghostty needs pending
          // wrap state while parsing suffix codepoints or it can attach them to the preceding wide
          // cell. Bound the exception to this one complete grapheme, then restore frame policy.
          if (!writer.append("\x1B[?7h") || !writer.append(std::as_bytes(grapheme_bytes)) ||
              !writer.append("\x1B[?7l")) {
            return std::unexpected(Error::out_of_space);
          }
        }
      }
      if (changed) {
        changed_end = writer.size();
        if constexpr (Links) {
          link_open_at_changed_end = active_link != 0;
        }
      }
    }
  }

  if (plain) {
    row_hash = probed.has_value()
                   ? *probed
                   : plain_fingerprint.finish(decoder.columns(), plain_row_color_epoch);
  }
  row_hashes().subspan(row_index, 1).front() = row_hash;
  if (!span_started) {
    LEMMA_ASSERT(writer.size() == checkpoint);
    return detail::RowEncoding::unchanged;
  }
  if (erase_line_tail && trailing_blank_start != std::numeric_limits<std::size_t>::max() &&
      trailing_blank_changed) {
    writer.rewind(trailing_blank_start);
    // EL paints with the active background. Re-emit the pane's semantic default style so the
    // attaching terminal supplies its own default unless the pane has an OSC 10/11 override.
    if (!append_style(writer, trailing_blank_style) || !writer.append("\x1B[K")) {
      return std::unexpected(Error::out_of_space);
    }
  } else if (trailing_blank_start != std::numeric_limits<std::size_t>::max() &&
             trailing_blank_changed && options.size.columns - trailing_blank_column > 8U) {
    // A composed pane cannot use EL: it would erase its right-hand neighbor. ECH clears
    // only this pane's blank tail and retains the already-emitted semantic default style.
    // Short tails stay literal; the bounded ECH sequence costs at most six bytes.
    writer.rewind(trailing_blank_content_start);
    if (!writer.append("\x1B[") ||
        !writer.append_integer(options.size.columns - trailing_blank_column) ||
        !writer.append("X")) {
      return std::unexpected(Error::out_of_space);
    }
  } else {
    writer.rewind(changed_end);
    if (Links && link_open_at_changed_end && !writer.append(hyperlink_close)) {
      return std::unexpected(Error::out_of_space);
    }
  }
  return detail::RowEncoding::emitted;
}

auto Terminal::update_render_state() noexcept -> std::expected<RenderUpdate, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->render_state != nullptr);

  const auto result = ghostty_render_state_update(impl_->render_state, impl_->terminal);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }

  RenderUpdate update{};
  const auto dirty = impl_->dirty_state();
  if (!dirty.has_value()) {
    return std::unexpected(dirty.error());
  }
  update.dirty = *dirty;

  const auto metadata_result = impl_->populate_render_metadata(update);
  if (!metadata_result.has_value()) {
    return std::unexpected(metadata_result.error());
  }

  const auto dirty_rows = impl_->dirty_row_count();
  if (!dirty_rows.has_value()) {
    return std::unexpected(dirty_rows.error());
  }
  update.dirty_rows = *dirty_rows;

  LEMMA_ASSERT(update.columns == impl_->options.size.columns);
  LEMMA_ASSERT(update.rows == impl_->options.size.rows);
  return update;
}

auto Terminal::mark_rendered() noexcept -> std::expected<void, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->render_state != nullptr);

  const auto result = ghostty_render_state_clean(impl_->render_state);
  if (result != GHOSTTY_SUCCESS) {
    return std::unexpected(detail::map_error(result));
  }
  return {};
}

auto Terminal::render_ansi(const std::span<std::byte> output, const bool force_full) noexcept
    -> std::expected<AnsiRenderResult, Error> {
  return render_ansi_impl(output, force_full, 0, 0, false, true, false, 0, 0,
                          TerminalScroll::screen, true);
}

auto Terminal::render_pane_ansi(const std::span<std::byte> output,
                                const PaneRenderOptions& options) noexcept
    -> std::expected<AnsiRenderResult, Error> {
  return render_ansi_impl(output, options.force_full, options.column, options.row, true,
                          options.focused, options.cursor_override, options.cursor_override_column,
                          options.cursor_override_row, options.terminal_scroll, options.hyperlinks);
}

void Terminal::invalidate_ansi_render_state() noexcept {
  LEMMA_ASSERT(impl_ != nullptr);
  impl_->ansi_physical_valid = false;
  impl_->mirrored_modes_valid = false;
  impl_->mirrored_compositor_modes_valid = false;
  impl_->projected_cursor_valid = false;
}

void Terminal::release_render_cache() noexcept {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->render_state != nullptr);
  std::uint16_t rows = 0;
  if (ghostty_render_state_get(impl_->render_state, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows) ==
          GHOSTTY_SUCCESS &&
      rows == 0) {
    return;
  }
  GhosttyRenderState replacement{nullptr};
  if (ghostty_render_state_new(impl_->allocator.native(), &replacement) != GHOSTTY_SUCCESS) {
    return;
  }
  ghostty_render_state_free(impl_->render_state);
  impl_->render_state = replacement;
  impl_->physical_cell_hashes.reset();
  impl_->physical_cell_capacity = 0;
  invalidate_ansi_render_state();
}

void Terminal::invalidate_ansi_mode_projection() noexcept {
  LEMMA_ASSERT(impl_ != nullptr);
  impl_->mirrored_modes_valid = false;
  impl_->mirrored_compositor_modes_valid = false;
}

void Terminal::invalidate_ansi_cursor_projection() noexcept {
  LEMMA_ASSERT(impl_ != nullptr);
  impl_->projected_cursor_valid = false;
}

// Rendering is an explicit bounded pass over rows and cells owned by Ghostty's snapshot.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::render_ansi_impl(const std::span<std::byte> output, const bool force_full,
                                const std::uint16_t origin_column, const std::uint16_t origin_row,
                                const bool composed, const bool focused, const bool cursor_override,
                                const std::uint16_t cursor_override_column,
                                const std::uint16_t cursor_override_row,
                                const TerminalScroll terminal_scroll,
                                const bool hyperlinks) noexcept
    -> std::expected<AnsiRenderResult, Error> {
  LEMMA_ASSERT(impl_ != nullptr);
  LEMMA_ASSERT(impl_->render_state != nullptr);
  if (impl_->physical_cell_hashes == nullptr) {
    // First presentation, or the first since the render cache was released: nothing is physically
    // valid, so the shadow is rebuilt in full at the current geometry.
    LEMMA_ASSERT(!impl_->ansi_physical_valid);
    try {
      // Runtime-sized cell storage cannot use std::array.
      // NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
      impl_->physical_cell_hashes =
          std::make_unique_for_overwrite<std::uint64_t[]>(impl_->physical_cell_count);
      // NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    } catch (const std::bad_alloc&) {
      return std::unexpected(Error::out_of_memory);
    }
    impl_->physical_cell_capacity = impl_->physical_cell_count;
  }

  auto result = ghostty_render_state_update(impl_->render_state, impl_->terminal);
  if (result != GHOSTTY_SUCCESS) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(detail::map_error(result));
  }

  RenderUpdate metadata{};
  const auto metadata_result = impl_->populate_render_metadata(metadata);
  if (!metadata_result.has_value()) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(metadata_result.error());
  }
  const auto dirty = impl_->dirty_state();
  if (!dirty.has_value()) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(dirty.error());
  }
  if (focused) {
    diagnostic::record_latency_trace(diagnostic::LatencyTraceStage::ghostty_damage_reported, 0,
                                     static_cast<std::uint64_t>(*dirty));
  }
  if (impl_->ansi_hyperlinks != hyperlinks) {
    // Physical state was presented under the other hyperlink policy; cell fingerprints include
    // links only while they are presented.
    impl_->ansi_physical_valid = false;
    impl_->ansi_hyperlinks = hyperlinks;
  }
  impl_->hyperlink_bytes_remaining =
      hyperlinks ? impl_->physical_cell_count * pane_ansi_hyperlink_bytes_per_cell : 0;
  const bool full = force_full || !impl_->ansi_physical_valid;
  // Cursor/default colors can change without cell damage, so every frame acquires the scalar
  // prefix. Ghostty guarantees palette mutations force redraw; clean frames can therefore avoid
  // copying the 256-entry suffix while retaining the previously acquired palette.
  const bool palette_acquired = full || *dirty != DirtyState::clean;
  // Only a redraw can carry a palette change, so partial frames need not compare it.
  const bool palette_may_change = full || *dirty == DirtyState::full;
  const auto previous_foreground = impl_->render_colors.foreground;
  const auto previous_background = impl_->render_colors.background;
  // Only read after being copied below; clearing it would cost every partial frame.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  std::array<GhosttyColorRgb, std::size(GhosttyRenderStateColors{}.palette)> previous_palette;
  if (palette_may_change) {
    std::ranges::copy(impl_->render_colors.palette, previous_palette.begin());
  }
  impl_->render_colors.size =
      palette_acquired ? sizeof(impl_->render_colors) : offsetof(GhosttyRenderStateColors, palette);
  result = ghostty_render_state_get(impl_->render_state, GHOSTTY_RENDER_STATE_DATA_COLORS,
                                    &impl_->render_colors);
  if (result != GHOSTTY_SUCCESS) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(detail::map_error(result));
  }
  const auto same_rgb = [](const GhosttyColorRgb left, const GhosttyColorRgb right) noexcept {
    return left.r == right.r && left.g == right.g && left.b == right.b;
  };
  // Default and palette colors are the only frame inputs to a plain cell's projection besides
  // the theme, whose changes force a full frame that re-encodes every row.
  if (!same_rgb(previous_foreground, impl_->render_colors.foreground) ||
      !same_rgb(previous_background, impl_->render_colors.background) ||
      (palette_may_change &&
       !std::ranges::equal(previous_palette, impl_->render_colors.palette, same_rgb))) {
    ++impl_->plain_row_color_epoch;
  }

  AnsiWriter writer(output);
  if (!composed && (!writer.append("\x1B[?2026h\x1B[?25l\x1B[?7l") ||
                    (full && !writer.append("\x1B[2J\x1B[H")))) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(Error::out_of_space);
  }

  std::int32_t scrolled_rows = 0;
  bool rows_hashed = false;
  if (terminal_scroll != TerminalScroll::none && !full && *dirty == DirtyState::full) {
    result = ghostty_render_state_get(impl_->render_state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                      static_cast<void*>(&impl_->row_iterator));
    if (result != GHOSTTY_SUCCESS) {
      impl_->ansi_physical_valid = false;
      return std::unexpected(detail::map_error(result));
    }
    std::size_t hash_index = 0;
    while (ghostty_render_state_row_iterator_next(impl_->row_iterator)) {
      const auto hash = impl_->calculate_row_hash(hash_index);
      if (!hash.has_value()) {
        impl_->ansi_physical_valid = false;
        return std::unexpected(hash.error());
      }
      impl_->current_row_hashes().subspan(hash_index, 1).front() = *hash;
      ++hash_index;
    }
    LEMMA_ASSERT(hash_index == impl_->row_hash_count);
    rows_hashed = true;
    scrolled_rows = impl_->detect_scroll();
    if (scrolled_rows != 0) {
      const auto amount = scrolled_rows > 0 ? scrolled_rows : -scrolled_rows;
      // Vertical margins confine the scroll to this pane's rows; they home the cursor, and every
      // later row and the cursor are positioned absolutely. Margins are reset within the same
      // synchronized frame so no other output observes them.
      const bool margins = terminal_scroll == TerminalScroll::margins;
      LEMMA_ASSERT(!margins || origin_column == 0);
      if ((margins &&
           (!writer.append("\x1B[") ||
            !writer.append_integer(static_cast<std::size_t>(origin_row) + 1U) ||
            !writer.append(";") ||
            !writer.append_integer(static_cast<std::size_t>(origin_row) + impl_->row_hash_count) ||
            !writer.append("r"))) ||
          !writer.append("\x1B[") || !writer.append_integer(amount) ||
          !writer.append(scrolled_rows > 0 ? "S" : "T") || (margins && !writer.append("\x1B[r"))) {
        impl_->ansi_physical_valid = false;
        return std::unexpected(Error::out_of_space);
      }
      impl_->apply_physical_scroll(scrolled_rows);
    }
  }

  std::size_t rendered_rows = 0;
  std::size_t encoded_rows = 0;
  if (full || *dirty != DirtyState::clean) {
    result = ghostty_render_state_get(impl_->render_state, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                      static_cast<void*>(&impl_->row_iterator));
    if (result != GHOSTTY_SUCCESS) {
      impl_->ansi_physical_valid = false;
      return std::unexpected(detail::map_error(result));
    }

    const auto encode_changed_row =
        [&](const std::size_t row_index) noexcept -> std::expected<void, Error> {
      // Retained row hashes describe the physical row after any applied scroll. A row whose
      // current hash already matches needs no encoding pass, scrolled or not. Rows are hashed only
      // for incremental frames; a full frame (including the first after a released render cache)
      // re-encodes every row. A redraw that cannot scroll the terminal instead probes each plain
      // row the same way inside encode_row.
      const bool row_unchanged =
          rows_hashed && impl_->row_hashes().subspan(row_index, 1).front() ==
                             impl_->current_row_hashes().subspan(row_index, 1).front();
      if (row_unchanged) {
        return {};
      }
      const auto encoded =
          impl_->encode_row(writer, row_index, full, !rows_hashed && *dirty == DirtyState::full,
                            origin_column, origin_row, !composed);
      if (!encoded.has_value()) {
        return std::unexpected(encoded.error());
      }
      encoded_rows += static_cast<std::size_t>(*encoded != detail::RowEncoding::matched);
      rendered_rows += static_cast<std::size_t>(*encoded == detail::RowEncoding::emitted);
      return {};
    };

    if (full) {
      std::size_t row_index = 0;
      while (ghostty_render_state_row_iterator_next(impl_->row_iterator)) {
        const auto encoded = encode_changed_row(row_index);
        if (!encoded.has_value()) {
          impl_->ansi_physical_valid = false;
          return std::unexpected(encoded.error());
        }
        ++row_index;
      }
    } else {
      std::uint16_t row_index = 0;
      while (ghostty_render_state_row_iterator_next_dirty(impl_->row_iterator, &row_index)) {
        const auto encoded = encode_changed_row(row_index);
        if (!encoded.has_value()) {
          impl_->ansi_physical_valid = false;
          return std::unexpected(encoded.error());
        }
      }
    }
  }

  if (!writer.append(composed ? "\x1B[0m" : "\x1B[0m\x1B[?7h")) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(Error::out_of_space);
  }
  const bool canonical_cursor_visible = metadata.cursor_visible && metadata.cursor_in_viewport;
  const bool presented_cursor_visible = cursor_override || canonical_cursor_visible;
  const auto presented_cursor_column =
      cursor_override ? cursor_override_column : metadata.cursor_column;
  const auto presented_cursor_row = cursor_override ? cursor_override_row : metadata.cursor_row;
  if ((!composed || focused) && presented_cursor_visible) {
    if (!writer.append("\x1B[") ||
        !writer.append_integer(static_cast<std::size_t>(origin_row) + presented_cursor_row + 1U) ||
        !writer.append(";") ||
        !writer.append_integer(static_cast<std::size_t>(origin_column) + presented_cursor_column +
                               1U) ||
        !writer.append("H\x1B[?25h")) {
      impl_->ansi_physical_valid = false;
      return std::unexpected(Error::out_of_space);
    }
  }
  if ((!composed || focused) && !presented_cursor_visible && !writer.append("\x1B[?25l")) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(Error::out_of_space);
  }
  if (!composed || focused) {
    // Native copy-mode cursors use Lemma's steady block; child cursors keep their DECSCUSR shape.
    const auto cursor_code =
        cursor_override ? decscusr_steady_block
                        : decscusr_code(impl_->render_cursor_style, impl_->render_cursor_blinking);
    const auto cursor_color = impl_->render_colors.cursor_has_value
                                  ? impl_->render_colors.cursor
                                  : impl_->render_colors.foreground;
    const bool cursor_projection_changed = cursor_override || full ||
                                           !impl_->projected_cursor_valid ||
                                           impl_->projected_cursor_code != cursor_code ||
                                           impl_->projected_cursor_color.r != cursor_color.r ||
                                           impl_->projected_cursor_color.g != cursor_color.g ||
                                           impl_->projected_cursor_color.b != cursor_color.b;
    if (cursor_projection_changed) {
      if (!append_cursor_color(writer, cursor_color, impl_->session_theme.cursor) ||
          !writer.append("\x1B[") || !writer.append_integer(cursor_code) || !writer.append(" q")) {
        impl_->ansi_physical_valid = false;
        return std::unexpected(Error::out_of_space);
      }
      impl_->projected_cursor_color = cursor_color;
      impl_->projected_cursor_code = cursor_code;
      impl_->projected_cursor_valid = true;
    }
  }

  struct MirroredMode final {
    GhosttyMode mode;
    std::uint16_t number;
    bool compositor_owned{false};
  };
  const std::array mirrored_modes{
      MirroredMode{.mode = GHOSTTY_MODE_DECCKM, .number = 1},
      MirroredMode{.mode = GHOSTTY_MODE_X10_MOUSE, .number = 9, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_NORMAL_MOUSE, .number = 1000, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_BUTTON_MOUSE, .number = 1002, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_ANY_MOUSE, .number = 1003, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_FOCUS_EVENT, .number = 1004, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_UTF8_MOUSE, .number = 1005, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_SGR_MOUSE, .number = 1006, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_ALT_SCROLL, .number = 1007},
      MirroredMode{.mode = GHOSTTY_MODE_URXVT_MOUSE, .number = 1015, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_SGR_PIXELS_MOUSE, .number = 1016, .compositor_owned = true},
      MirroredMode{.mode = GHOSTTY_MODE_BRACKETED_PASTE, .number = 2004},
  };
  static_assert(mirrored_modes.size() == 12);
  if (!composed || focused) {
    std::size_t mode_index = 0;
    for (const auto mode : mirrored_modes) {
      // A composed frame receives normalized physical mouse input for both Lemma and the child,
      // and outer focus reports stay enabled so Lemma can derive per-Pane focus changes. The
      // compositor owns those outer modes; only standalone rendering mirrors them directly.
      if (composed && mode.compositor_owned) {
        impl_->mirrored_compositor_modes_valid = false;
        ++mode_index;
        continue;
      }
      const auto enabled = terminal_mode_enabled(impl_->terminal, mode.mode);
      if (!enabled.has_value()) {
        impl_->ansi_physical_valid = false;
        return std::unexpected(enabled.error());
      }
      auto& physical_value = std::span(impl_->mirrored_mode_values).subspan(mode_index, 1).front();
      const bool must_emit = full || !impl_->mirrored_modes_valid ||
                             (mode.compositor_owned && !impl_->mirrored_compositor_modes_valid) ||
                             physical_value != *enabled;
      if (must_emit && (!writer.append("\x1B[?") || !writer.append_integer(mode.number) ||
                        !writer.append(*enabled ? "h" : "l"))) {
        impl_->ansi_physical_valid = false;
        return std::unexpected(Error::out_of_space);
      }
      physical_value = *enabled;
      ++mode_index;
    }
    impl_->mirrored_modes_valid = true;
    if (!composed) {
      impl_->mirrored_compositor_modes_valid = true;
    }
  }

  if (!composed && !writer.append("\x1B[?2026l")) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(Error::out_of_space);
  }

  result = ghostty_render_state_clean(impl_->render_state);
  if (result != GHOSTTY_SUCCESS) {
    impl_->ansi_physical_valid = false;
    return std::unexpected(detail::map_error(result));
  }

  impl_->ansi_physical_valid = true;
  return AnsiRenderResult{
      .bytes = writer.size(),
      .rows = rendered_rows,
      .encoded_rows = encoded_rows,
      .scrolled_rows = scrolled_rows,
      .full = full,
      .cursor =
          (!composed || focused) && presented_cursor_visible
              ? std::optional{AnsiCursorPosition{
                    .column = static_cast<std::uint16_t>(origin_column + presented_cursor_column),
                    .row = static_cast<std::uint16_t>(origin_row + presented_cursor_row)}}
              : std::nullopt,
  };
}

auto outer_hyperlink_uri_forwardable(const std::span<const std::uint8_t> uri) noexcept -> bool {
  if (uri.empty() || uri.size() > limits::outer_hyperlink_uri_bytes_max) {
    return false;
  }
  // Excludes C0, DEL, C1, and every non-ASCII byte, so the URI can neither terminate the sequence
  // nor carry controls the outer terminal might interpret.
  constexpr std::uint8_t printable_first = 0x20;
  constexpr std::uint8_t printable_last = 0x7E;
  if (!std::ranges::all_of(uri, [](const std::uint8_t byte) noexcept {
        return byte >= printable_first && byte <= printable_last;
      })) {
    return false;
  }
  // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), followed by ":".
  const auto alpha = [](const std::uint8_t byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
  };
  if (!alpha(uri.front())) {
    return false;
  }
  for (const auto byte : uri.subspan(1)) {
    if (byte == ':') {
      return true;
    }
    if (!alpha(byte) && (byte < '0' || byte > '9') && byte != '+' && byte != '-' && byte != '.') {
      return false;
    }
  }
  return false;
}

} // namespace lemma::vt
