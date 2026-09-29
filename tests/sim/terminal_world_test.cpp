#include "random.hpp"
#include "terminal_trace.hpp"

#include "frame/limits.hpp"
#include "frame/terminal/terminal.hpp"
#include "render/pane_composition.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace frame::test::sim {
namespace {

constexpr std::size_t snapshot_bytes_max = std::size_t{256} * 1'024U;
constexpr std::size_t frame_bytes_max = std::size_t{256} * 1'024U;
constexpr std::size_t input_bytes_max = 128;
constexpr std::size_t terminal_default_operations = 512;

constexpr std::array terminal_payloads{
    std::string_view{"plain output"},
    std::string_view{"\r\nline two\r\nline three"},
    std::string_view{"\x1B[2J\x1B[Hhome\x1B[3;4Hcursor"},
    std::string_view{"\x1B[1;31mred\x1B[0;48;2;7;8;9m rgb \x1B[0m"},
    std::string_view{"Ae\xCC\x81"
                     "Z"},
    std::string_view{"\x1B[?1049h\x1B[2Jalternate"},
    std::string_view{"\x1B[?1049lprimary"},
    std::string_view{"\x1B[?1h\x1B[?1004h\x1B[?2004h\x1B[?1000h\x1B[?1006h"},
    std::string_view{"\x1B[?1l\x1B[?1004l\x1B[?2004l\x1B[?1000l\x1B[?1003l"},
    std::string_view{"\x1B[>3u"},
    std::string_view{"\x1B[<u"},
    std::string_view{"\x1B[6n\x1B[c\x1B[18t\x1B[?996n\x1B[>q"},
    std::string_view{"\x1B[?2048h"},
    std::string_view{"\x1B[?2048l"},
    std::string_view{"\a\x1B]2;sim title\x1B\\\x1B]7;file:///tmp/sim\x07"
                     "\x1B]777;notify;sim;attention\x07\x1B]9;4;1;42\x1B\\"},
    std::string_view{"\x1B]4;1;rgb:01/02/03\x1B\\\x1B]10;rgb:04/05/06\x1B\\"
                     "\x1B]11;rgb:07/08/09\x1B\\\x1B]12;rgb:0a/0b/0c\x1B\\"},
    std::string_view{"\x1B_unsupported-terminal-payload\x1B\\"},
    std::string_view{"history-abcdefghijklmnopqrstuvwxyz-0123456789\r\n"},
    std::string_view{"\x1B[?2026hheld"},
    std::string_view{" released\x1B[?2026l"},
};

struct TerminalCoverage final {
  std::array<std::size_t, static_cast<std::size_t>(TerminalOperationKind::count)> operations{};
  std::array<std::size_t, terminal_payloads.size()> payloads{};

  void include(const TerminalOperation& operation) noexcept {
    const auto kind = static_cast<std::size_t>(operation.kind);
    if (kind < operations.size()) {
      ++std::span(operations).subspan(kind, 1).front();
    }
    if (operation.kind == TerminalOperationKind::write && operation.argument_0 < payloads.size()) {
      ++std::span(payloads).subspan(operation.argument_0, 1).front();
    }
  }

  void merge(const TerminalCoverage& other) noexcept {
    for (std::size_t index = 0; index < operations.size(); ++index) {
      std::span(operations).subspan(index, 1).front() +=
          std::span(other.operations).subspan(index, 1).front();
    }
    for (std::size_t index = 0; index < payloads.size(); ++index) {
      std::span(payloads).subspan(index, 1).front() +=
          std::span(other.payloads).subspan(index, 1).front();
    }
  }
};

[[nodiscard]] constexpr auto hash_byte(std::uint64_t hash, const std::uint8_t value) noexcept
    -> std::uint64_t {
  return (hash ^ value) * 1'099'511'628'211ULL;
}

[[nodiscard]] constexpr auto hash_value(std::uint64_t hash, const std::uint64_t value) noexcept
    -> std::uint64_t {
  for (std::size_t shift = 0; shift < 64; shift += 8) {
    hash = hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
  }
  return hash;
}

[[nodiscard]] auto hash_bytes(std::uint64_t hash, const std::span<const std::byte> bytes) noexcept
    -> std::uint64_t {
  for (const auto byte : bytes) {
    hash = hash_byte(hash, std::to_integer<std::uint8_t>(byte));
  }
  return hash;
}

[[nodiscard]] auto make_terminal(const vt::TerminalOptions& options) -> vt::Terminal {
  auto terminal = vt::Terminal::create(options);
  if (!terminal.has_value()) {
    std::abort();
  }
  return std::move(*terminal);
}

[[nodiscard]] constexpr auto effects_equal(const vt::EffectBatch& first,
                                           const vt::EffectBatch& second) noexcept -> bool {
  return first.bells == second.bells && first.title_changes == second.title_changes &&
         first.pwd_changes == second.pwd_changes &&
         first.desktop_notifications == second.desktop_notifications &&
         first.progress_reports == second.progress_reports &&
         first.clipboard_writes_denied == second.clipboard_writes_denied &&
         first.unknown_sequences_dropped == second.unknown_sequences_dropped &&
         first.unknown_sequence_truncated == second.unknown_sequence_truncated &&
         first.pty_response_overflowed == second.pty_response_overflowed;
}

void accumulate_effects(vt::EffectBatch& total, const vt::EffectBatch& effects) noexcept {
  total.bells += effects.bells;
  total.title_changes += effects.title_changes;
  total.pwd_changes += effects.pwd_changes;
  total.desktop_notifications += effects.desktop_notifications;
  total.progress_reports += effects.progress_reports;
  total.clipboard_writes_denied += effects.clipboard_writes_denied;
  total.unknown_sequences_dropped += effects.unknown_sequences_dropped;
  total.unknown_sequence_truncated =
      total.unknown_sequence_truncated || effects.unknown_sequence_truncated;
  total.pty_response_overflowed = total.pty_response_overflowed || effects.pty_response_overflowed;
}

[[nodiscard]] constexpr auto inspection_equal(const vt::TerminalInspection& first,
                                              const vt::TerminalInspection& second) noexcept
    -> bool {
  return first.viewport.total_rows == second.viewport.total_rows &&
         first.viewport.offset == second.viewport.offset &&
         first.viewport.visible_rows == second.viewport.visible_rows &&
         first.viewport.follows_output == second.viewport.follows_output &&
         first.scrollback_rows == second.scrollback_rows &&
         first.cursor_column == second.cursor_column && first.cursor_row == second.cursor_row &&
         first.active_screen == second.active_screen &&
         first.cursor_visible == second.cursor_visible &&
         first.cursor_at_prompt == second.cursor_at_prompt;
}

[[nodiscard]] constexpr auto valid_inspection(const vt::TerminalInspection& inspection,
                                              const vt::TerminalSize size) noexcept -> bool {
  return inspection.viewport.offset <= inspection.viewport.total_rows &&
         inspection.viewport.visible_rows <= inspection.viewport.total_rows &&
         inspection.cursor_column < size.columns && inspection.cursor_row < size.rows;
}

class TerminalWorld final {
public:
  TerminalWorld()
      : options_(initial_options()), canonical_(make_terminal(options_)),
        chunked_(make_terminal(options_)), projected_(make_terminal(options_)),
        projected_chunked_(make_terminal(options_)) {}

  [[nodiscard]] auto apply(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    const auto choice = random.index(static_cast<std::size_t>(TerminalOperationKind::count));
    operation.kind = static_cast<TerminalOperationKind>(choice);
    auto error = [this, &random, &operation]() -> std::optional<std::string> {
      switch (operation.kind) {
      case TerminalOperationKind::write:
        return write(random, operation);
      case TerminalOperationKind::resize:
        return resize(random, operation);
      case TerminalOperationKind::compose:
        return compose(random, operation);
      case TerminalOperationKind::encode_key:
        return encode_key(random, operation);
      case TerminalOperationKind::encode_paste:
        return encode_paste(random, operation);
      case TerminalOperationKind::encode_focus:
        return encode_focus(random, operation);
      case TerminalOperationKind::encode_mouse:
        return encode_mouse(random, operation);
      case TerminalOperationKind::drain_pty:
        return drain_pty(random, operation);
      case TerminalOperationKind::set_theme:
        return set_theme(random, operation);
      case TerminalOperationKind::scroll_viewport:
        return scroll_viewport(random, operation);
      case TerminalOperationKind::select:
        return select(random, operation);
      case TerminalOperationKind::invalidate_render:
        return invalidate_render(operation);
      case TerminalOperationKind::count:
        return std::string{"generator selected the terminal operation sentinel"};
      }
      return std::string{"generator selected an unknown terminal operation"};
    }();
    if (error.has_value()) {
      return error;
    }
    coverage_.include(operation);
    return consume_effects();
  }

  [[nodiscard]] auto validate() -> std::optional<std::string> {
    if (canonical_.size() != chunked_.size() || canonical_.theme() != chunked_.theme()) {
      return std::string{"canonical and chunked terminal configuration diverged"};
    }
    const auto canonical_inspection = canonical_.inspection();
    const auto chunked_inspection = chunked_.inspection();
    if (!canonical_inspection.has_value() || !chunked_inspection.has_value() ||
        !inspection_equal(*canonical_inspection, *chunked_inspection) ||
        !valid_inspection(*canonical_inspection, canonical_.size())) {
      return std::string{"canonical and chunked terminal inspection diverged or became invalid"};
    }
    if (const auto error = validate_modes(); error.has_value()) {
      return error;
    }
    if (canonical_.pending_pty_response_bytes() != chunked_.pending_pty_response_bytes() ||
        canonical_.pending_pty_response_bytes() > limits::terminal_pty_response_bytes_max) {
      return std::string{"PTY response queue sizes diverged or exceeded their bound"};
    }
    if (canonical_.integrity_failed() || chunked_.integrity_failed() ||
        projected_.integrity_failed() || projected_chunked_.integrity_failed()) {
      return std::string{"a generated bounded terminal history lost semantic integrity"};
    }
    if (const auto error = validate_allocations(); error.has_value()) {
      return error;
    }
    if (const auto error = compare_formatted(canonical_, chunked_, vt::ScreenFormat::vt_full,
                                             "canonical and chunked terminal state diverged");
        error.has_value()) {
      return error;
    }
    last_state_hash_ = hash_terminal_state(*canonical_inspection);
    if (const auto error =
            compare_formatted(projected_, projected_chunked_, vt::ScreenFormat::vt_full,
                              "projected outer terminal state diverged");
        error.has_value()) {
      return error;
    }
    if (projection_current_) {
      if (const auto error = compare_visible_text(); error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto finish() -> std::optional<std::string> {
    while (canonical_.pending_pty_response_bytes() > 0 ||
           chunked_.pending_pty_response_bytes() > 0) {
      TerminalOperation operation{.kind = TerminalOperationKind::drain_pty};
      std::array<std::byte, input_bytes_max> first{};
      std::array<std::byte, input_bytes_max> second{};
      const auto first_size = canonical_.read_pty_responses(first);
      const auto second_size = chunked_.read_pty_responses(second);
      if (first_size != second_size || !std::ranges::equal(std::span(first).first(first_size),
                                                           std::span(second).first(second_size))) {
        return std::string{"final PTY response drain diverged"};
      }
      retain_child_bytes(std::span(first).first(first_size));
      operation.result = static_cast<std::int32_t>(first_size);
    }
    if (const auto error = consume_effects(); error.has_value()) {
      return error;
    }
    return validate();
  }

  [[nodiscard]] constexpr auto state_hash() const noexcept -> std::uint64_t {
    return last_state_hash_;
  }
  [[nodiscard]] constexpr auto coverage() const noexcept -> const TerminalCoverage& {
    return coverage_;
  }

private:
  [[nodiscard]] static auto initial_options() noexcept -> vt::TerminalOptions {
    vt::TerminalOptions options;
    options.size = {.columns = 24, .rows = 6, .cell_width_px = 8, .cell_height_px = 16};
    options.scrollback_lines_max = 256;
    return options;
  }

  [[nodiscard]] auto write(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    const auto payload_index = random.index(terminal_payloads.size());
    const auto payload = std::span(terminal_payloads).subspan(payload_index, 1).front();
    const auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));
    const auto split_seed = random.next();
    operation.argument_0 = static_cast<std::uint16_t>(payload_index);
    operation.argument_1 = static_cast<std::uint16_t>(split_seed);
    operation.argument_2 = static_cast<std::uint16_t>(bytes.size());
    operation.result = static_cast<std::int32_t>(bytes.size());

    canonical_.write(bytes);
    Random chunks(split_seed);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto remaining = bytes.size() - offset;
      const auto length = 1U + chunks.index(std::min<std::size_t>(remaining, 11U));
      chunked_.write(bytes.subspan(offset, length));
      offset += length;
    }
    projection_current_ = false;
    return std::nullopt;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto resize(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    const auto before = canonical_.size();
    vt::TerminalSize requested;
    if (random.index(8) == 0) {
      requested = random.boolean() ? vt::TerminalSize{.columns = 0, .rows = before.rows}
                                   : vt::TerminalSize{.columns = before.columns, .rows = 0};
    } else {
      requested = {
          .columns = random.between(6, 40),
          .rows = random.between(2, 12),
          .cell_width_px = random.boolean() ? 8U : 0U,
          .cell_height_px = random.boolean() ? 16U : 0U,
      };
    }
    operation.argument_0 = requested.columns;
    operation.argument_1 = requested.rows;
    operation.argument_2 = static_cast<std::uint16_t>(requested.cell_width_px);

    if (std::getenv("FRAME_SIM_TRACE") != nullptr) {
      const auto inspection = canonical_.inspection();
      std::cerr << "next " << operation;
      if (inspection.has_value()) {
        std::cerr << " screen=" << static_cast<std::uint16_t>(inspection->active_screen)
                  << " follows=" << inspection->viewport.follows_output;
      }
      std::cerr << '\n';
    }
    const auto first = canonical_.resize(requested);
    const auto second = chunked_.resize(requested);
    if (first.has_value() != second.has_value() ||
        (!first.has_value() && first.error() != second.error())) {
      return std::string{"canonical and chunked resize outcomes diverged"};
    }
    if (!first.has_value()) {
      operation.result = -static_cast<std::int32_t>(first.error()) - 1;
      if (canonical_.size() != before || chunked_.size() != before) {
        return std::string{"rejected terminal resize mutated geometry"};
      }
      return std::nullopt;
    }

    const auto projected = projected_.resize(requested);
    const auto projected_chunked = projected_chunked_.resize(requested);
    if (!projected.has_value() || !projected_chunked.has_value()) {
      return std::string{"projected terminal rejected an applied source resize"};
    }
    operation.result = 1;
    projection_current_ = false;
    force_full_required_ = true;
    return discard_projected_callbacks();
  }

  [[nodiscard]] auto compose(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    const bool force_full = random.boolean() || force_full_required_;
    const bool constrained = random.index(5) == 0;
    const auto capacity = constrained ? random.index(129) : frame_buffer_.size();
    operation.argument_0 = static_cast<std::uint16_t>(force_full);
    operation.argument_1 = static_cast<std::uint16_t>(constrained);
    operation.argument_2 = static_cast<std::uint16_t>(capacity);

    const auto synchronized = canonical_.synchronized_output();
    const auto chunked_synchronized = chunked_.synchronized_output();
    if (!synchronized.has_value() || synchronized != chunked_synchronized) {
      return std::string{"synchronized-output state diverged before composition"};
    }
    const render::PaneSurface first_surface{
        .terminal = &canonical_,
        .rectangle = {.columns = canonical_.size().columns, .rows = canonical_.size().rows},
        .focused = true,
        .presentation_suppressed = *synchronized,
    };
    const render::PaneSurface second_surface{
        .terminal = &chunked_,
        .rectangle = {.columns = chunked_.size().columns, .rows = chunked_.size().rows},
        .focused = true,
        .presentation_suppressed = *chunked_synchronized,
    };
    const render::Viewport viewport{.columns = canonical_.size().columns,
                                    .rows = canonical_.size().rows};
    const auto first = render::compose_frame(std::span(&first_surface, 1), viewport,
                                             std::span(frame_buffer_).first(capacity), force_full,
                                             {}, previous_outer_modes_);
    const auto second = render::compose_frame(std::span(&second_surface, 1), viewport,
                                              std::span(frame_buffer_chunked_).first(capacity),
                                              force_full, {}, previous_outer_modes_chunked_);
    if (first.has_value() != second.has_value() ||
        (!first.has_value() && first.error() != second.error())) {
      return std::string{"canonical and chunked composition outcomes diverged"};
    }
    if (!first.has_value()) {
      operation.result = -static_cast<std::int32_t>(first.error()) - 1;
      projection_current_ = false;
      force_full_required_ = true;
      return std::nullopt;
    }
    if (first->bytes != second->bytes || first->panes != second->panes ||
        first->rows != second->rows || first->outer_modes != second->outer_modes ||
        first->full != second->full || first->status != second->status ||
        !std::ranges::equal(std::span(frame_buffer_).first(first->bytes),
                            std::span(frame_buffer_chunked_).first(second->bytes))) {
      return std::string{"canonical and chunked composed frames diverged"};
    }

    projected_.write(std::span(frame_buffer_).first(first->bytes));
    projected_chunked_.write(std::span(frame_buffer_chunked_).first(second->bytes));
    previous_outer_modes_ = first->outer_modes;
    previous_outer_modes_chunked_ = second->outer_modes;
    operation.result = static_cast<std::int32_t>(first->bytes);
    projection_current_ = !*synchronized;
    force_full_required_ = *synchronized;
    return discard_projected_callbacks();
  }

  [[nodiscard]] auto encode_key(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    constexpr std::array keys{vt::Key::a,           vt::Key::c,        vt::Key::enter,
                              vt::Key::tab,         vt::Key::arrow_up, vt::Key::arrow_left,
                              vt::Key::arrow_right, vt::Key::f1,       vt::Key::f4};
    constexpr std::array actions{vt::KeyAction::press, vt::KeyAction::release,
                                 vt::KeyAction::repeat};
    constexpr std::array modifiers{std::uint16_t{0}, vt::key_modifier_shift,
                                   vt::key_modifier_control, vt::key_modifier_alt};
    const auto key = std::span(keys).subspan(random.index(keys.size()), 1).front();
    const auto action = std::span(actions).subspan(random.index(actions.size()), 1).front();
    const auto modifier = std::span(modifiers).subspan(random.index(modifiers.size()), 1).front();
    const bool letter = key == vt::Key::a || key == vt::Key::c;
    const char character = key == vt::Key::c ? 'c' : 'a';
    const std::string_view text = letter ? std::string_view(&character, 1) : std::string_view{};
    const vt::KeyEvent event{
        .action = action,
        .key = key,
        .modifiers = modifier,
        .unshifted_codepoint = letter ? static_cast<std::uint32_t>(character) : 0U,
        .text = text,
    };
    const auto capacity = random.index(input_buffer_.size() + 1U);
    operation.argument_0 = static_cast<std::uint16_t>(key);
    operation.argument_1 = static_cast<std::uint16_t>(action);
    operation.argument_2 = static_cast<std::uint16_t>(capacity);
    const auto first = canonical_.encode_key(event, std::span(input_buffer_).first(capacity));
    const auto second =
        chunked_.encode_key(event, std::span(input_buffer_chunked_).first(capacity));
    return compare_encoded(first, second, capacity, operation, "key encoding diverged");
  }

  [[nodiscard]] auto encode_paste(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    constexpr std::array payloads{
        std::string_view{"line one\nline two"},
        std::string_view{"safe paste"},
        std::string_view{"a\x1B"
                         "b\tend"},
    };
    const auto payload_index = random.index(payloads.size());
    const auto payload = std::span(payloads).subspan(payload_index, 1).front();
    std::array<std::byte, 32> first_input{};
    std::array<std::byte, 32> second_input{};
    std::memcpy(first_input.data(), payload.data(), payload.size());
    std::memcpy(second_input.data(), payload.data(), payload.size());
    const auto capacity = random.index(input_buffer_.size() + 1U);
    operation.argument_0 = static_cast<std::uint16_t>(payload_index);
    operation.argument_1 = static_cast<std::uint16_t>(payload.size());
    operation.argument_2 = static_cast<std::uint16_t>(capacity);
    const auto first = canonical_.encode_paste(std::span(first_input).first(payload.size()),
                                               std::span(input_buffer_).first(capacity));
    const auto second = chunked_.encode_paste(std::span(second_input).first(payload.size()),
                                              std::span(input_buffer_chunked_).first(capacity));
    if (!std::ranges::equal(first_input, second_input)) {
      return std::string{"paste filtering diverged"};
    }
    return compare_encoded(first, second, capacity, operation, "paste encoding diverged");
  }

  [[nodiscard]] auto encode_focus(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    const auto event = random.boolean() ? vt::FocusEvent::gained : vt::FocusEvent::lost;
    const auto capacity = random.index(9);
    operation.argument_0 = static_cast<std::uint16_t>(event);
    operation.argument_1 = static_cast<std::uint16_t>(capacity);
    const auto first = canonical_.encode_focus(event, std::span(input_buffer_).first(capacity));
    const auto second =
        chunked_.encode_focus(event, std::span(input_buffer_chunked_).first(capacity));
    return compare_encoded(first, second, capacity, operation, "focus encoding diverged");
  }

  [[nodiscard]] auto encode_mouse(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    constexpr std::array actions{vt::MouseAction::press, vt::MouseAction::release,
                                 vt::MouseAction::motion};
    constexpr std::array buttons{vt::MouseButton::left, vt::MouseButton::right,
                                 vt::MouseButton::middle};
    const auto size = canonical_.size();
    constexpr std::uint32_t cell_width = 8;
    constexpr std::uint32_t cell_height = 16;
    const auto column = random.between(0, static_cast<std::uint16_t>(size.columns - 1U));
    const auto row = random.between(0, static_cast<std::uint16_t>(size.rows - 1U));
    const auto action = std::span(actions).subspan(random.index(actions.size()), 1).front();
    const bool has_button = action != vt::MouseAction::motion || random.boolean();
    const vt::MouseEvent event{
        .action = action,
        .button = has_button
                      ? std::optional<vt::MouseButton>{std::span(buttons)
                                                           .subspan(random.index(buttons.size()), 1)
                                                           .front()}
                      : std::nullopt,
        .modifiers = random.boolean() ? vt::key_modifier_shift : std::uint16_t{0},
        .x = static_cast<float>((static_cast<std::uint32_t>(column) * cell_width) + 1U),
        .y = static_cast<float>((static_cast<std::uint32_t>(row) * cell_height) + 1U),
        .geometry = {.screen_width = static_cast<std::uint32_t>(size.columns) * cell_width,
                     .screen_height = static_cast<std::uint32_t>(size.rows) * cell_height,
                     .cell_width = cell_width,
                     .cell_height = cell_height},
        .any_button_pressed = has_button,
    };
    const auto capacity = random.index(33);
    operation.argument_0 = column;
    operation.argument_1 = row;
    operation.argument_2 = static_cast<std::uint16_t>(capacity);
    const auto first = canonical_.encode_mouse(event, std::span(input_buffer_).first(capacity));
    const auto second =
        chunked_.encode_mouse(event, std::span(input_buffer_chunked_).first(capacity));
    return compare_encoded(first, second, capacity, operation, "mouse encoding diverged");
  }

  [[nodiscard]] auto drain_pty(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    const auto capacity = random.index(input_buffer_.size() + 1U);
    operation.argument_0 = static_cast<std::uint16_t>(capacity);
    const auto first = canonical_.read_pty_responses(std::span(input_buffer_).first(capacity));
    const auto second =
        chunked_.read_pty_responses(std::span(input_buffer_chunked_).first(capacity));
    if (first != second || !std::ranges::equal(std::span(input_buffer_).first(first),
                                               std::span(input_buffer_chunked_).first(second))) {
      return std::string{"partial PTY response drains diverged"};
    }
    retain_child_bytes(std::span(input_buffer_).first(first));
    operation.result = static_cast<std::int32_t>(first);
    return std::nullopt;
  }

  [[nodiscard]] auto set_theme(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    auto theme = vt::default_theme();
    theme.foreground = {.red = static_cast<std::uint8_t>(random.next()),
                        .green = static_cast<std::uint8_t>(random.next()),
                        .blue = static_cast<std::uint8_t>(random.next())};
    theme.background = {.red = static_cast<std::uint8_t>(random.next()),
                        .green = static_cast<std::uint8_t>(random.next()),
                        .blue = static_cast<std::uint8_t>(random.next())};
    theme.cursor = {.red = static_cast<std::uint8_t>(random.next()),
                    .green = static_cast<std::uint8_t>(random.next()),
                    .blue = static_cast<std::uint8_t>(random.next())};
    const auto palette_index = random.index(theme.palette.size());
    std::span(theme.palette).subspan(palette_index, 1).front() = theme.foreground;
    operation.argument_0 = static_cast<std::uint16_t>(palette_index);
    operation.argument_1 = theme.foreground.red;
    operation.argument_2 = theme.background.red;

    const auto first = canonical_.set_theme(theme);
    const auto second = chunked_.set_theme(theme);
    const auto outer = projected_.set_theme(theme);
    const auto outer_chunked = projected_chunked_.set_theme(theme);
    if (!first.has_value() || !second.has_value() || !outer.has_value() ||
        !outer_chunked.has_value()) {
      return std::string{"generated valid theme replacement failed"};
    }
    operation.result = 1;
    projection_current_ = false;
    force_full_required_ = true;
    return std::nullopt;
  }

  [[nodiscard]] auto scroll_viewport(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    constexpr std::array behaviors{vt::ViewportScroll::top, vt::ViewportScroll::bottom,
                                   vt::ViewportScroll::delta, vt::ViewportScroll::row};
    const auto behavior = std::span(behaviors).subspan(random.index(behaviors.size()), 1).front();
    const auto value = behavior == vt::ViewportScroll::delta
                           ? static_cast<std::int64_t>(random.index(7)) - 3
                           : static_cast<std::int64_t>(random.index(32));
    operation.argument_0 = static_cast<std::uint16_t>(behavior);
    operation.argument_1 = static_cast<std::uint16_t>(value + 32);
    canonical_.scroll_viewport(behavior, value);
    chunked_.scroll_viewport(behavior, value);
    projection_current_ = false;
    operation.result = 1;
    return std::nullopt;
  }

  [[nodiscard]] auto select(Random& random, TerminalOperation& operation)
      -> std::optional<std::string> {
    if (random.index(5) == 0) {
      canonical_.clear_selection();
      chunked_.clear_selection();
      operation.result = 1;
      projection_current_ = false;
      return std::nullopt;
    }
    constexpr std::array units{vt::SelectionUnit::cell, vt::SelectionUnit::word,
                               vt::SelectionUnit::line, vt::SelectionUnit::block};
    const auto unit = std::span(units).subspan(random.index(units.size()), 1).front();
    const auto size = canonical_.size();
    const vt::TerminalPoint point{
        .space = vt::PointSpace::viewport,
        .column = random.between(0, static_cast<std::uint16_t>(size.columns - 1U)),
        .row = random.between(0, static_cast<std::uint16_t>(size.rows - 1U)),
    };
    operation.argument_0 = static_cast<std::uint16_t>(unit);
    operation.argument_1 = point.column;
    operation.argument_2 = static_cast<std::uint16_t>(point.row);
    const auto first = canonical_.select(unit, point);
    const auto second = chunked_.select(unit, point);
    if (first.has_value() != second.has_value() || (first.has_value() && *first != *second) ||
        (!first.has_value() && first.error() != second.error())) {
      return std::string{"selection outcomes diverged"};
    }
    operation.result = first.has_value() ? static_cast<std::int32_t>(*first)
                                         : -static_cast<std::int32_t>(first.error()) - 1;
    projection_current_ = false;
    return std::nullopt;
  }

  [[nodiscard]] auto invalidate_render(TerminalOperation& operation) -> std::optional<std::string> {
    canonical_.invalidate_ansi_render_state();
    chunked_.invalidate_ansi_render_state();
    force_full_required_ = true;
    operation.result = 1;
    return std::nullopt;
  }

  template <typename Expected>
  [[nodiscard]] auto compare_encoded(const Expected& first, const Expected& second,
                                     const std::size_t capacity, TerminalOperation& operation,
                                     const std::string_view failure) -> std::optional<std::string> {
    if (first.has_value() != second.has_value() ||
        (!first.has_value() && first.error() != second.error())) {
      return std::string(failure);
    }
    if (!first.has_value()) {
      operation.result = -static_cast<std::int32_t>(first.error()) - 1;
      return std::nullopt;
    }
    if (*first != *second || *first > capacity ||
        !std::ranges::equal(std::span(input_buffer_).first(*first),
                            std::span(input_buffer_chunked_).first(*second))) {
      return std::string(failure);
    }
    retain_child_bytes(std::span(input_buffer_).first(*first));
    operation.result = static_cast<std::int32_t>(*first);
    return std::nullopt;
  }

  [[nodiscard]] auto consume_effects() -> std::optional<std::string> {
    const auto first = canonical_.take_effects();
    const auto second = chunked_.take_effects();
    if (!effects_equal(first, second)) {
      return std::string{"terminal effects differed across write fragmentation"};
    }
    accumulate_effects(total_effects_, first);
    return std::nullopt;
  }

  [[nodiscard]] auto discard_projected_callbacks() -> std::optional<std::string> {
    const auto first_effects = projected_.take_effects();
    const auto second_effects = projected_chunked_.take_effects();
    if (!effects_equal(first_effects, second_effects)) {
      return std::string{"projected terminal effects diverged"};
    }
    while (projected_.pending_pty_response_bytes() > 0 ||
           projected_chunked_.pending_pty_response_bytes() > 0) {
      const auto first = projected_.read_pty_responses(input_buffer_);
      const auto second = projected_chunked_.read_pty_responses(input_buffer_chunked_);
      if (first != second || !std::ranges::equal(std::span(input_buffer_).first(first),
                                                 std::span(input_buffer_chunked_).first(second))) {
        return std::string{"projected terminal PTY responses diverged"};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto validate_modes() const -> std::optional<std::string> {
    const auto first_mouse = canonical_.mouse_tracking();
    const auto second_mouse = chunked_.mouse_tracking();
    const auto first_wheel = canonical_.wheel_uses_alternate_scroll();
    const auto second_wheel = chunked_.wheel_uses_alternate_scroll();
    const auto first_sync = canonical_.synchronized_output();
    const auto second_sync = chunked_.synchronized_output();
    if (!first_mouse.has_value() || first_mouse != second_mouse || !first_wheel.has_value() ||
        first_wheel != second_wheel || !first_sync.has_value() || first_sync != second_sync) {
      return std::string{"canonical input or presentation modes diverged"};
    }
    return std::nullopt;
  }

  [[nodiscard]] auto validate_allocations() const -> std::optional<std::string> {
    const std::array stats{canonical_.allocation_stats(), chunked_.allocation_stats(),
                           projected_.allocation_stats(), projected_chunked_.allocation_stats()};
    for (const auto& value : stats) {
      if (value.bytes_current > options_.allocation_bytes_max ||
          value.bytes_peak > options_.allocation_bytes_max || value.failures_total != 0) {
        return std::string{"Ghostty allocation accounting exceeded its configured quota"};
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto compare_visible_text() -> std::optional<std::string> {
    const auto rows = canonical_.size().rows;
    const auto first_size =
        canonical_.format_visible_tail(vt::ScreenFormat::plain, rows, snapshot_first_);
    const auto second_size =
        projected_.format_visible_tail(vt::ScreenFormat::plain, rows, snapshot_second_);
    if (!first_size.has_value() || !second_size.has_value()) {
      return std::string{"visible terminal formatting failed"};
    }
    const auto trim_blank_tail = [](std::span<const std::byte> bytes) noexcept {
      while (!bytes.empty()) {
        const auto value = std::to_integer<std::uint8_t>(bytes.back());
        if (value != static_cast<std::uint8_t>(' ') && value != static_cast<std::uint8_t>('\r') &&
            value != static_cast<std::uint8_t>('\n')) {
          break;
        }
        bytes = bytes.first(bytes.size() - 1U);
      }
      return bytes;
    };
    const auto first = trim_blank_tail(std::span(snapshot_first_).first(*first_size));
    const auto second = trim_blank_tail(std::span(snapshot_second_).first(*second_size));
    if (std::ranges::equal(first, second)) {
      return std::nullopt;
    }
    // Visible text is copied only on the failure path for replay diagnostics.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string first_text(reinterpret_cast<const char*>(first.data()), first.size());
    const std::string second_text(reinterpret_cast<const char*>(second.data()), second.size());
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::string{"composed output did not reproduce visible text: source=["} + first_text +
           "] projected=[" + second_text + "]";
  }

  [[nodiscard]] auto compare_formatted(vt::Terminal& first, vt::Terminal& second,
                                       const vt::ScreenFormat format,
                                       const std::string_view failure)
      -> std::optional<std::string> {
    const auto first_size = first.format_screen(format, snapshot_first_);
    const auto second_size = second.format_screen(format, snapshot_second_);
    if (!first_size.has_value() || !second_size.has_value()) {
      return std::string(failure) + ": formatting failed";
    }
    if (*first_size != *second_size ||
        !std::ranges::equal(std::span(snapshot_first_).first(*first_size),
                            std::span(snapshot_second_).first(*second_size))) {
      const auto first_diagnostic =
          std::span(snapshot_first_).first(std::min(*first_size, std::size_t{256}));
      const auto second_diagnostic =
          std::span(snapshot_second_).first(std::min(*second_size, std::size_t{256}));
      // Terminal formatter output is UTF-8/ANSI and is copied only on the failure path.
      // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
      const std::string first_text(reinterpret_cast<const char*>(first_diagnostic.data()),
                                   first_diagnostic.size());
      const std::string second_text(reinterpret_cast<const char*>(second_diagnostic.data()),
                                    second_diagnostic.size());
      // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
      return std::string(failure) + ": first=" + std::to_string(*first_size) + " [" + first_text +
             "] second=" + std::to_string(*second_size) + " [" + second_text + "]";
    }
    return std::nullopt;
  }

  [[nodiscard]] auto hash_terminal_state(const vt::TerminalInspection& inspection) noexcept
      -> std::uint64_t {
    const auto formatted = canonical_.format_screen(vt::ScreenFormat::vt_full, snapshot_first_);
    if (!formatted.has_value()) {
      return 0;
    }
    auto hash =
        hash_bytes(14'695'981'039'346'656'037ULL, std::span(snapshot_first_).first(*formatted));
    const auto size = canonical_.size();
    hash = hash_value(hash, size.columns);
    hash = hash_value(hash, size.rows);
    hash = hash_value(hash, size.cell_width_px);
    hash = hash_value(hash, size.cell_height_px);
    hash = hash_value(hash, inspection.viewport.total_rows);
    hash = hash_value(hash, inspection.viewport.offset);
    hash = hash_value(hash, inspection.viewport.visible_rows);
    hash = hash_value(hash, inspection.cursor_column);
    hash = hash_value(hash, inspection.cursor_row);
    hash = hash_value(hash, static_cast<std::uint64_t>(inspection.active_screen));
    hash = hash_value(hash, canonical_.pending_pty_response_bytes());
    hash = hash_value(hash, child_bytes_hash_);
    hash = hash_value(hash, child_bytes_count_);
    hash = hash_value(hash, total_effects_.bells);
    hash = hash_value(hash, total_effects_.title_changes);
    hash = hash_value(hash, total_effects_.pwd_changes);
    hash = hash_value(hash, total_effects_.desktop_notifications);
    hash = hash_value(hash, total_effects_.progress_reports);
    hash = hash_value(hash, total_effects_.clipboard_writes_denied);
    hash = hash_value(hash, total_effects_.unknown_sequences_dropped);
    const auto theme = canonical_.theme();
    const std::array principal{theme.foreground, theme.background, theme.cursor};
    for (const auto color : principal) {
      hash = hash_byte(hash, color.red);
      hash = hash_byte(hash, color.green);
      hash = hash_byte(hash, color.blue);
    }
    for (const auto color : theme.palette) {
      hash = hash_byte(hash, color.red);
      hash = hash_byte(hash, color.green);
      hash = hash_byte(hash, color.blue);
    }
    return hash;
  }

  void retain_child_bytes(const std::span<const std::byte> bytes) noexcept {
    child_bytes_hash_ = hash_bytes(child_bytes_hash_, bytes);
    child_bytes_count_ += bytes.size();
  }

  vt::TerminalOptions options_;
  vt::Terminal canonical_;
  vt::Terminal chunked_;
  vt::Terminal projected_;
  vt::Terminal projected_chunked_;
  std::optional<render::OuterModeProjection> previous_outer_modes_;
  std::optional<render::OuterModeProjection> previous_outer_modes_chunked_;
  std::array<std::byte, frame_bytes_max> frame_buffer_{};
  std::array<std::byte, frame_bytes_max> frame_buffer_chunked_{};
  std::array<std::byte, snapshot_bytes_max> snapshot_first_{};
  std::array<std::byte, snapshot_bytes_max> snapshot_second_{};
  std::array<std::byte, input_bytes_max> input_buffer_{};
  std::array<std::byte, input_bytes_max> input_buffer_chunked_{};
  vt::EffectBatch total_effects_{};
  TerminalCoverage coverage_{};
  std::uint64_t child_bytes_hash_{14'695'981'039'346'656'037ULL};
  std::uint64_t child_bytes_count_{0};
  std::uint64_t last_state_hash_{0};
  bool projection_current_{false};
  bool force_full_required_{true};
};

[[nodiscard]] auto parse_environment_integer(const char* const name, std::uint64_t& result) noexcept
    -> bool {
  const auto* const encoded = std::getenv(name);
  if (encoded == nullptr) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const auto parsed = std::strtoull(encoded, &end, 0);
  if (errno != 0 || end == encoded || *end != '\0') {
    return false;
  }
  result = parsed;
  return true;
}

[[nodiscard]] auto run_terminal_world(const std::uint64_t seed, const std::size_t operation_count,
                                      std::uint64_t* const final_state_hash = nullptr,
                                      TerminalCoverage* const coverage = nullptr)
    -> testing::AssertionResult {
  auto trace = std::make_unique<TerminalTrace>(seed, operation_count);
  auto world = std::make_unique<TerminalWorld>();
  Random random(seed);
  for (std::size_t index = 0; index < operation_count; ++index) {
    TerminalOperation operation;
    const auto applied = world->apply(random, operation);
    if (!trace->append(operation)) {
      return testing::AssertionFailure() << "terminal simulation trace capacity exhausted\n"
                                         << *trace;
    }
    if (applied.has_value()) {
      return testing::AssertionFailure() << *applied << " at operation " << index << '\n' << *trace;
    }
    if (const auto invariant = world->validate(); invariant.has_value()) {
      return testing::AssertionFailure() << *invariant << " at operation " << index << '\n'
                                         << *trace;
    }
    trace->complete_last(world->state_hash());
    if (std::getenv("FRAME_SIM_TRACE") != nullptr) {
      std::cerr << index << ' ' << operation << '\n';
    }
  }
  if (const auto finished = world->finish(); finished.has_value()) {
    return testing::AssertionFailure() << *finished << " while quiescing terminal world\n"
                                       << *trace;
  }
  if (final_state_hash != nullptr) {
    *final_state_hash = world->state_hash();
  }
  if (coverage != nullptr) {
    coverage->merge(world->coverage());
  }
  return testing::AssertionSuccess();
}

TEST(TerminalSimulationTest, SameSeedAndConfigurationReachTheSameState) {
  std::uint64_t first_hash = 0;
  std::uint64_t second_hash = 0;
  ASSERT_TRUE(run_terminal_world(0x6A0577EEDULL, 128, &first_hash));
  ASSERT_TRUE(run_terminal_world(0x6A0577EEDULL, 128, &second_hash));
  EXPECT_EQ(first_hash, second_hash);
}

// Every single cut is cheap enough to exhaust; random histories then cover longer combinations.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalExhaustiveTest, StructuredPayloadsAreEquivalentAtEveryFragmentBoundary) {
  vt::TerminalOptions options;
  options.size = {.columns = 32, .rows = 8};
  options.scrollback_lines_max = 256;
  for (std::size_t payload_index = 0; payload_index < terminal_payloads.size(); ++payload_index) {
    const auto payload = terminal_payloads.at(payload_index);
    for (std::size_t split = 0; split <= payload.size(); ++split) {
      SCOPED_TRACE(testing::Message() << "payload=" << payload_index << " split=" << split);
      auto whole = make_terminal(options);
      auto fragmented = make_terminal(options);
      const auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));
      whole.write(bytes);
      fragmented.write(bytes.first(split));
      fragmented.write(bytes.subspan(split));

      const auto whole_inspection = whole.inspection();
      const auto fragmented_inspection = fragmented.inspection();
      ASSERT_TRUE(whole_inspection.has_value());
      ASSERT_TRUE(fragmented_inspection.has_value());
      EXPECT_TRUE(inspection_equal(*whole_inspection, *fragmented_inspection));
      EXPECT_TRUE(effects_equal(whole.take_effects(), fragmented.take_effects()));
      std::array<std::byte, snapshot_bytes_max> whole_screen{};
      std::array<std::byte, snapshot_bytes_max> fragmented_screen{};
      const auto whole_size = whole.format_screen(vt::ScreenFormat::vt_full, whole_screen);
      const auto fragmented_size =
          fragmented.format_screen(vt::ScreenFormat::vt_full, fragmented_screen);
      ASSERT_TRUE(whole_size.has_value());
      ASSERT_TRUE(fragmented_size.has_value());
      EXPECT_TRUE(std::ranges::equal(std::span(whole_screen).first(*whole_size),
                                     std::span(fragmented_screen).first(*fragmented_size)));
      std::array<std::byte, input_bytes_max> whole_responses{};
      std::array<std::byte, input_bytes_max> fragmented_responses{};
      while (whole.pending_pty_response_bytes() > 0 ||
             fragmented.pending_pty_response_bytes() > 0) {
        const auto whole_read = whole.read_pty_responses(whole_responses);
        const auto fragmented_read = fragmented.read_pty_responses(fragmented_responses);
        ASSERT_EQ(whole_read, fragmented_read);
        EXPECT_TRUE(std::ranges::equal(std::span(whole_responses).first(whole_read),
                                       std::span(fragmented_responses).first(fragmented_read)));
      }
    }

    auto whole = make_terminal(options);
    auto bytewise = make_terminal(options);
    const auto bytes = std::as_bytes(std::span(payload.data(), payload.size()));
    whole.write(bytes);
    for (const auto byte : bytes) {
      bytewise.write(std::span(&byte, 1));
    }
    std::array<std::byte, snapshot_bytes_max> whole_screen{};
    std::array<std::byte, snapshot_bytes_max> bytewise_screen{};
    const auto whole_size = whole.format_screen(vt::ScreenFormat::vt_full, whole_screen);
    const auto bytewise_size = bytewise.format_screen(vt::ScreenFormat::vt_full, bytewise_screen);
    ASSERT_TRUE(whole_size.has_value());
    ASSERT_TRUE(bytewise_size.has_value());
    EXPECT_TRUE(std::ranges::equal(std::span(whole_screen).first(*whole_size),
                                   std::span(bytewise_screen).first(*bytewise_size)))
        << "payload=" << payload_index;
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalExhaustiveTest, RenderExhaustionIsTransactionalAtTheExactCapacityBoundary) {
  vt::TerminalOptions options;
  options.size = {.columns = 12, .rows = 3};
  constexpr std::string_view payload = "A\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"
                                       "e\xCC\x81\x1B[1;31mred\x1B[0m";
  auto reference = make_terminal(options);
  reference.write(std::as_bytes(std::span(payload.data(), payload.size())));
  std::array<std::byte, std::size_t{64} * 1'024U> expected{};
  const auto rendered = reference.render_ansi(expected, true);
  ASSERT_TRUE(rendered.has_value());
  const auto required = rendered->bytes;
  ASSERT_GT(required, 1U);

  for (const auto capacity : {required - 1U, required, required + 1U}) {
    SCOPED_TRACE(testing::Message() << "capacity=" << capacity << " required=" << required);
    auto terminal = make_terminal(options);
    terminal.write(std::as_bytes(std::span(payload.data(), payload.size())));
    std::array<std::byte, std::size_t{64} * 1'024U> output{};
    const auto first = terminal.render_ansi(std::span(output).first(capacity), true);
    if (capacity < required) {
      ASSERT_FALSE(first.has_value());
      EXPECT_EQ(first.error(), vt::Error::out_of_space);
      const auto retry = terminal.render_ansi(output, true);
      ASSERT_TRUE(retry.has_value());
      EXPECT_EQ(retry->bytes, required);
      EXPECT_TRUE(std::ranges::equal(std::span(output).first(retry->bytes),
                                     std::span(expected).first(required)));
    } else {
      ASSERT_TRUE(first.has_value());
      EXPECT_EQ(first->bytes, required);
      EXPECT_TRUE(std::ranges::equal(std::span(output).first(first->bytes),
                                     std::span(expected).first(required)));
    }
  }
}

// Generated histories intentionally combine parser, input, resize, effects, and composition paths.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalSimulationTest, GeneratedHistoriesPreserveFragmentationAndProjectionInvariants) {
  constexpr std::array<std::uint64_t, 6> default_seeds{
      0ULL,
      1ULL,
      0xC0FFEEULL,
      0x51A7E123ULL,
      0xDEADBEEFCAFEBABEULL,
      std::numeric_limits<std::uint64_t>::max(),
  };

  std::uint64_t configured_seed = 0;
  std::uint64_t configured_operations = terminal_default_operations;
  const bool has_seed = std::getenv("FRAME_SIM_SEED") != nullptr;
  if (has_seed) {
    ASSERT_TRUE(parse_environment_integer("FRAME_SIM_SEED", configured_seed))
        << "FRAME_SIM_SEED must be an integer accepted by strtoull";
  }
  if (std::getenv("FRAME_SIM_OPERATIONS") != nullptr) {
    ASSERT_TRUE(parse_environment_integer("FRAME_SIM_OPERATIONS", configured_operations))
        << "FRAME_SIM_OPERATIONS must be an integer accepted by strtoull";
  }
  ASSERT_GT(configured_operations, 0U);
  ASSERT_LE(configured_operations, terminal_trace_operations_max);

  if (has_seed) {
    EXPECT_TRUE(
        run_terminal_world(configured_seed, static_cast<std::size_t>(configured_operations)));
    return;
  }

  TerminalCoverage coverage;
  for (const auto seed : default_seeds) {
    SCOPED_TRACE(testing::Message() << "seed=0x" << std::hex << seed);
    EXPECT_TRUE(run_terminal_world(seed, static_cast<std::size_t>(configured_operations), nullptr,
                                   &coverage));
  }
  for (std::size_t index = 0; index < coverage.operations.size(); ++index) {
    EXPECT_GT(std::span(coverage.operations).subspan(index, 1).front(), 0U)
        << "uncovered terminal operation kind " << index;
  }
  for (std::size_t index = 0; index < coverage.payloads.size(); ++index) {
    EXPECT_GT(std::span(coverage.payloads).subspan(index, 1).front(), 0U)
        << "uncovered terminal payload " << index;
  }
}

} // namespace
} // namespace frame::test::sim
