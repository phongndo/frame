#include "render/status_line.hpp"

#include "lemma/assert.hpp"
#include "lemma/limits.hpp"
#include "render/scene.hpp"
#include "render/ui.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace lemma::render {
namespace {

constexpr std::size_t status_title_columns_max = 16;
constexpr std::size_t status_session_columns_max = 32;
constexpr std::size_t status_label_bytes_max = status_session_columns_max + 4U;
// An inactive label is "NN:" plus its title, then one space and the attention marker.
static_assert(3U + status_title_columns_max + 1U + status_attention_bytes_max <=
              status_label_bytes_max);
constexpr std::string_view status_group_separator = " | ";
constexpr std::string_view status_create_button = "  +";

[[nodiscard]] constexpr auto status_leading_columns(const std::size_t session_columns) noexcept
    -> std::size_t {
  return session_columns == 0 ? 0U : session_columns + status_group_separator.size();
}

struct StatusLabel final {
  std::array<char, status_label_bytes_max> text{};
  std::size_t size{0};
  // Trailing bytes drawn with identity emphasis.
  std::size_t attention_size{0};
  bool active{false};
};

[[nodiscard]] auto sanitized_title(const std::string_view title,
                                   const std::span<char> output) noexcept -> std::size_t {
  const auto used = std::min(title.size(), output.size());
  std::size_t index = 0;
  for (const char character : std::span(title).first(used)) {
    const auto value = static_cast<unsigned char>(character);
    output.subspan(index, 1).front() = value >= 0x20U && value < 0x7FU ? character : '?';
    ++index;
  }
  if (used == 0) {
    constexpr std::string_view fallback = "shell";
    const auto fallback_size = std::min(fallback.size(), output.size());
    std::ranges::copy(std::span(fallback).first(fallback_size), output.begin());
    if (fallback_size < fallback.size()) {
      output.subspan(fallback_size - 1U, 1).front() = '~';
    }
    return fallback_size;
  }
  if (title.size() > used) {
    output.subspan(used - 1U, 1).front() = '~';
  }
  return used;
}

[[nodiscard]] auto
status_label(const StatusTab& tab,
             const std::size_t title_columns_max = status_title_columns_max) noexcept
    -> StatusLabel {
  StatusLabel label;
  label.active = tab.active;
  const auto append_character = [&](const char character) {
    std::span(label.text).subspan(label.size, 1).front() = character;
    ++label.size;
  };
  if (tab.active) {
    append_character('[');
    append_character(' ');
    const auto result = std::to_chars(std::span(label.text).subspan(label.size).data(),
                                      label.text.end(), tab.number);
    if (result.ec != std::errc{}) {
      return {};
    }
    label.size = static_cast<std::size_t>(std::distance(label.text.begin(), result.ptr));
    append_character(':');
    if (title_columns_max > 0) {
      label.size += sanitized_title(
          tab.title, std::span(label.text).subspan(label.size).first(title_columns_max));
    }
    append_character(' ');
    append_character(']');
    return label;
  }
  const auto result =
      std::to_chars(std::span(label.text).subspan(label.size).data(), label.text.end(), tab.number);
  if (result.ec != std::errc{}) {
    return {};
  }
  label.size = static_cast<std::size_t>(std::distance(label.text.begin(), result.ptr));
  if (title_columns_max > 0) {
    append_character(':');
    label.size += sanitized_title(
        tab.title, std::span(label.text).subspan(label.size).first(title_columns_max));
  }
  if (!tab.attention.empty()) {
    append_character(' ');
    for (const char character : tab.attention) {
      append_character(character);
    }
    label.attention_size = tab.attention.size();
  }
  return label;
}

[[nodiscard]] auto session_label(const std::string_view session_name,
                                 const std::size_t columns_max) noexcept -> StatusLabel {
  StatusLabel label;
  const auto bounded_columns = std::min(columns_max, status_session_columns_max + std::size_t{2});
  if (session_name.empty() || bounded_columns < 3U) {
    return label;
  }
  label.text.front() = ' ';
  label.size = 1U;
  label.size += sanitized_title(
      session_name,
      std::span(label.text).subspan(label.size).first(bounded_columns - std::size_t{2}));
  std::span(label.text).subspan(label.size, 1).front() = ' ';
  ++label.size;
  return label;
}

// Overflow indicators reserve one more column for a hidden-attention glyph whenever any Tab
// carries attention, so which Tabs are hidden never depends on which ones carry it.
[[nodiscard]] auto overflow_columns(const std::span<const StatusLabel> labels) noexcept
    -> std::size_t {
  return std::ranges::any_of(labels,
                             [](const StatusLabel& label) { return label.attention_size > 0; })
             ? 3U
             : 2U;
}

[[nodiscard]] auto attention_text(const StatusLabel& label) noexcept -> std::string_view {
  return std::string_view(label.text.data(), label.size).substr(label.size - label.attention_size);
}

// The most urgent attention among hidden labels: `x` (something failed), then `!` (unseen alert),
// then `%` (any other attention, which is progress in flight).
[[nodiscard]] auto hidden_attention(const std::span<const StatusLabel> hidden) noexcept -> char {
  const auto any = [&](const auto predicate) { return std::ranges::any_of(hidden, predicate); };
  if (any([](const StatusLabel& label) { return attention_text(label).contains('x'); })) {
    return 'x';
  }
  if (any([](const StatusLabel& label) { return attention_text(label).contains('!'); })) {
    return '!';
  }
  return any([](const StatusLabel& label) { return label.attention_size > 0; }) ? '%' : '\0';
}

[[nodiscard]] auto status_width(const std::span<const StatusLabel> labels, const std::size_t begin,
                                const std::size_t end) noexcept -> std::size_t {
  const auto overflow = overflow_columns(labels);
  std::size_t width = begin > 0 ? overflow : 0U;
  for (std::size_t index = begin; index <= end; ++index) {
    width += std::span(labels).subspan(index, 1).front().size;
    if (index < end) {
      width += 2U;
    }
  }
  if (end + 1U < labels.size()) {
    width += overflow;
  }
  return width;
}

struct PromptValueProjection final {
  std::size_t begin{0};
  std::size_t size{0};
  std::size_t cursor{0};
};

[[nodiscard]] constexpr auto prompt_value_projection(const StatusLine status,
                                                     const std::size_t capacity) noexcept
    -> PromptValueProjection {
  if (capacity == 0) {
    return {};
  }
  const auto cursor = std::min(status.prompt_cursor, status.prompt_value.size());
  const auto begin = cursor > capacity ? cursor - capacity : 0U;
  return {
      .begin = begin,
      .size = std::min(status.prompt_value.size() - begin, capacity),
      .cursor = std::min(cursor - begin, capacity),
  };
}

[[nodiscard]] constexpr auto bounded_status_view(std::string_view value, const std::size_t begin,
                                                 const std::size_t size) noexcept
    -> std::string_view {
  value.remove_prefix(std::min(begin, value.size()));
  value.remove_suffix(value.size() - std::min(size, value.size()));
  return value;
}

struct PromptField final {
  StatusLabel label;
  std::size_t edit_offset{0};
  std::size_t edit_size{0};
  std::size_t cursor_offset{0};
};

void append_label_character(StatusLabel& label, const char character) noexcept {
  LEMMA_ASSERT(label.size < label.text.size());
  std::span(label.text).subspan(label.size, 1).front() = character;
  ++label.size;
}

void append_label_text(StatusLabel& label, const std::string_view text) noexcept {
  LEMMA_ASSERT(text.size() <= label.text.size() - label.size);
  std::memcpy(std::span(label.text).subspan(label.size).data(), text.data(), text.size());
  label.size += text.size();
}

[[nodiscard]] auto editable_session_label(const StatusLine status,
                                          const std::size_t value_capacity) noexcept
    -> PromptField {
  PromptField field;
  append_label_character(field.label, ' ');
  field.edit_offset = field.label.size;
  const auto value =
      prompt_value_projection(status, std::min(value_capacity, status_session_columns_max));
  append_label_text(field.label, bounded_status_view(status.prompt_value, value.begin, value.size));
  field.edit_size = value.size;
  field.cursor_offset = field.edit_offset + value.cursor;
  append_label_character(field.label, ' ');
  return field;
}

[[nodiscard]] auto editable_tab_label(const StatusLine status, const std::uint16_t tab_number,
                                      const std::size_t value_capacity) noexcept -> PromptField {
  PromptField field;
  field.label.active = true;
  append_label_character(field.label, '[');
  append_label_character(field.label, ' ');
  const auto result = std::to_chars(std::span(field.label.text).subspan(field.label.size).data(),
                                    field.label.text.end(), tab_number);
  if (result.ec != std::errc{}) {
    return {};
  }
  field.label.size = static_cast<std::size_t>(std::distance(field.label.text.begin(), result.ptr));
  append_label_character(field.label, ':');
  field.edit_offset = field.label.size;
  const auto value =
      prompt_value_projection(status, std::min(value_capacity, status_title_columns_max));
  append_label_text(field.label, bounded_status_view(status.prompt_value, value.begin, value.size));
  field.edit_size = value.size;
  field.cursor_offset = field.edit_offset + value.cursor;
  append_label_character(field.label, ' ');
  append_label_character(field.label, ']');
  return field;
}

[[nodiscard]] auto editable_bare_label(const StatusLine status, const std::size_t capacity) noexcept
    -> PromptField {
  PromptField field;
  if (capacity == 0) {
    return field;
  }
  const auto value = prompt_value_projection(status, capacity - 1U);
  append_label_text(field.label, bounded_status_view(status.prompt_value, value.begin, value.size));
  field.edit_size = value.size;
  field.cursor_offset = value.cursor;
  append_label_character(field.label, ' ');
  return field;
}

[[nodiscard]] constexpr auto prompt_message(const StatusLine status) noexcept -> std::string_view {
  switch (status.prompt_feedback) {
  case StatusPromptFeedback::none:
    return {};
  case StatusPromptFeedback::conflict:
    return "Session already exists";
  case StatusPromptFeedback::invalid:
    return status.prompt_target == StatusPromptTarget::session ? "Invalid session name"
                                                               : "Invalid tab title";
  }
  return {};
}

struct InlineStatusPromptProjection final {
  StatusLabel session;
  std::array<StatusLabel, status_tabs_max> labels{};
  PromptField field;
  std::string_view message;
  std::size_t label_count{0};
  std::size_t active{0};
  std::size_t begin{0};
  std::size_t end{0};
  std::uint16_t tab_column{1};
  std::uint16_t cursor_column{1};
  bool show_tabs{false};
  bool show_message{false};
  bool bare_field{false};
};

// Prompt projection prioritizes the edited identity, then the active context, then neighboring
// tabs and feedback. Narrow terminals degrade to the horizontally-scrolled field alone.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto inline_status_prompt_projection(const StatusLine status,
                                                   const Viewport viewport) noexcept
    -> InlineStatusPromptProjection {
  InlineStatusPromptProjection projection;
  projection.label_count = status.tabs.size();
  auto labels = std::span(projection.labels).first(projection.label_count);
  for (std::size_t index = 0; index < status.tabs.size(); ++index) {
    const auto& tab = status.tabs.subspan(index, 1).front();
    labels.subspan(index, 1).front() = status_label(tab);
    if (tab.active) {
      projection.active = index;
    }
  }

  const auto columns = static_cast<std::size_t>(viewport.columns);
  if (status.prompt_target == StatusPromptTarget::session) {
    projection.field = editable_session_label(status, status_session_columns_max);
    projection.session = projection.field.label;
  } else {
    projection.session = session_label(status.session_name, columns);
    projection.field = editable_tab_label(
        status, status.tabs.subspan(projection.active, 1).front().number, status_title_columns_max);
    labels.subspan(projection.active, 1).front() = projection.field.label;
  }

  const auto tab_columns_for = [](const std::size_t available,
                                  const std::size_t session_columns) noexcept {
    const auto leading_columns = status_leading_columns(session_columns);
    return leading_columns < available ? available - leading_columns : std::size_t{0};
  };
  projection.message = prompt_message(status);
  auto active_width = labels.subspan(projection.active, 1).front().size;
  const auto minimum_left = status_leading_columns(projection.session.size) + active_width;
  projection.show_message =
      !projection.message.empty() && minimum_left + 2U + projection.message.size() <= columns;
  auto left_columns = projection.show_message ? columns - projection.message.size() - 2U : columns;

  if (status.prompt_target == StatusPromptTarget::active_tab && minimum_left > left_columns) {
    const auto reserved_columns = active_width + status_group_separator.size();
    const auto session_columns =
        left_columns > reserved_columns ? left_columns - reserved_columns : 0U;
    projection.session = session_label(status.session_name, session_columns);
  }

  auto tab_columns = tab_columns_for(left_columns, projection.session.size);
  if (status.prompt_target == StatusPromptTarget::active_tab && active_width > tab_columns) {
    auto capacity = status_title_columns_max;
    while (capacity > 1U && projection.field.label.size > tab_columns) {
      --capacity;
      projection.field = editable_tab_label(
          status, status.tabs.subspan(projection.active, 1).front().number, capacity);
    }
    labels.subspan(projection.active, 1).front() = projection.field.label;
    active_width = projection.field.label.size;
  }

  if (status.prompt_target == StatusPromptTarget::session &&
      projection.session.size > left_columns) {
    projection.field = left_columns >= 3U ? editable_session_label(status, left_columns - 2U)
                                          : editable_bare_label(status, left_columns);
    projection.session = projection.field.label;
    projection.show_tabs = false;
    projection.show_message = false;
    left_columns = columns;
  } else {
    tab_columns = tab_columns_for(left_columns, projection.session.size);
    projection.show_tabs = active_width <= tab_columns &&
                           (status.prompt_target != StatusPromptTarget::active_tab ||
                            status.prompt_value.empty() || projection.field.edit_size > 0);
  }

  if (status.prompt_target == StatusPromptTarget::active_tab && !projection.show_tabs) {
    projection.session = {};
    projection.field = editable_bare_label(status, left_columns);
    labels.subspan(projection.active, 1).front() = projection.field.label;
    projection.show_tabs = projection.field.label.size <= left_columns;
    projection.show_message = false;
    projection.bare_field = true;
    tab_columns = left_columns;
  }

  projection.tab_column =
      static_cast<std::uint16_t>(status_leading_columns(projection.session.size) + 1U);
  projection.begin = projection.active;
  projection.end = projection.active;
  if (projection.show_tabs && !projection.bare_field) {
    bool try_left = true;
    bool left_blocked = projection.begin == 0;
    bool right_blocked = projection.end + 1U == labels.size();
    while (!left_blocked || !right_blocked) {
      const bool use_left = (try_left && !left_blocked) || right_blocked;
      const auto candidate_begin = use_left ? projection.begin - 1U : projection.begin;
      const auto candidate_end = use_left ? projection.end : projection.end + 1U;
      if (status_width(labels, candidate_begin, candidate_end) <= tab_columns) {
        projection.begin = candidate_begin;
        projection.end = candidate_end;
      } else if (use_left) {
        left_blocked = true;
      } else {
        right_blocked = true;
      }
      left_blocked = left_blocked || projection.begin == 0;
      right_blocked = right_blocked || projection.end + 1U == labels.size();
      try_left = !try_left;
    }
  }

  std::size_t cursor_column = 1U;
  if (status.prompt_target == StatusPromptTarget::session) {
    cursor_column += projection.field.cursor_offset;
  } else {
    cursor_column = projection.tab_column;
    cursor_column +=
        projection.begin > 0 && !projection.bare_field
            ? overflow_columns(std::span(projection.labels).first(projection.label_count))
            : 0U;
    for (std::size_t index = projection.begin; index < projection.active; ++index) {
      cursor_column += labels.subspan(index, 1).front().size + 2U;
    }
    cursor_column += projection.field.cursor_offset;
  }
  projection.cursor_column =
      static_cast<std::uint16_t>(std::clamp(cursor_column, std::size_t{1}, columns));
  return projection;
}

struct StatusLineProjection final {
  std::array<StatusLabel, status_tabs_max> labels{};
  StatusLabel session;
  std::size_t label_count{0};
  std::size_t begin{0};
  std::size_t end{0};
  std::uint16_t tab_column{1};
  std::uint16_t create_column{0};
  bool show_range{false};
  bool show_create{false};
};

// Rendering and mouse hit testing consume this same bounded projection so visible labels have one
// geometry owner. Its branches preserve the active tab under narrow-terminal degradation.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto status_line_projection(const StatusLine status, const Viewport viewport) noexcept
    -> StatusLineProjection {
  StatusLineProjection projection;
  projection.label_count = status.tabs.size();
  auto labels = std::span(projection.labels).first(projection.label_count);
  std::size_t active = 0;
  for (std::size_t index = 0; index < status.tabs.size(); ++index) {
    const auto& tab = status.tabs.subspan(index, 1).front();
    labels.subspan(index, 1).front() = status_label(tab);
    if (tab.active) {
      active = index;
    }
  }

  const auto content_columns = static_cast<std::size_t>(viewport.columns);
  const auto active_width = labels.subspan(active, 1).front().size;
  const auto separator_reservation =
      status.session_name.empty() ? 0U : status_group_separator.size();
  const bool create_available = status.tabs.size() < status_tabs_max;
  const auto create_reservation =
      create_available && active_width + status_create_button.size() <= content_columns
          ? status_create_button.size()
          : 0U;
  const auto session_reservation = active_width + separator_reservation + create_reservation;
  const auto session_columns_max =
      session_reservation < content_columns ? content_columns - session_reservation : 0U;
  projection.session = session_label(status.session_name, session_columns_max);
  const auto leading_columns = status_leading_columns(projection.session.size);
  const auto available_tab_columns = content_columns - leading_columns;
  projection.show_create =
      create_available && active_width + status_create_button.size() <= available_tab_columns;
  const auto tab_columns =
      available_tab_columns - (projection.show_create ? status_create_button.size() : 0U);

  auto title_columns = status_title_columns_max;
  while (labels.subspan(active, 1).front().size > tab_columns && title_columns > 0U) {
    --title_columns;
    labels.subspan(active, 1).front() =
        status_label(status.tabs.subspan(active, 1).front(), title_columns);
  }
  if (labels.subspan(active, 1).front().size > tab_columns) {
    auto& label = labels.subspan(active, 1).front();
    constexpr std::string_view empty_active = "[  ]";
    label.size = std::min(tab_columns, empty_active.size());
    std::ranges::copy(std::span(empty_active.data(), empty_active.size()).first(label.size),
                      label.text.begin());
  }

  projection.begin = active;
  projection.end = active;
  if (status_width(labels, projection.begin, projection.end) <= tab_columns) {
    bool try_left = true;
    bool left_blocked = projection.begin == 0;
    bool right_blocked = projection.end + 1U == labels.size();
    while (!left_blocked || !right_blocked) {
      const bool use_left = (try_left && !left_blocked) || right_blocked;
      const auto candidate_begin = use_left ? projection.begin - 1U : projection.begin;
      const auto candidate_end = use_left ? projection.end : projection.end + 1U;
      if (status_width(labels, candidate_begin, candidate_end) <= tab_columns) {
        projection.begin = candidate_begin;
        projection.end = candidate_end;
      } else if (use_left) {
        left_blocked = true;
      } else {
        right_blocked = true;
      }
      left_blocked = left_blocked || projection.begin == 0;
      right_blocked = right_blocked || projection.end + 1U == labels.size();
      try_left = !try_left;
    }
  }

  projection.show_range = status_width(labels, projection.begin, projection.end) <= tab_columns;
  if (!projection.show_range) {
    projection.begin = active;
    projection.end = active;
  }
  projection.tab_column = static_cast<std::uint16_t>(leading_columns + 1U);
  const auto rendered_tab_columns = projection.show_range
                                        ? status_width(labels, projection.begin, projection.end)
                                        : labels.subspan(projection.begin, 1).front().size;
  if (projection.show_create) {
    projection.create_column = static_cast<std::uint16_t>(
        projection.tab_column - 1U + rendered_tab_columns + status_create_button.size() - 1U);
  }
  return projection;
}

constexpr ui::Style status_default_cell_style{};
constexpr ui::Style status_identity_cell_style{.attributes = ui::attribute_bold};
constexpr ui::Style status_prompt_cell_style{.attributes =
                                                 ui::attribute_bold | ui::attribute_underline};

[[nodiscard]] constexpr auto utf8_scalar_size(const std::uint8_t first) noexcept -> std::size_t {
  if (first < 0x80U) {
    return 1;
  }
  if (first < 0xE0U) {
    return 2;
  }
  if (first < 0xF0U) {
    return 3;
  }
  return 4;
}

[[nodiscard]] auto write_status_text(const std::span<ui::Cell> cells, std::size_t& column,
                                     const std::string_view text, const ui::Style style) noexcept
    -> bool {
  const std::span text_bytes(text.data(), text.size());
  std::size_t offset = 0;
  while (offset < text_bytes.size()) {
    if (column >= cells.size()) {
      return false;
    }
    const auto first = static_cast<std::uint8_t>(text_bytes.subspan(offset, 1).front());
    const auto size = utf8_scalar_size(first);
    if (size > text_bytes.size() - offset || size > ui::cell_text_bytes_max) {
      return false;
    }
    auto& cell = cells.subspan(column, 1).front();
    std::memcpy(cell.text.data(), text_bytes.subspan(offset, size).data(), size);
    cell.style = style;
    cell.text_size = static_cast<std::uint8_t>(size);
    cell.painted = true;
    ++column;
    offset += size;
  }
  return true;
}

[[nodiscard]] auto write_status_label(const std::span<ui::Cell> cells, std::size_t& column,
                                      const StatusLabel& label, const ui::Style style) noexcept
    -> bool {
  const std::string_view text(label.text.data(), label.size);
  const auto plain = label.size - label.attention_size;
  return write_status_text(cells, column, text.substr(0, plain), style) &&
         write_status_text(cells, column, text.substr(plain), status_identity_cell_style);
}

// Writes `… ` before or ` …` after the visible labels, padded to the reserved overflow width,
// with the hidden labels' most urgent attention glyph emphasized after the ellipsis.
[[nodiscard]] auto write_overflow(const std::span<ui::Cell> cells, std::size_t& column,
                                  const std::span<const StatusLabel> labels,
                                  const std::span<const StatusLabel> hidden,
                                  const bool leading) noexcept -> bool {
  const auto glyph = hidden_attention(hidden);
  const auto mark = glyph == '\0' ? std::string_view{} : std::string_view(&glyph, 1);
  const auto padding = overflow_columns(labels) - 1U - mark.size();
  constexpr std::string_view spaces = "   ";
  return (leading || write_status_text(cells, column, " ", status_default_cell_style)) &&
         write_status_text(cells, column, "…", status_default_cell_style) &&
         write_status_text(cells, column, mark, status_identity_cell_style) &&
         write_status_text(cells, column, spaces.substr(0, leading ? padding : padding - 1U),
                           status_default_cell_style);
}

[[nodiscard]] auto write_prompt_field(const std::span<ui::Cell> cells, std::size_t& column,
                                      const PromptField& field, const ui::Style base,
                                      const ui::Style edit) noexcept -> bool {
  const auto text = std::string_view(field.label.text.data(), field.label.size);
  return write_status_text(cells, column, bounded_status_view(text, 0, field.edit_offset), base) &&
         write_status_text(cells, column,
                           bounded_status_view(text, field.edit_offset, field.edit_size), edit) &&
         write_status_text(
             cells, column,
             bounded_status_view(text, field.edit_offset + field.edit_size, text.size()), base);
}

struct ModalPromptProjection final {
  PromptValueProjection value;
  std::uint16_t cursor_column{1};
};

[[nodiscard]] constexpr auto modal_prompt_projection(const StatusLine status,
                                                     const Viewport viewport) noexcept
    -> ModalPromptProjection {
  ModalPromptProjection projection;
  const auto columns = static_cast<std::size_t>(viewport.columns);
  const auto capacity = columns > 1U ? columns - 1U : 0U;
  projection.value = prompt_value_projection(status, capacity);
  projection.cursor_column = static_cast<std::uint16_t>(
      std::clamp(std::size_t{2} + projection.value.cursor, std::size_t{1}, columns));
  return projection;
}

[[nodiscard]] constexpr auto is_modal_prompt(const StatusPromptTarget target) noexcept -> bool {
  return target == StatusPromptTarget::command_line ||
         target == StatusPromptTarget::copy_search_forward ||
         target == StatusPromptTarget::copy_search_backward;
}

[[nodiscard]] constexpr auto modal_prompt_prefix(const StatusPromptTarget target) noexcept
    -> std::string_view {
  switch (target) {
  case StatusPromptTarget::command_line:
    return ":";
  case StatusPromptTarget::copy_search_forward:
    return "/";
  case StatusPromptTarget::copy_search_backward:
    return "?";
  case StatusPromptTarget::none:
  case StatusPromptTarget::session:
  case StatusPromptTarget::active_tab:
  case StatusPromptTarget::message:
    return {};
  }
  return {};
}

[[nodiscard]] auto build_modal_prompt_cells(const StatusLine status, const Viewport viewport,
                                            const std::span<ui::Cell> cells) noexcept -> bool {
  const auto projection = modal_prompt_projection(status, viewport);
  std::size_t column = 0;
  return write_status_text(cells, column, modal_prompt_prefix(status.prompt_target),
                           status_identity_cell_style) &&
         write_status_text(cells, column,
                           bounded_status_view(status.prompt_value, projection.value.begin,
                                               projection.value.size),
                           status_identity_cell_style);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto build_status_prompt_cells(const StatusLine status, const Viewport viewport,
                                             const std::span<ui::Cell> cells) noexcept -> bool {
  if (is_modal_prompt(status.prompt_target)) {
    return build_modal_prompt_cells(status, viewport, cells);
  }
  const auto projection = inline_status_prompt_projection(status, viewport);
  std::size_t column = 0;
  if (projection.session.size > 0) {
    if (status.prompt_target == StatusPromptTarget::session) {
      if (!write_prompt_field(cells, column, projection.field, status_identity_cell_style,
                              status_prompt_cell_style)) {
        return false;
      }
    } else if (!write_status_label(cells, column, projection.session, status_identity_cell_style)) {
      return false;
    }
  }
  if (projection.session.size > 0 && projection.show_tabs &&
      !write_status_text(cells, column, status_group_separator, status_default_cell_style)) {
    return false;
  }
  if (projection.show_tabs) {
    column = projection.tab_column - 1U;
    const auto labels = std::span(projection.labels).first(projection.label_count);
    if (!projection.bare_field && projection.begin > 0 &&
        !write_overflow(cells, column, labels, labels.first(projection.begin), true)) {
      return false;
    }
    for (std::size_t index = projection.begin; index <= projection.end; ++index) {
      const auto& label = labels.subspan(index, 1).front();
      const bool editable =
          status.prompt_target == StatusPromptTarget::active_tab && index == projection.active;
      if ((editable && !write_prompt_field(cells, column, projection.field,
                                           status_identity_cell_style, status_prompt_cell_style)) ||
          (!editable && !write_status_label(cells, column, label,
                                            label.active ? status_identity_cell_style
                                                         : status_default_cell_style)) ||
          (index < projection.end &&
           !write_status_text(cells, column, "  ", status_default_cell_style))) {
        return false;
      }
    }
    if (!projection.bare_field && projection.end + 1U < labels.size() &&
        !write_overflow(cells, column, labels, labels.subspan(projection.end + 1U), false)) {
      return false;
    }
  }
  if (projection.show_message) {
    column = viewport.columns - projection.message.size();
    if (!write_status_text(cells, column, projection.message, status_identity_cell_style)) {
      return false;
    }
  }
  return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto build_status_cells(const StatusLine status, const Viewport viewport,
                                      const std::span<ui::Cell> cells) noexcept -> bool {
  if (status.prompt_target == StatusPromptTarget::message) {
    std::size_t column = 0;
    return write_status_text(
        cells, column,
        bounded_status_view(
            status.input_context, 0,
            std::min(status.input_context.size(), static_cast<std::size_t>(viewport.columns))),
        status_identity_cell_style);
  }
  if (status.prompting()) {
    return build_status_prompt_cells(status, viewport, cells);
  }
  if (!status.input_context.empty()) {
    std::size_t column = 0;
    return write_status_text(
        cells, column,
        bounded_status_view(
            status.input_context, 0,
            std::min(status.input_context.size(), static_cast<std::size_t>(viewport.columns))),
        status_identity_cell_style);
  }
  const auto projection = status_line_projection(status, viewport);
  const auto labels = std::span(projection.labels).first(projection.label_count);
  std::size_t column = 0;
  if (projection.session.size > 0 &&
      (!write_status_label(cells, column, projection.session, status_identity_cell_style) ||
       !write_status_text(cells, column, status_group_separator, status_default_cell_style))) {
    return false;
  }
  column = projection.tab_column - 1U;
  if (projection.show_range) {
    if (projection.begin > 0 &&
        !write_overflow(cells, column, labels, labels.first(projection.begin), true)) {
      return false;
    }
    for (std::size_t index = projection.begin; index <= projection.end; ++index) {
      const auto& label = labels.subspan(index, 1).front();
      if (!write_status_label(cells, column, label,
                              label.active ? status_identity_cell_style
                                           : status_default_cell_style) ||
          (index < projection.end &&
           !write_status_text(cells, column, "  ", status_default_cell_style))) {
        return false;
      }
    }
    if (projection.end + 1U < labels.size() &&
        !write_overflow(cells, column, labels, labels.subspan(projection.end + 1U), false)) {
      return false;
    }
  } else if (!write_status_label(cells, column, labels.subspan(projection.begin, 1).front(),
                                 status_identity_cell_style)) {
    return false;
  }
  return !projection.show_create ||
         write_status_text(cells, column, status_create_button, status_default_cell_style);
}

// Default status and prompt content share one bounded cell projection and painter.
[[nodiscard]] constexpr auto valid_prompt_target(const StatusPromptTarget target) noexcept -> bool {
  switch (target) {
  case StatusPromptTarget::none:
  case StatusPromptTarget::session:
  case StatusPromptTarget::active_tab:
  case StatusPromptTarget::command_line:
  case StatusPromptTarget::copy_search_forward:
  case StatusPromptTarget::copy_search_backward:
  case StatusPromptTarget::message:
    return true;
  }
  return false;
}

[[nodiscard]] constexpr auto valid_prompt_feedback(const StatusPromptFeedback feedback) noexcept
    -> bool {
  switch (feedback) {
  case StatusPromptFeedback::none:
  case StatusPromptFeedback::invalid:
  case StatusPromptFeedback::conflict:
    return true;
  }
  return false;
}

[[nodiscard]] constexpr auto prompt_capacity(const StatusPromptTarget target) noexcept
    -> std::size_t {
  switch (target) {
  case StatusPromptTarget::session:
    return limits::session_name_bytes_max;
  case StatusPromptTarget::command_line:
    return limits::command_line_bytes_max;
  case StatusPromptTarget::copy_search_forward:
  case StatusPromptTarget::copy_search_backward:
    return limits::search_query_bytes_max;
  case StatusPromptTarget::active_tab:
    return limits::tab_title_bytes_max;
  case StatusPromptTarget::none:
  case StatusPromptTarget::message:
    return 0;
  }
  return 0;
}

[[nodiscard]] auto valid_prompt_value(const StatusLine status) noexcept -> bool {
  if (!status.prompting()) {
    return status.prompt_value.empty() && status.prompt_cursor == 0;
  }
  const auto valid_character = [target = status.prompt_target](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    if (target == StatusPromptTarget::session) {
      return (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) ||
             (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) ||
             (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9')) ||
             byte == static_cast<unsigned char>('_') || byte == static_cast<unsigned char>('-');
    }
    return byte >= 0x20U && byte <= 0x7eU;
  };
  return status.prompt_value.size() <= prompt_capacity(status.prompt_target) &&
         std::ranges::all_of(status.prompt_value, valid_character);
}

[[nodiscard]] auto valid_viewport(const Viewport viewport) noexcept -> bool {
  return viewport.columns > 0 && viewport.rows > 0 &&
         viewport.columns <= limits::terminal_columns_hard_max &&
         viewport.rows <= limits::terminal_rows_hard_max;
}

} // namespace

[[nodiscard]] auto valid_status(const StatusLine status) noexcept -> bool {
  const auto printable = [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x20U && byte <= 0x7eU;
  };
  const auto context_capacity = status.prompt_target == StatusPromptTarget::message
                                    ? limits::status_message_bytes_max
                                    : status_context_bytes_max;
  const bool valid_context =
      status.input_context.size() <= context_capacity &&
      (!is_modal_prompt(status.prompt_target) || status.input_context.empty()) &&
      (status.prompt_target != StatusPromptTarget::message || !status.input_context.empty()) &&
      std::ranges::all_of(status.input_context, printable);
  return valid_context && valid_prompt_target(status.prompt_target) &&
         valid_prompt_feedback(status.prompt_feedback) && valid_prompt_value(status) &&
         status.tabs.size() <= status_tabs_max &&
         (status.tabs.empty() || std::ranges::count(status.tabs, true, &StatusTab::active) == 1) &&
         std::ranges::none_of(status.tabs, [](const StatusTab& tab) { return tab.number == 0; }) &&
         std::ranges::all_of(status.tabs,
                             [&](const StatusTab& tab) {
                               return tab.attention.size() <= status_attention_bytes_max &&
                                      (!tab.active || tab.attention.empty()) &&
                                      std::ranges::all_of(tab.attention, printable);
                             }) &&
         status.prompt_cursor <= status.prompt_value.size() &&
         (!status.prompting() || !status.tabs.empty()) &&
         (status.prompting() || status.prompt_feedback == StatusPromptFeedback::none);
}

auto status_cursor_column(const StatusLine status, const Viewport viewport) noexcept
    -> std::uint16_t {
  return is_modal_prompt(status.prompt_target)
             ? modal_prompt_projection(status, viewport).cursor_column
             : inline_status_prompt_projection(status, viewport).cursor_column;
}

auto project_status_cells(const StatusLine status, const Viewport viewport,
                          const std::span<ui::Cell> cells, std::uint16_t& cursor) noexcept -> bool {
  if (!valid_viewport(viewport) || !valid_status(status) || cells.size() != viewport.columns) {
    return false;
  }
  std::ranges::fill(cells, ui::Cell{});
  cursor = status.prompting()
               ? static_cast<std::uint16_t>(
                     (is_modal_prompt(status.prompt_target)
                          ? modal_prompt_projection(status, viewport).cursor_column
                          : inline_status_prompt_projection(status, viewport).cursor_column) -
                     1U)
               : std::uint16_t{0};
  return build_status_cells(status, viewport, cells);
}

[[nodiscard]] auto status_target_at_column(const StatusLine status, const Viewport viewport,
                                           const std::uint16_t column) noexcept
    -> std::optional<StatusTarget> {
  if (!valid_viewport(viewport) || !valid_status(status) ||
      (status.tabs.empty() || viewport.rows < 2) || status.prompting() ||
      status.prompt_target == StatusPromptTarget::message || !status.input_context.empty() ||
      column >= viewport.columns) {
    return std::nullopt;
  }
  const auto projection = status_line_projection(status, viewport);
  if (projection.show_create && column == projection.create_column) {
    return StatusTarget{.kind = StatusTargetKind::create_tab, .tab_position = 0};
  }
  const auto labels = std::span(projection.labels).first(projection.label_count);
  auto current_column = static_cast<std::size_t>(projection.tab_column - 1U);
  if (projection.show_range && projection.begin > 0) {
    current_column += overflow_columns(labels);
  }
  for (std::size_t index = projection.begin; index <= projection.end; ++index) {
    const auto label_columns = labels.subspan(index, 1).front().size;
    if (column >= current_column && column < current_column + label_columns) {
      return StatusTarget{.kind = StatusTargetKind::tab, .tab_position = index};
    }
    current_column += label_columns;
    if (projection.show_range && index < projection.end) {
      current_column += 2U;
    }
  }
  return std::nullopt;
}

} // namespace lemma::render
