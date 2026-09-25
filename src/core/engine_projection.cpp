#include "core/engine_projection.hpp"
#include "clipboard/transaction.hpp"
#include "core/engine_connection_state.hpp"
#include "core/engine_state.hpp"

#include "extension/runtime.hpp"

#include "api/command.hpp"
#include "api/json.hpp"
#include "api/proc.hpp"
#include "core/client_frame_output.hpp"
#include "core/connection_output.hpp"
#include "core/layout.hpp"
#include "core/presentation_gate.hpp"
#include "core/session.hpp"
#include "diagnostic/latency_trace.hpp"
#include "input/input_router.hpp"
#include "lemma/assert.hpp"
#include "lemma/command.hpp"
#include "lemma/geometry.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "lemma/version.hpp"
#include "protocol/attachment.hpp"
#include "render/frame_buffer.hpp"
#include "render/pane_composition.hpp"
#include "render/scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lemma::core::engine_detail {

struct MessageViewStorage final {
  std::array<std::array<char, render::message_view_line_bytes_max>,
             limits::status_message_history_max>
      text{};
  std::array<render::MessageViewLine, limits::status_message_history_max> lines{};
  std::size_t size{0};
};

[[nodiscard]] auto format_message_view_line(const StatusMessageEntry& entry,
                                            const std::span<char> output) noexcept -> std::size_t {
  std::size_t used = 0;
  const auto timestamp = static_cast<std::time_t>(entry.unix_seconds);
  std::tm local{};
  if (::localtime_r(&timestamp, &local) != nullptr) {
    std::array<char, 32> encoded{};
    const auto count =
        std::strftime(encoded.data(), encoded.size(), "[%Y-%m-%d %H:%M:%S] ", &local);
    if (count <= output.size()) {
      std::ranges::copy(std::span(encoded).first(count), output.begin());
      used = count;
    }
  }
  const auto message_size = std::min(entry.view().size(), output.size() - used);
  std::ranges::copy(std::span(entry.view()).first(message_size), output.subspan(used).begin());
  return used + message_size;
}

[[nodiscard]] auto collect_message_view(SessionRecord& session,
                                        MessageViewStorage& storage) noexcept
    -> render::MessageView {
  if (!session.attachment.message_view.active) {
    return {};
  }
  const auto rows = static_cast<std::size_t>(pane_rows(session.attachment.rows));
  const auto log_size = static_cast<std::size_t>(session.attachment.status_messages.size);
  const auto maximum = log_size > rows ? log_size - rows : 0U;
  const auto offset = std::min<std::size_t>(session.attachment.message_view.offset, maximum);
  session.attachment.message_view.offset = static_cast<std::uint8_t>(offset);
  const auto count = std::min(rows, log_size - offset);
  const auto entries = std::span(session.attachment.status_messages.entries);
  auto texts = std::span(storage.text);
  auto lines = std::span(storage.lines);
  for (std::size_t position = 0; position < count; ++position) {
    const auto entry_index = offset + count - position - 1U;
    const auto& entry = entries.subspan(entry_index, 1U).front();
    auto& text = texts.subspan(position, 1U).front();
    const auto size = format_message_view_line(entry, text);
    lines.subspan(position, 1U).front() = {.text = std::string_view(text.data(), size),
                                           .error = entry.kind == StatusMessageKind::error};
  }
  storage.size = count;
  return {.lines = std::span(storage.lines).first(storage.size), .active = true};
}

// The one presentation predicate for Pane surfaces of an Attachment's active Tab.
[[nodiscard]] auto pane_presented(const Tab& active, const Pane& pane) noexcept -> bool {
  return !active.layout_suspended && pane.tab == active.id &&
         (!active.zoomed || pane.id == active.focused_pane);
}

// A full frame redraws every presented Pane. Panes absent from it keep no retained render rows;
// presenting one again rebuilds them with the full damage its first frame needs anyway.
void release_unpresented_render_caches(SessionRecord& session,
                                       PaneRuntimeStore& runtimes) noexcept {
  const auto* const tab = active_tab(session);
  for (const auto& pane_slot : session.panes) {
    const auto* const pane = pane_slot.pane.get();
    if (pane == nullptr || (tab != nullptr && pane_presented(*tab, *pane))) {
      continue;
    }
    auto* const runtime = find_pane_runtime(runtimes, session, *pane);
    if (runtime != nullptr) {
      runtime->terminal.release_render_cache();
    }
  }
}

// Surface projection combines bounded semantic and runtime state without retaining either.
[[nodiscard]] auto
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
collect_surfaces(SessionRecord& session, PaneRuntimeStore& runtimes,
                 std::array<render::PaneSurface, panes_per_tab_max>& storage) noexcept
    -> std::span<const render::PaneSurface> {
  auto* const tab = active_tab(session);
  if (tab == nullptr || tab->layout_suspended) {
    return std::span<const render::PaneSurface>{};
  }
  std::size_t count = 0;
  for (auto& pane_slot : session.panes) {
    auto& pane = pane_slot.pane;
    if (pane == nullptr || !pane_presented(*tab, *pane)) {
      continue;
    }
    auto* const runtime = find_pane_runtime(runtimes, session, *tab, *pane);
    if (runtime == nullptr || !runtime->live()) {
      continue;
    }
    const bool copy_pane =
        session.attachment.copy_mode.active() &&
        session.attachment.selection_target ==
            std::optional{AttachmentPaneTarget{.tab = tab->id, .pane = pane->id}};
    const auto copy_cursor = copy_pane
                                 ? runtime->terminal.selection_endpoint(vt::PointSpace::viewport)
                                 : std::expected<std::optional<vt::TerminalPoint>, vt::Error>{
                                       std::optional<vt::TerminalPoint>{}};
    const auto cursor = copy_cursor.value_or(std::optional<vt::TerminalPoint>{});
    const auto cursor_point = cursor.value_or(vt::TerminalPoint{});
    const bool cursor_override = cursor.has_value() && cursor_point.row < pane->rectangle.rows &&
                                 cursor_point.column < pane->rectangle.columns;
    std::span(storage).subspan(count, 1).front() = {
        .terminal = &runtime->terminal,
        .rectangle = pane->rectangle,
        .cursor_override_column = cursor_override ? cursor_point.column : std::uint16_t{0},
        .cursor_override_row =
            cursor_override ? static_cast<std::uint16_t>(cursor_point.row) : std::uint16_t{0},
        .focused = pane->id == tab->focused_pane,
        .cursor_override = cursor_override,
        .presentation_suppressed = runtime->presentation_gate.presentation_suppressed(),
        .border_right =
            static_cast<std::uint32_t>(pane->rectangle.column) + pane->rectangle.columns <
            static_cast<std::uint32_t>(tab->layout_column) + tab->layout_columns,
        .border_bottom = static_cast<std::uint32_t>(pane->rectangle.row) + pane->rectangle.rows <
                         static_cast<std::uint32_t>(tab->layout_row) + tab->layout_rows,
    };
    ++count;
  }
  return std::span(storage).first(count);
}

[[nodiscard]] auto tab_title(const SessionRecord& session, const Tab& tab,
                             const PaneRuntimeStore& runtimes) noexcept -> std::string_view {
  if (!tab.title_override().empty()) {
    return tab.title_override();
  }
  const auto* const focused = find_pane(session, tab, tab.focused_pane);
  LEMMA_ASSERT(focused != nullptr);
  const auto* const runtime = find_pane_runtime(runtimes, session, tab, *focused);
  LEMMA_ASSERT(runtime != nullptr);
  if (runtime->process_name_size > 0) {
    return {runtime->process_name.data(), runtime->process_name_size};
  }
  const auto title = runtime->terminal.title();
  return title.has_value() && !title->empty() ? *title : std::string_view{"shell"};
}

template <typename Mixer>
void mix_command_line_status(const CommandLineState& command_line, Mixer& mix) noexcept {
  mix(command_line.active ? 1U : 0U);
  mix(static_cast<std::uint8_t>(command_line.cursor));
  mix(static_cast<std::uint8_t>(command_line.cursor >> 8U));
  mix(static_cast<std::uint8_t>(command_line.size));
  mix(static_cast<std::uint8_t>(command_line.size >> 8U));
  for (const char character : command_line.view()) {
    mix(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
  }
}

[[nodiscard]] auto visible_status_message(const SessionRecord& session) noexcept
    -> std::string_view {
  const auto& attachment = session.attachment;
  return attachment.status_message_visible && attachment.status_messages.size > 0
             ? attachment.status_messages.entries.front().view()
             : std::string_view{};
}

[[nodiscard]] auto status_input_context(const SessionRecord& session) noexcept -> std::string_view {
  if (session.attachment.command_line.active || !visible_status_message(session).empty() ||
      session.attachment.copy_mode.phase == CopyModePhase::search_prompt) {
    return {};
  }
  if (session.attachment.copy_mode.active() || session.attachment.rename_prompt.active() ||
      session.attachment.message_view.active) {
    return session.interaction_router.active_label();
  }
  return session.input_router.active_label();
}

// The branches hash each bounded status projection into one invalidation signature.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto current_status_signature(const SessionRecord& session,
                                            const PaneRuntimeStore& runtimes) noexcept
    -> std::uint64_t {
  constexpr std::uint64_t offset_basis = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;
  std::uint64_t signature = offset_basis;
  const auto mix = [&](const std::uint8_t value) {
    signature ^= value;
    signature *= prime;
  };
  for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
    const auto id = session.tab_order.at(position);
    LEMMA_ASSERT(id.has_value());
    const auto* const tab = find_tab(session, *id);
    LEMMA_ASSERT(tab != nullptr);
    mix(static_cast<std::uint8_t>(position + 1U));
    mix(tab->id == session.active_tab ? 1U : 0U);
    const auto title = tab_title(session, *tab, runtimes);
    for (const char character : std::span(title).first(std::min(title.size(), std::size_t{16}))) {
      mix(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    mix(title.size() > 16 ? 1U : 0U);
  }
  const auto& prompt = session.attachment.rename_prompt;
  mix(static_cast<std::uint8_t>(prompt.kind));
  mix(static_cast<std::uint8_t>(prompt.feedback));
  mix(static_cast<std::uint8_t>(prompt.cursor));
  for (const char character : prompt.view()) {
    mix(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
  }
  mix_command_line_status(session.attachment.command_line, mix);
  const auto& copy_mode = session.attachment.copy_mode;
  mix(static_cast<std::uint8_t>(copy_mode.phase));
  mix(static_cast<std::uint8_t>(copy_mode.feedback));
  mix(static_cast<std::uint8_t>(copy_mode.prompt_search_direction));
  for (const char character : copy_mode.draft_query_view()) {
    mix(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
  }
  mix(session.attachment.status_message_visible ? 1U : 0U);
  for (const char character : visible_status_message(session)) {
    mix(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
  }
  mix(session.attachment.message_view.active ? 1U : 0U);
  mix(session.attachment.message_view.offset);
  for (const char character : status_input_context(session)) {
    mix(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
  }
  mix(static_cast<std::uint8_t>(copy_mode.search_direction));
  for (const char character : copy_mode.query_view()) {
    mix(static_cast<std::uint8_t>(character));
  }
  if (const auto* runtime = copy_mode_runtime(session, runtimes); runtime != nullptr) {
    if (const auto viewport = runtime->terminal.viewport_state(); viewport.has_value()) {
      for (const auto value : {viewport->offset, viewport->total_rows, viewport->visible_rows}) {
        signature = (signature ^ value) * prime;
      }
    }
  }
  return signature;
}

[[nodiscard]] constexpr auto status_prompt_target(const RenamePromptKind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case RenamePromptKind::inactive:
    return "none";
  case RenamePromptKind::session:
    return "session";
  case RenamePromptKind::tab:
    return "tab";
  }
  return "none";
}

[[nodiscard]] constexpr auto status_prompt_feedback(const RenamePromptFeedback feedback) noexcept
    -> std::string_view {
  switch (feedback) {
  case RenamePromptFeedback::none:
    return "none";
  case RenamePromptFeedback::invalid:
    return "invalid";
  case RenamePromptFeedback::conflict:
    return "conflict";
  }
  return "none";
}

struct StatusPromptProjection final {
  std::string_view target{"none"};
  std::string_view feedback{"none"};
  std::string_view value;
  std::size_t cursor{0};
};

[[nodiscard]] auto collect_status_prompt(const SessionRecord& session,
                                         const std::string_view copy_search_prompt) noexcept
    -> StatusPromptProjection {
  const auto& command_line = session.attachment.command_line;
  if (command_line.active) {
    return {.target = "command",
            .feedback = "none",
            .value = command_line.view(),
            .cursor = command_line.cursor};
  }
  const auto message = visible_status_message(session);
  if (!message.empty()) {
    return {.target = "message", .feedback = "none", .value = {}, .cursor = 0};
  }
  const auto& copy_mode = session.attachment.copy_mode;
  if (copy_mode.phase == CopyModePhase::search_prompt) {
    const auto* const target = copy_mode.prompt_search_direction == CopySearchDirection::forward
                                   ? "search.forward"
                                   : "search.backward";
    return {.target = target,
            .feedback = "none",
            .value = copy_search_prompt,
            .cursor = copy_search_prompt.size()};
  }
  const auto& rename = session.attachment.rename_prompt;
  return {.target = status_prompt_target(rename.kind),
          .feedback = status_prompt_feedback(rename.feedback),
          .value = rename.view(),
          .cursor = rename.cursor};
}

// Encoded clipboard traffic shares one bounded publication buffer. OSC 52 parts cannot interleave
// with presentation; OSC 5522 yields only between complete records.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto encode_pending_clipboard_write(SessionRecord& session) noexcept
    -> std::optional<std::size_t> {
  if (session.attachment_runtime.clipboard_write.bytes == nullptr ||
      session.attachment_runtime.clipboard_write.size == 0) {
    return std::nullopt;
  }
  auto& pending = session.attachment_runtime.clipboard_write;
  if (pending.format != PendingClipboardWrite::Format::selection) {
    const auto remaining = std::span(pending.bytes.get(), pending.size).subspan(pending.offset);
    const auto capacity = session.attachment_runtime.frame.capacity();
    const auto count = pending.format == PendingClipboardWrite::Format::kitty
                           ? clipboard::Transaction::frame_prefix(remaining, capacity)
                           : std::min({remaining.size(), capacity, std::size_t{64} * 1024U});
    if (count == 0) {
      return std::nullopt;
    }
    std::memcpy(session.attachment_runtime.frame.writable().data(),
                std::span(pending.bytes.get(), pending.size).subspan(pending.offset, count).data(),
                count);
    return count;
  }
  constexpr std::string_view prefix = "\x1B]52;c;";
  constexpr std::string_view suffix = "\x1B\\";
  constexpr auto digits =
      std::to_array("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
  auto output = session.attachment_runtime.frame.writable();
  const auto encoded_size =
      prefix.size() + clipboard_base64_bytes(session.attachment_runtime.clipboard_write.size) +
      suffix.size();
  if (encoded_size > output.size()) {
    return std::nullopt;
  }
  std::size_t used = 0;
  std::memcpy(output.data(), prefix.data(), prefix.size());
  used += prefix.size();
  const auto input = std::span(session.attachment_runtime.clipboard_write.bytes.get(),
                               session.attachment_runtime.clipboard_write.size);
  for (std::size_t offset = 0; offset < input.size(); offset += 3U) {
    const auto remaining = input.size() - offset;
    const auto first = std::to_integer<std::uint32_t>(input.subspan(offset, 1).front());
    const auto second =
        remaining > 1U ? std::to_integer<std::uint32_t>(input.subspan(offset + 1U, 1).front()) : 0U;
    const auto third =
        remaining > 2U ? std::to_integer<std::uint32_t>(input.subspan(offset + 2U, 1).front()) : 0U;
    const auto value = (first << 16U) | (second << 8U) | third;
    const auto digit = [&digits](const std::uint32_t index) noexcept {
      return std::span(digits).subspan(index, 1).front();
    };
    const std::array encoded{
        digit((value >> 18U) & 0x3FU),
        digit((value >> 12U) & 0x3FU),
        remaining > 1U ? digit((value >> 6U) & 0x3FU) : '=',
        remaining > 2U ? digit(value & 0x3FU) : '=',
    };
    std::memcpy(output.subspan(used, encoded.size()).data(), encoded.data(), encoded.size());
    used += encoded.size();
  }
  std::memcpy(output.subspan(used, suffix.size()).data(), suffix.data(), suffix.size());
  used += suffix.size();
  LEMMA_ASSERT(used == encoded_size);
  return used;
}

[[nodiscard]] auto queue_pending_clipboard_write(SessionRecord& session,
                                                 const ClientFrameOutput::TimePoint now) noexcept
    -> bool {
  const auto encoded = encode_pending_clipboard_write(session);
  if (!encoded.has_value() || session.attachment_runtime.full_redraw_generation == 0 ||
      session.attachment_runtime.server_sequence == 0) {
    return false;
  }
  const auto messages = ClientFrameOutput::frame_message_count(*encoded);
  if (messages == 0 ||
      messages >
          std::numeric_limits<std::uint32_t>::max() - session.attachment_runtime.server_sequence ||
      !session.attachment_runtime.output.queue_frame(
          *encoded, session.attachment_runtime.server_sequence,
          session.attachment_runtime.full_redraw_generation, false, now)) {
    return false;
  }
  session.attachment_runtime.server_sequence += static_cast<std::uint32_t>(messages);
  auto& pending = session.attachment_runtime.clipboard_write;
  if (pending.format != PendingClipboardWrite::Format::selection) {
    pending.offset += *encoded;
    pending.interleave_frame = pending.format == PendingClipboardWrite::Format::kitty;
  }
  if (pending.format == PendingClipboardWrite::Format::selection ||
      pending.offset == pending.size) {
    if (pending.format != PendingClipboardWrite::Format::selection &&
        session.attachment_runtime.clipboard != nullptr) {
      session.attachment_runtime.clipboard->published();
    }
    pending.reset();
  }
  pending.redraw_after_write = true;
  return true;
}

// Child-controlled titles are sanitized before entering the outer-terminal stream. C0, DEL, UTF-8
// encoded C1 controls, and malformed UTF-8 bytes are dropped; the bounded result is truncated at a
// code point boundary, so no escape, BEL, or string terminator can end the OSC early.
class OuterTitle final {
public:
  void append(const std::string_view text) noexcept {
    std::size_t index = 0;
    while (!truncated_ && index < text.size()) {
      const auto length = sequence_length(text.substr(index));
      if (length == 0) {
        ++index;
        continue;
      }
      const auto sequence = text.substr(index, length);
      index += length;
      if (control(sequence)) {
        continue;
      }
      if (length > bytes_.size() - size_) {
        truncated_ = true;
        return;
      }
      std::ranges::copy(sequence, std::span(bytes_).subspan(size_).begin());
      size_ += length;
    }
  }

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {bytes_.data(), size_}; }

private:
  // Returns the length of one well-formed UTF-8 sequence at the start of text, or zero.
  [[nodiscard]] static auto sequence_length(const std::string_view text) noexcept -> std::size_t {
    const auto lead = static_cast<std::uint8_t>(text.front());
    std::size_t length = 0;
    if (lead < 0x80U) {
      return 1;
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
      length = 2;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      length = 3;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      length = 4;
    }
    if (length == 0 || length > text.size()) {
      return 0;
    }
    const auto continuation = text.substr(1, length - 1U);
    return std::ranges::all_of(continuation,
                               [](const char byte) noexcept {
                                 return (static_cast<std::uint8_t>(byte) & 0xC0U) == 0x80U;
                               })
               ? length
               : 0;
  }

  [[nodiscard]] static auto control(const std::string_view sequence) noexcept -> bool {
    const auto lead = static_cast<std::uint8_t>(sequence.front());
    return lead < 0x20U || lead == 0x7FU ||
           (lead == 0xC2U && static_cast<std::uint8_t>(sequence.at(1)) < 0xA0U);
  }

  std::array<char, limits::outer_title_bytes_max> bytes_{};
  std::size_t size_{0};
  bool truncated_{false};
};

// The active Tab's explicit name wins; otherwise the focused Pane's terminal title, then its
// process name, identifies what the user is looking at.
[[nodiscard]] auto outer_title_label(const SessionRecord& session,
                                     const PaneRuntimeStore& runtimes) noexcept
    -> std::string_view {
  const auto* const tab = active_tab(session);
  if (tab == nullptr) {
    return {};
  }
  if (!tab->title_override().empty()) {
    return tab->title_override();
  }
  const auto* const focused = find_pane(session, *tab, tab->focused_pane);
  const auto* const runtime =
      focused == nullptr ? nullptr : find_pane_runtime(runtimes, session, *tab, *focused);
  if (runtime == nullptr) {
    return {};
  }
  const auto title = runtime->terminal.title();
  return title.has_value() && !title->empty() ? *title : tab_title(session, *tab, runtimes);
}

[[nodiscard]] auto append_frame_text(const std::span<std::byte> output, std::size_t& used,
                                     const std::string_view text) noexcept -> bool {
  if (text.size() > output.size() - used) {
    return false;
  }
  std::ranges::copy(std::as_bytes(std::span(text.data(), text.size())),
                    output.subspan(used).begin());
  used += text.size();
  return true;
}

// Only title changes reach the outer terminal. Disabling presentation restores the title the client
// pushed on attach and pushes it again so detach still restores it. Frame capacity reserves
// limits::outer_title_frame_bytes_max; if composition nevertheless leaves no room, the title is
// presentation-only and retries on a later frame instead of failing the attachment.
void append_outer_title(SessionRecord& session, const PaneRuntimeStore& runtimes,
                        const std::span<std::byte> output, std::size_t& used) noexcept {
  auto& attachment = session.attachment_runtime;
  if (!reactor_outer_title()) {
    if (attachment.outer_title_presented &&
        append_frame_text(output, used, "\x1B[23;2t\x1B[22;2t")) {
      attachment.outer_title_presented = false;
    }
    return;
  }
  OuterTitle title;
  title.append(session.session_name());
  title.append(": ");
  title.append(outer_title_label(session, runtimes));
  const auto presented =
      std::string_view(attachment.outer_title.data(), attachment.outer_title_size);
  if (attachment.outer_title_presented && presented == title.view()) {
    return;
  }
  constexpr std::string_view begin = "\x1B]2;";
  constexpr std::string_view end = "\x1B\\";
  static_assert(begin.size() + limits::outer_title_bytes_max + end.size() ==
                limits::outer_title_frame_bytes_max);
  if (begin.size() + title.view().size() + end.size() > output.size() - used) {
    return;
  }
  static_cast<void>(append_frame_text(output, used, begin) &&
                    append_frame_text(output, used, title.view()) &&
                    append_frame_text(output, used, end));
  std::ranges::copy(title.view(), attachment.outer_title.begin());
  attachment.outer_title_size = title.view().size();
  attachment.outer_title_presented = true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto compose_session_frame(SessionRecord& session, PaneRuntimeStore& runtimes,
                                         extension::Runtime& extensions, const bool force_full,
                                         const ClientFrameOutput::TimePoint now) noexcept -> bool {
  auto& clipboard_write = session.attachment_runtime.clipboard_write;
  if (clipboard_write.bytes != nullptr && !clipboard_write.interleave_frame) {
    return queue_pending_clipboard_write(session, now);
  }
  clipboard_write.interleave_frame = false;
  std::array<render::PaneSurface, panes_per_tab_max> surface_storage{};
  std::array<render::GridSurface, limits::extension_surfaces_hard_max> grid_storage{};
  MessageViewStorage message_storage;
  session.attachment_runtime.status_signature = current_status_signature(session, runtimes);
  session.attachment_runtime.status_valid = true;
  const auto surfaces = collect_surfaces(session, runtimes, surface_storage);
  const auto message_view = collect_message_view(session, message_storage);
  const auto grids = extensions.collect_surfaces(
      session.attachment.id,
      {.columns = session.attachment.columns, .rows = pane_rows(session.attachment.rows)},
      grid_storage);
  if (std::ranges::any_of(grids, &render::GridSurface::focused)) {
    for (auto& pane : std::span(surface_storage).first(surfaces.size())) {
      pane.focused = false;
      pane.cursor_override = false;
    }
  }
  std::uint64_t trace_correlation = 0;
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  trace_correlation = session.attachment_runtime.frame_trace_correlation;
  diagnostic::set_latency_trace_correlation(trace_correlation);
#endif
  diagnostic::record_latency_trace(diagnostic::LatencyTraceStage::frame_composition_started,
                                   static_cast<std::uint32_t>(session.attachment_runtime.client),
                                   surfaces.size());
  const auto rendered = render::compose_retained_scene(
      {.panes = surfaces, .grids = grids},
      {.columns = session.attachment.columns, .rows = session.attachment.rows},
      session.attachment_runtime.frame, force_full, {}, session.attachment_runtime.outer_modes,
      message_view, &session.attachment_runtime.graphics);
  diagnostic::record_latency_trace(diagnostic::LatencyTraceStage::frame_composition_finished,
                                   static_cast<std::uint32_t>(session.attachment_runtime.client),
                                   rendered.has_value() ? rendered->bytes : 0);
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  diagnostic::set_latency_trace_correlation(0);
  session.attachment_runtime.frame_trace_correlation = 0;
#endif
  if (!rendered.has_value() || session.attachment_runtime.server_sequence == 0) {
    return false;
  }
  auto frame_bytes = rendered->bytes;
  if (session.attachment_runtime.bell_pending) {
    auto output = session.attachment_runtime.frame.writable();
    if (frame_bytes >= output.size()) {
      return false;
    }
    output.subspan(frame_bytes, 1).front() = std::byte{0x07};
    ++frame_bytes;
  }
  append_outer_title(session, runtimes, session.attachment_runtime.frame.writable(), frame_bytes);
  const auto frame_messages = ClientFrameOutput::frame_message_count(frame_bytes);
  if (frame_messages == 0 || frame_messages > std::numeric_limits<std::uint32_t>::max() -
                                                  session.attachment_runtime.server_sequence) {
    return false;
  }
  auto generation = session.attachment_runtime.full_redraw_generation;
  if (rendered->full) {
    if (generation == std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    ++generation;
  }
  if (generation == 0 || !session.attachment_runtime.output.queue_frame(
                             frame_bytes, session.attachment_runtime.server_sequence, generation,
                             rendered->full, now, trace_correlation)) {
    return false;
  }
  session.attachment_runtime.server_sequence += static_cast<std::uint32_t>(frame_messages);
  session.attachment_runtime.full_redraw_generation = generation;
  session.attachment_runtime.outer_modes = rendered->outer_modes;
  session.attachment_runtime.bell_pending = false;
  if (rendered->full) {
    release_unpresented_render_caches(session, runtimes);
  }
  return true;
}

template <typename Id>
[[nodiscard]] auto append_id(ConnectionOutput& output, const Id id) noexcept -> bool {
  return id.is_valid() && output.append_number(id.slot()) && output.append_text(":") &&
         output.append_number(id.generation());
}

[[nodiscard]] auto append_listing(ConnectionOutput& output, const SessionRecord& session,
                                  const PaneRuntimeStore& runtimes) noexcept -> bool {
  const auto* const tab = active_tab(session);
  if (tab == nullptr) {
    return false;
  }
  const auto* const focused = find_pane(session, *tab, tab->focused_pane);
  LEMMA_ASSERT(focused != nullptr);
  const auto* const runtime = find_pane_runtime(runtimes, session, *tab, *focused);
  LEMMA_ASSERT(runtime != nullptr);
  const auto title_value = tab_title(session, *tab, runtimes);
  return output.append_text("lemma session \"") && output.append_title(session.session_name()) &&
         output.append_text("\": ") && output.append_number(tab_count(session)) &&
         output.append_text(" tab(s), ") && output.append_number(pane_count(session)) &&
         output.append_text(" pane(s), focused pid ") &&
         output.append_number(runtime->child > 0 ? static_cast<std::uint64_t>(runtime->child)
                                                 : 0U) &&
         output.append_text(session.attachment_runtime.client >= 0 ? ", attached, "
                                                                   : ", detached, ") &&
         output.append_number(session.attachment.columns) && output.append_text("x") &&
         output.append_number(session.attachment.rows) && output.append_text(", title \"") &&
         output.append_title(title_value) && output.append_text("\", ids session=") &&
         append_id(output, session.id) && output.append_text(" tab=") &&
         append_id(output, tab->id) && output.append_text(" pane=") &&
         append_id(output, focused->id) &&
         (session.attachment_runtime.connection_id.is_valid()
              ? output.append_text(" client=") &&
                    append_id(output, session.attachment_runtime.connection_id)
              : output.append_text(" client=detached")) &&
         output.append_text("\n");
}

[[nodiscard]] auto append_tab_listings(ConnectionOutput& output, const SessionRecord& session,
                                       const PaneRuntimeStore& runtimes) noexcept -> bool {
  for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
    const auto id = session.tab_order.at(position);
    LEMMA_ASSERT(id.has_value());
    const auto* const tab_value = find_tab(session, *id);
    LEMMA_ASSERT(tab_value != nullptr);
    const auto& tab = *tab_value;
    const auto title_value = tab_title(session, tab, runtimes);
    if (!output.append_text("lemma tab ") || !output.append_number(position + 1U) ||
        !output.append_text(": ") || !output.append_number(pane_count(tab)) ||
        !output.append_text(" pane(s), ") ||
        !output.append_text(tab.id == session.active_tab ? "active, title \""
                                                         : "inactive, title \"") ||
        !output.append_title(title_value) || !output.append_text("\", id=") ||
        !append_id(output, tab.id) || !output.append_text(", focused-pane=") ||
        !append_id(output, tab.focused_pane) || !output.append_text("\n")) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto append_process_state(ConnectionOutput& output, const Pane& pane,
                                        const PaneRuntime& runtime) noexcept -> bool {
  if (!pane.process_exit.has_value()) {
    return runtime.child > 0 ? output.append_text("running, pid ") &&
                                   output.append_number(static_cast<std::uint64_t>(runtime.child))
                             : output.append_text("exiting");
  }
  switch (pane.process_exit->kind) {
  case ProcessExitKind::unknown:
    return output.append_text("exited, status unknown");
  case ProcessExitKind::exited:
    return output.append_text("exited, code ") && output.append_number(pane.process_exit->value);
  case ProcessExitKind::signaled:
    return output.append_text("exited, signal ") && output.append_number(pane.process_exit->value);
  }
  return false;
}

// Listing traverses the bounded Session -> Tab -> Pane hierarchy once.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto append_pane_listings(ConnectionOutput& output, const SessionRecord& session,
                                        const PaneRuntimeStore& runtimes) noexcept -> bool {
  for (std::size_t tab_position = 0; tab_position < session.tab_order.size(); ++tab_position) {
    const auto tab_id = session.tab_order.at(tab_position);
    LEMMA_ASSERT(tab_id.has_value());
    const auto* const tab = find_tab(session, *tab_id);
    LEMMA_ASSERT(tab != nullptr);
    for (const auto& pane_slot : session.panes) {
      if (pane_slot.pane == nullptr || pane_slot.pane->tab != tab->id) {
        continue;
      }
      const auto& pane = *pane_slot.pane;
      const auto* const runtime = find_pane_runtime(runtimes, session, *tab, pane);
      LEMMA_ASSERT(runtime != nullptr);
      if (!output.append_text("lemma pane ") || !append_id(output, pane.id) ||
          !output.append_text(": tab ") || !output.append_number(tab_position + 1U) ||
          !output.append_text(pane.id == tab->focused_pane ? ", focused, " : ", unfocused, ") ||
          !append_process_state(output, pane, *runtime) || !output.append_text(", ") ||
          !output.append_number(pane.rectangle.columns) || !output.append_text("x") ||
          !output.append_number(pane.rectangle.rows) || !output.append_text(", tab-id=") ||
          !append_id(output, tab->id) || !output.append_text("\n")) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] auto append_json_id(ConnectionOutput& output, const auto id) noexcept -> bool {
  return output.append_text("\"") && append_id(output, id) && output.append_text("\"");
}

[[nodiscard]] auto append_structured_session(ConnectionOutput& output,
                                             const SessionRecord& session) noexcept -> bool {
  const auto* const tab = active_tab(session);
  if (tab == nullptr) {
    return false;
  }
  return output.append_text(R"({"name":")") && output.append_text(session.session_name()) &&
         output.append_text(R"(","id":)") && append_json_id(output, session.id) &&
         output.append_text(session.attachment_runtime.client >= 0 ? R"(,"attached":true)"
                                                                   : R"(,"attached":false)") &&
         output.append_text(R"(,"revision":)") &&
         output.append_number(session.mutation_generation) && output.append_text(R"(,"tabs":)") &&
         output.append_number(tab_count(session)) && output.append_text(R"(,"panes":)") &&
         output.append_number(pane_count(session)) &&
         output.append_text(R"(,"attachments":{"connected":)") &&
         output.append_number(session.attachment_runtime.client >= 0 ? 1U : 0U) &&
         output.append_text(R"(,"controllers":)") &&
         output.append_number(session.attachment_runtime.client >= 0 ? 1U : 0U) &&
         output.append_text(R"(,"viewers":0})") && output.append_text(R"(,"columns":)") &&
         output.append_number(session.attachment.columns) && output.append_text(R"(,"rows":)") &&
         output.append_number(session.attachment.rows) && output.append_text(R"(,"active_tab":)") &&
         append_json_id(output, tab->id) && output.append_text(R"(,"focused_pane":)") &&
         append_json_id(output, tab->focused_pane) && output.append_text("}");
}

// Closed JSON escaping is deliberately local to the fixed-capacity ConnectionOutput projection.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto append_connection_json_string(ConnectionOutput& output,
                                                 const std::string_view value) noexcept -> bool {
  if (!output.append_text("\"")) {
    return false;
  }
  for (const char character : value) {
    if (character == '"') {
      if (!output.append_text("\\\"")) {
        return false;
      }
    } else if (character == '\\') {
      if (!output.append_text("\\\\")) {
        return false;
      }
    } else if (static_cast<unsigned char>(character) < 0x20U) {
      if (!output.append_text("?")) {
        return false;
      }
    } else if (!output.append_text(std::string_view(&character, 1))) {
      return false;
    }
  }
  return output.append_text("\"");
}

[[nodiscard]] auto append_structured_tabs(ConnectionOutput& output, const SessionRecord& session,
                                          const PaneRuntimeStore& runtimes) noexcept -> bool {
  if (!output.append_text("[")) {
    return false;
  }
  for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
    const auto id = session.tab_order.at(position);
    LEMMA_ASSERT(id.has_value());
    const auto* const tab = find_tab(session, *id);
    LEMMA_ASSERT(tab != nullptr);
    const auto title = tab_title(session, *tab, runtimes);
    if ((position > 0 && !output.append_text(",")) || !output.append_text("{\"position\":") ||
        !output.append_number(position + 1U) || !output.append_text(",\"id\":") ||
        !append_json_id(output, tab->id) ||
        !output.append_text(tab->id == session.active_tab ? ",\"active\":true"
                                                          : ",\"active\":false") ||
        !output.append_text(",\"title\":") || !append_connection_json_string(output, title) ||
        !output.append_text(tab->zoomed ? ",\"zoomed\":true" : ",\"zoomed\":false") ||
        !output.append_text(",\"panes\":") || !output.append_number(pane_count(*tab)) ||
        !output.append_text(",\"focused_pane\":") || !append_json_id(output, tab->focused_pane) ||
        !output.append_text("}")) {
      return false;
    }
  }
  return output.append_text("]");
}

[[nodiscard]] auto append_structured_process(ConnectionOutput& output, const Pane& pane,
                                             const PaneRuntime& runtime) noexcept -> bool {
  const auto append_pid = [&output, &runtime] {
    return output.append_text(R"(,"pid":)") &&
           output.append_number(runtime.child > 0 ? static_cast<std::uint64_t>(runtime.child) : 0U);
  };
  if (!pane.process_exit.has_value()) {
    return output.append_text(runtime.child > 0 ? R"({"state":"running")"
                                                : R"({"state":"exiting")") &&
           append_pid() && output.append_text("}");
  }
  std::string_view state;
  switch (pane.process_exit->kind) {
  case ProcessExitKind::unknown:
    state = "exited_unknown";
    break;
  case ProcessExitKind::exited:
    state = "exited";
    break;
  case ProcessExitKind::signaled:
    state = "signaled";
    break;
  }
  return output.append_text(R"({"state":")") && output.append_text(state) &&
         output.append_text(R"(","value":)") && output.append_number(pane.process_exit->value) &&
         append_pid() && output.append_text("}");
}

[[nodiscard]] constexpr auto progress_state_name(const vt::ProgressState state) noexcept
    -> std::string_view {
  switch (state) {
  case vt::ProgressState::normal:
    return "normal";
  case vt::ProgressState::error:
    return "error";
  case vt::ProgressState::indeterminate:
    return "indeterminate";
  case vt::ProgressState::paused:
    return "paused";
  case vt::ProgressState::none:
    break;
  }
  return {};
}

[[nodiscard]] constexpr auto command_state_name(const vt::CommandState state) noexcept
    -> std::string_view {
  switch (state) {
  case vt::CommandState::prompt:
    return "prompt";
  case vt::CommandState::running:
    return "running";
  case vt::CommandState::finished:
    return "finished";
  case vt::CommandState::none:
    break;
  }
  return {};
}

[[nodiscard]] auto signal_text(ConnectionOutput& output, const std::string_view text) noexcept
    -> bool {
  return output.append_text(text);
}

[[nodiscard]] auto signal_text(std::string& output, const std::string_view text) -> bool {
  return append_public(output, text);
}

[[nodiscard]] auto signal_number(ConnectionOutput& output, const std::uint64_t value) noexcept
    -> bool {
  return output.append_number(value);
}

[[nodiscard]] auto signal_number(std::string& output, const std::uint64_t value) -> bool {
  return append_public_number(output, value);
}

template <typename Output>
[[nodiscard]] auto append_signal_progress(Output& output, const vt::TerminalSignals& signals)
    -> bool {
  const auto progress = progress_state_name(signals.progress);
  if (progress.empty()) {
    return signal_text(output, "null");
  }
  return signal_text(output, R"({"state":")") && signal_text(output, progress) &&
         signal_text(output, R"(","percent":)") &&
         (signals.progress_percent.has_value() ? signal_number(output, *signals.progress_percent)
                                               : signal_text(output, "null")) &&
         signal_text(output, "}");
}

template <typename Output>
[[nodiscard]] auto append_signal_exit_code(Output& output, const std::optional<std::int32_t> exit)
    -> bool {
  if (!exit.has_value()) {
    return signal_text(output, "null");
  }
  const auto code = static_cast<std::int64_t>(*exit);
  return (code >= 0 || signal_text(output, "-")) &&
         signal_number(output, static_cast<std::uint64_t>(code < 0 ? -code : code));
}

template <typename Output>
[[nodiscard]] auto append_signal_command(Output& output, const vt::TerminalSignals& signals)
    -> bool {
  const auto command = command_state_name(signals.command);
  if (command.empty()) {
    return signal_text(output, "null");
  }
  return signal_text(output, R"({"state":")") && signal_text(output, command) &&
         signal_text(output, R"(","exit_code":)") &&
         append_signal_exit_code(output, signals.exit_code) && signal_text(output, "}");
}

// Every field except notification text; all values are numbers or fixed enum names, so the
// record also fits the fixed-capacity pane listing projection.
template <typename Output>
[[nodiscard]] auto append_signal_fields(Output& output, const PaneRuntime& runtime) -> bool {
  const auto& signals = runtime.terminal.signals();
  return signal_text(output, R"({"generation":)") && signal_number(output, runtime.signal_stamp) &&
         signal_text(output, R"(,"bells":)") && signal_number(output, signals.bells) &&
         signal_text(output, R"(,"notifications":)") &&
         signal_number(output, signals.notifications) && signal_text(output, R"(,"progress":)") &&
         append_signal_progress(output, signals) && signal_text(output, R"(,"commands":)") &&
         signal_number(output, signals.commands) && signal_text(output, R"(,"command":)") &&
         append_signal_command(output, signals) && signal_text(output, R"(,"title_changes":)") &&
         signal_number(output, signals.title_changes) &&
         signal_text(output, R"(,"cwd_changes":)") && signal_number(output, signals.cwd_changes);
}

// Structured pane queries traverse the bounded semantic hierarchy once and expose no title-based
// selectors or terminal-owned representation.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto append_structured_panes(ConnectionOutput& output, const SessionRecord& session,
                                           const PaneRuntimeStore& runtimes) noexcept -> bool {
  if (!output.append_text("[")) {
    return false;
  }
  std::size_t emitted = 0;
  for (std::size_t tab_position = 0; tab_position < session.tab_order.size(); ++tab_position) {
    const auto tab_id = session.tab_order.at(tab_position);
    LEMMA_ASSERT(tab_id.has_value());
    const auto* const tab = find_tab(session, *tab_id);
    LEMMA_ASSERT(tab != nullptr);
    for (const auto& pane_slot : session.panes) {
      if (pane_slot.pane == nullptr || pane_slot.pane->tab != tab->id) {
        continue;
      }
      const auto& pane = *pane_slot.pane;
      const auto* const runtime = find_pane_runtime(runtimes, session, *tab, pane);
      LEMMA_ASSERT(runtime != nullptr);
      if ((emitted > 0 && !output.append_text(",")) || !output.append_text("{\"id\":") ||
          !append_json_id(output, pane.id) || !output.append_text(",\"tab\":") ||
          !append_json_id(output, tab->id) || !output.append_text(",\"tab_position\":") ||
          !output.append_number(tab_position + 1U) ||
          !output.append_text(pane.id == tab->focused_pane ? ",\"focused\":true"
                                                           : ",\"focused\":false") ||
          !output.append_text(",\"column\":") || !output.append_number(pane.rectangle.column) ||
          !output.append_text(",\"row\":") || !output.append_number(pane.rectangle.row) ||
          !output.append_text(",\"columns\":") || !output.append_number(pane.rectangle.columns) ||
          !output.append_text(",\"rows\":") || !output.append_number(pane.rectangle.rows) ||
          !output.append_text(",\"process\":") ||
          !append_structured_process(output, pane, *runtime) ||
          !output.append_text(",\"terminal_generation\":") ||
          !output.append_number(runtime->observation_generation) ||
          !output.append_text(",\"observed_title\":") ||
          !append_connection_json_string(
              output, std::string_view(runtime->process_name.data(), runtime->process_name_size)) ||
          !output.append_text(",\"signals\":") || !append_signal_fields(output, *runtime) ||
          !output.append_text("}}")) {
        return false;
      }
      ++emitted;
    }
  }
  return output.append_text("]");
}

[[nodiscard]] auto append_structured_sessions(ConnectionOutput& output,
                                              const Sessions& sessions) noexcept -> bool {
  if (!output.append_text("[")) {
    return false;
  }
  std::size_t emitted = 0;
  for (const auto& session : sessions) {
    if (session == nullptr || !session->active) {
      continue;
    }
    if ((emitted > 0 && !output.append_text(",")) || !append_structured_session(output, *session)) {
      return false;
    }
    ++emitted;
  }
  return output.append_text("]");
}

[[nodiscard]] auto append_public(std::string& output, const std::string_view text) -> bool {
  if (text.size() > api::json_bytes_max - output.size()) {
    return false;
  }
  output.append(text);
  return true;
}

[[nodiscard]] auto append_public_number(std::string& output, const std::uint64_t value) -> bool {
  std::array<char, 32> encoded{};
  const auto result = std::to_chars(encoded.begin(), encoded.end(), value);
  return result.ec == std::errc{} &&
         append_public(output,
                       {encoded.data(), static_cast<std::size_t>(result.ptr - encoded.data())});
}

[[nodiscard]] constexpr auto public_status_name(const CommandStatus status) noexcept
    -> std::string_view {
  switch (status) {
  case CommandStatus::applied:
    return "applied";
  case CommandStatus::no_effect:
    return "no_effect";
  case CommandStatus::stale_target:
    return "stale";
  case CommandStatus::wrong_owner:
    return "wrong_owner";
  case CommandStatus::conflict:
    return "conflict";
  case CommandStatus::capacity:
    return "capacity";
  case CommandStatus::unavailable:
    return "unavailable";
  case CommandStatus::detach_requested:
  case CommandStatus::invalid_command:
  case CommandStatus::invalid_target:
  case CommandStatus::failed:
    return "failed";
  }
  return "failed";
}

[[nodiscard]] auto public_session(Sessions& sessions, const api::SessionSelector& selector) noexcept
    -> SessionRecord* {
  return selector.id.is_valid() ? sessions.get(selector.id) : find_session(sessions, selector.name);
}

[[nodiscard]] auto public_tab(SessionRecord& session, const api::TabSelector& selector) noexcept
    -> Tab* {
  if (selector.id.is_valid()) {
    return find_tab(session, selector.id);
  }
  return selector.position > 0 ? tab_at_position(session, selector.position - 1U) : nullptr;
}

[[nodiscard]] auto public_launch_command(const api::Command& request,
                                         std::vector<std::byte>& output) -> bool {
  std::size_t size = 0;
  for (const auto& argument : request.arguments) {
    if (argument.size() + 1U > protocol::command_bytes_max - size) {
      return false;
    }
    size += argument.size() + 1U;
  }
  try {
    output.resize(size);
  } catch (...) {
    return false;
  }
  std::size_t offset = 0;
  for (const auto& argument : request.arguments) {
    std::ranges::copy(std::as_bytes(std::span(argument.data(), argument.size())),
                      std::span(output).subspan(offset).begin());
    offset += argument.size();
    std::span(output).subspan(offset, 1).front() = std::byte{0};
    ++offset;
  }
  return valid_launch_command(output);
}

[[nodiscard]] auto public_environment(const api::Command& request, std::vector<std::byte>& output)
    -> bool {
  std::size_t size = 0;
  for (const auto& entry : request.environment) {
    if (entry.size() + 1U > protocol::environment_bytes_max - size) {
      return false;
    }
    size += entry.size() + 1U;
  }
  try {
    output.resize(size);
  } catch (...) {
    return false;
  }
  std::size_t offset = 0;
  for (const auto& entry : request.environment) {
    std::ranges::copy(std::as_bytes(std::span(entry.data(), entry.size())),
                      std::span(output).subspan(offset).begin());
    offset += entry.size();
    std::span(output).subspan(offset, 1).front() = std::byte{0};
    ++offset;
  }
  return valid_environment(output);
}

[[nodiscard]] auto copy_connection_json(const ConnectionOutput& source, std::string& output)
    -> bool {
  const auto bytes = source.readable();
  if (bytes.size() > api::json_bytes_max) {
    return false;
  }
  try {
    // Byte payloads and character storage have the same object representation.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    output.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  } catch (...) {
    return false;
  }
  return true;
}

[[nodiscard]] auto format_public_visible(vt::Terminal& terminal, const vt::ScreenFormat format,
                                         const api::CaptureWrap wrap, const std::uint16_t lines,
                                         const std::span<std::byte> output) noexcept
    -> PublicCaptureFormatting {
  const bool unwrap = wrap == api::CaptureWrap::logical;
  const auto requested_rows = static_cast<std::size_t>(lines == 0 ? terminal.size().rows : lines);
  auto formatted = terminal.format_visible_tail(format, requested_rows, output, unwrap);
  if (formatted.has_value() || formatted.error() != vt::Error::out_of_space) {
    return {.result = formatted, .truncated = false};
  }
  auto rows = requested_rows;
  while (rows > 1U) {
    rows /= 2U;
    formatted = terminal.format_visible_tail(format, rows, output, unwrap);
    if (formatted.has_value() || formatted.error() != vt::Error::out_of_space) {
      return {.result = formatted, .truncated = true};
    }
  }
  return {.result = formatted, .truncated = true};
}

[[nodiscard]] auto append_json_id_field(std::string& output, const std::string_view field,
                                        const auto id) -> bool {
  return append_public(output, "\"") && append_public(output, field) &&
         append_public(output, "\":") && append_public_id(output, id);
}

[[nodiscard]] auto append_launch_argv(std::string& output, const std::span<const std::byte> command)
    -> bool {
  if (!append_public(output, "[")) {
    return false;
  }
  std::size_t offset = 0;
  std::size_t argument = 0;
  while (offset < command.size()) {
    const auto remaining = command.subspan(offset);
    const auto terminator = std::ranges::find(remaining, std::byte{0});
    if (terminator == remaining.end()) {
      return false;
    }
    const auto size = static_cast<std::size_t>(std::distance(remaining.begin(), terminator));
    if (argument++ > 0 && !append_public(output, ",")) {
      return false;
    }
    // Command bytes were validated as non-NUL argv at creation.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view value(reinterpret_cast<const char*>(remaining.data()), size);
    if (!api::append_json_string(output, value)) {
      return false;
    }
    offset += size + 1U;
  }
  return append_public(output, "]");
}

[[nodiscard]] auto append_layout_node(std::string& output, const LayoutSnapshot& layout,
                                      const std::size_t index, const std::size_t depth = 0)
    -> bool {
  if (index >= layout.size || depth >= limits::layout_depth_hard_max) {
    return false;
  }
  const auto& node = std::span(layout.nodes).subspan(index, 1).front();
  if (node.leaf) {
    return append_public(output, R"({"pane":)") && append_public_id(output, node.pane) &&
           append_public(output, "}");
  }
  const auto first = static_cast<std::size_t>(node.first);
  const auto second = static_cast<std::size_t>(node.second);
  return node.first >= 0 && node.second >= 0 && append_public(output, R"({"split":)") &&
         api::append_json_string(output, node.axis == SplitAxis::left_right ? "left_right"
                                                                            : "top_bottom") &&
         append_public(output, R"(,"ratio":)") && append_public_number(output, node.ratio) &&
         append_public(output, R"(,"first":)") &&
         append_layout_node(output, layout, first, depth + 1U) &&
         append_public(output, R"(,"second":)") &&
         append_layout_node(output, layout, second, depth + 1U) && append_public(output, "}");
}

[[nodiscard]] auto daemon_inspection(const Sessions& sessions, const PaneRuntimeStore& runtimes)
    -> std::string {
  std::size_t active_sessions = 0;
  std::size_t attached_sessions = 0;
  for (const auto& session : sessions) {
    if (session == nullptr || !session->active) {
      continue;
    }
    ++active_sessions;
    attached_sessions += session->attachment_runtime.client >= 0 ? 1U : 0U;
  }
  std::string output = R"({"version":)";
  if (!api::append_json_string(output, lemma::version) || !append_public(output, R"(,"api":)") ||
      !api::append_json_string(output, api::proc_schema) ||
      !append_public(output, R"(,"resources":{"sessions":{"used":)") ||
      !append_public_number(output, active_sessions) || !append_public(output, R"(,"limit":)") ||
      !append_public_number(output, limits::sessions_hard_max) ||
      !append_public(output, R"(},"panes":{"used":)") ||
      !append_public_number(output, runtimes.size()) || !append_public(output, R"(,"limit":)") ||
      !append_public_number(output, limits::panes_hard_max) ||
      !append_public(output, R"(},"scrollback_bytes":{"reserved":)") ||
      !append_public_number(output, runtimes.scrollback_bytes_reserved()) ||
      !append_public(output, R"(,"limit":)") ||
      !append_public_number(output, limits::terminal_scrollback_bytes_aggregate_max) ||
      !append_public(output, R"(}},"attachments":{"connected":)") ||
      !append_public_number(output, attached_sessions) || !append_public(output, "}}")) {
    return {};
  }
  return output;
}

[[nodiscard]] auto session_inspection(const SessionRecord& session) -> std::string {
  std::string output = "{";
  const auto environment_entries =
      static_cast<std::size_t>(std::ranges::count(session.launch_environment(), std::byte{0}));
  if (!append_json_id_field(output, "id", session.id) || !append_public(output, R"(,"name":)") ||
      !api::append_json_string(output, session.session_name()) ||
      !append_public(output, R"(,"revision":)") ||
      !append_public_number(output, session.mutation_generation) ||
      !append_public(output, R"(,"launch":{"cwd":)") ||
      !api::append_json_string(output, session.cwd()) ||
      !append_public(output, R"(,"environment":{"mode":)") ||
      !api::append_json_string(output, session.environment_mode == LaunchEnvironmentMode::replace
                                           ? "captured"
                                           : "inherited") ||
      !append_public(output, R"(,"entries":)") ||
      !append_public_number(output, environment_entries) || !append_public(output, "}}") ||
      !append_public(output, R"(,"active_tab":)") ||
      !append_public_id(output, session.active_tab) ||
      !append_public(output, R"(,"previous_tab":)") ||
      !append_public_id(output, session.previous_tab) || !append_public(output, R"(,"tabs":)") ||
      !append_public_number(output, tab_count(session)) || !append_public(output, R"(,"panes":)") ||
      !append_public_number(output, pane_count(session)) ||
      !append_public(output, R"(,"theme_bound":)") ||
      !append_public(output, session.theme_bound ? "true" : "false") ||
      !append_public(output, R"(,"geometry":{"columns":)") ||
      !append_public_number(output, session.attachment.columns) ||
      !append_public(output, R"(,"rows":)") ||
      !append_public_number(output, session.attachment.rows) ||
      !append_public(output, R"(},"attachments":{"connected":)") ||
      !append_public_number(output, session.attachment_runtime.client >= 0 ? 1U : 0U) ||
      !append_public(output, R"(,"controllers":)") ||
      !append_public_number(output, session.attachment_runtime.client >= 0 ? 1U : 0U) ||
      !append_public(output, R"(,"viewers":0}})")) {
    return {};
  }
  return output;
}

[[nodiscard]] auto tab_inspection(const SessionRecord& session, const Tab& tab,
                                  const PaneRuntimeStore& runtimes) -> std::string {
  const auto position = session.tab_order.position_of(tab.id);
  const auto layout = tab.layout.snapshot();
  if (!position.has_value() || !layout.has_value()) {
    return {};
  }
  std::string output = "{";
  const auto title = tab_title(session, tab, runtimes);
  if (!append_json_id_field(output, "id", tab.id) || !append_public(output, R"(,"position":)") ||
      !append_public_number(output, *position + 1U) || !append_public(output, R"(,"title":)") ||
      !api::append_json_string(output, title) || !append_public(output, R"(,"active":)") ||
      !append_public(output, tab.id == session.active_tab ? "true" : "false") ||
      !append_public(output, R"(,"focused_pane":)") ||
      !append_public_id(output, tab.focused_pane) ||
      !append_public(output, R"(,"previous_pane":)") ||
      !append_public_id(output, tab.previous_pane) || !append_public(output, R"(,"zoomed":)") ||
      !append_public(output, tab.zoomed ? "true" : "false") ||
      !append_public(output, R"(,"layout_suspended":)") ||
      !append_public(output, tab.layout_suspended ? "true" : "false") ||
      !append_public(output, R"(,"geometry":{"columns":)") ||
      !append_public_number(output, tab.layout_columns) || !append_public(output, R"(,"rows":)") ||
      !append_public_number(output, tab.layout_rows) || !append_public(output, R"(},"layout":)") ||
      !append_layout_node(output, *layout, 0) || !append_public(output, "}")) {
    return {};
  }
  return output;
}

[[nodiscard]] constexpr auto process_exit_name(const ProcessExitKind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case ProcessExitKind::unknown:
    return "exited_unknown";
  case ProcessExitKind::exited:
    return "exited";
  case ProcessExitKind::signaled:
    return "signaled";
  }
  return "exited_unknown";
}

// Process lifecycle variants share the launch and observed metadata suffix.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto append_process_inspection(std::string& output, const Pane& pane,
                                             const PaneRuntime& runtime) -> bool {
  if (!append_public(output, R"({"state":)")) {
    return false;
  }
  if (!pane.process_exit.has_value()) {
    if (!api::append_json_string(output, runtime.child > 0 ? "running" : "exiting")) {
      return false;
    }
  } else {
    if (!api::append_json_string(output, process_exit_name(pane.process_exit->kind))) {
      return false;
    }
    if (pane.process_exit->kind != ProcessExitKind::unknown &&
        (!append_public(output, pane.process_exit->kind == ProcessExitKind::signaled
                                    ? R"(,"signal":)"
                                    : R"(,"code":)") ||
         !append_public_number(output, pane.process_exit->value))) {
      return false;
    }
  }
  return append_public(output, R"(,"pid":)") &&
         append_public_number(output,
                              runtime.child > 0 ? static_cast<std::uint64_t>(runtime.child) : 0U) &&
         append_public(output, R"(,"launch":{"cwd":)") &&
         api::append_json_string(output, pane.launch_working_directory()) &&
         append_public(output, R"(,"argv":)") &&
         append_launch_argv(output, pane.launch_command()) &&
         append_public(output, R"(,"exit_policy":)") &&
         api::append_json_string(output,
                                 pane.exit_policy == PaneExitPolicy::hold ? "hold" : "close") &&
         append_public(output, R"(},"observed_title":)") &&
         api::append_json_string(
             output, std::string_view(runtime.process_name.data(), runtime.process_name_size)) &&
         append_public(output, "}");
}

[[nodiscard]] auto append_public_signals(std::string& output, const PaneRuntime& runtime) -> bool {
  const auto& signals = runtime.terminal.signals();
  if (!append_signal_fields(output, runtime) || !append_public(output, R"(,"notification":)")) {
    return false;
  }
  if (signals.notifications == 0) {
    return append_public(output, "null}");
  }
  return append_public(output, R"({"title":)") &&
         api::append_json_string(output, signals.notification_title()) &&
         append_public(output, R"(,"body":)") &&
         api::append_json_string(output, signals.notification_body()) &&
         append_public(output, R"(,"truncated":)") &&
         append_public(output, signals.notification_truncated ? "true}}" : "false}}");
}

[[nodiscard]] auto pane_inspection(const Tab& tab, const Pane& pane, const PaneRuntime& runtime)
    -> std::string {
  const auto terminal = runtime.terminal.inspection();
  const auto title = runtime.terminal.title();
  const auto pwd = runtime.terminal.pwd();
  if (!terminal.has_value() || !title.has_value() || !pwd.has_value()) {
    return {};
  }
  std::string output = R"({"pane":{)";
  if (!append_json_id_field(output, "id", pane.id) || !append_public(output, R"(,"tab":)") ||
      !append_public_id(output, tab.id) || !append_public(output, R"(,"rectangle":{"column":)") ||
      !append_public_number(output, pane.rectangle.column) ||
      !append_public(output, R"(,"row":)") || !append_public_number(output, pane.rectangle.row) ||
      !append_public(output, R"(,"columns":)") ||
      !append_public_number(output, pane.rectangle.columns) ||
      !append_public(output, R"(,"rows":)") || !append_public_number(output, pane.rectangle.rows) ||
      !append_public(output, R"(}},"process":)") ||
      !append_process_inspection(output, pane, runtime) ||
      !append_public(output, R"(,"terminal":{"generation":)") ||
      !append_public_number(output, runtime.observation_generation) ||
      !append_public(output, R"(,"size":{"columns":)") ||
      !append_public_number(output, runtime.terminal.size().columns) ||
      !append_public(output, R"(,"rows":)") ||
      !append_public_number(output, runtime.terminal.size().rows) ||
      !append_public(output, R"(},"screen":)") ||
      !api::append_json_string(
          output, terminal->active_screen == vt::ActiveScreen::primary ? "primary" : "alternate") ||
      !append_public(output, R"(,"viewport":{"at_bottom":)") ||
      !append_public(output, terminal->viewport.follows_output ? "true" : "false") ||
      !append_public(output, R"(,"offset_rows":)") ||
      !append_public_number(output, terminal->viewport.offset) ||
      !append_public(output, R"(},"history":{"retained_rows":)") ||
      !append_public_number(output, terminal->scrollback_rows) ||
      !append_public(output, R"(},"cursor":{"column":)") ||
      !append_public_number(output, terminal->cursor_column) ||
      !append_public(output, R"(,"row":)") || !append_public_number(output, terminal->cursor_row) ||
      !append_public(output, R"(,"visible":)") ||
      !append_public(output, terminal->cursor_visible ? "true" : "false") ||
      !append_public(output, R"(},"title":)") || !api::append_json_string(output, *title) ||
      !append_public(output, R"(,"pwd":{"value":)") || !api::append_json_string(output, *pwd) ||
      !append_public(output, R"(,"source":)") ||
      !api::append_json_string(output, pwd->empty() ? "unknown" : "osc7") ||
      !append_public(output, R"(},"cursor_at_prompt":)") ||
      !append_public(output, terminal->cursor_at_prompt ? "true" : "false") ||
      !append_public(output, R"(,"input_accepted":)") ||
      !append_public(output, runtime.accepts_input() ? "true" : "false") ||
      !append_public(output, R"(,"health":)") ||
      !api::append_json_string(output, runtime.terminal.integrity_failed() ? "failed" : "ok") ||
      !append_public(output, R"(},"signals":)") || !append_public_signals(output, runtime) ||
      !append_public(output, "}")) {
    return {};
  }
  return output;
}

// Every admitted Command reaches the same semantic transitions and Runtime helpers as native input.
// The JSON envelope owns no mux policy.

[[nodiscard]] auto append_wait_process(std::string& output, const ProcessExit process) -> bool {
  switch (process.kind) {
  case ProcessExitKind::unknown:
    return append_public(output, R"({"state":"exited_unknown"})");
  case ProcessExitKind::exited:
    return append_public(output, R"({"state":"exited","code":)") &&
           append_public_number(output, process.value) && append_public(output, "}");
  case ProcessExitKind::signaled:
    return append_public(output, R"({"state":"signaled","signal":)") &&
           append_public_number(output, process.value) && append_public(output, "}");
  }
  return false;
}

[[nodiscard]] auto encode_public_error(const api::CommandDecodeError error,
                                       const std::optional<std::size_t> byte) -> std::string {
  std::string output;
  try {
    output = R"({"schema":"lemma.proc-result/v1","ok":false,"error":{"reason":)";
    if (!api::append_json_string(output, error.reason)) {
      return {};
    }
    if (!error.field.empty() &&
        (!append_public(output, ",\"field\":") || !api::append_json_string(output, error.field))) {
      return {};
    }
    if (byte.has_value() &&
        (!append_public(output, ",\"byte\":") || !append_public_number(output, *byte))) {
      return {};
    }
    return append_public(output, "},\"results\":[]}\n") ? output : std::string{};
  } catch (...) {
    return {};
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto encode_command_result(const api::Command& request,
                                         const PublicCommandExecution& result,
                                         const bool target_resolved) -> std::string {
  std::string output;
  try {
    output = R"({"schema":"lemma.command-result/v1","command":)";
    if (!api::append_json_string(output, api::command_name(request.kind)) ||
        !append_public(output, ",\"status\":") ||
        !api::append_json_string(output, public_status_name(result.status))) {
      return {};
    }
    if (request.kind == api::CommandKind::pane_wait &&
        (!append_public(output, R"(,"condition":)") ||
         !api::append_json_string(output, api::wait_condition_name(request.wait_condition)))) {
      return {};
    }
    if (result.session.is_valid()) {
      if (!append_public(output, R"(,"session":{"id":)") ||
          !append_public_id(output, result.session) || !append_public(output, ",\"name\":") ||
          !api::append_json_string(output, result.session_name) ||
          (result.session_revision > 0 &&
           (!append_public(output, R"(,"revision":)") ||
            !append_public_number(output, result.session_revision))) ||
          !append_public(output, "}")) {
        return {};
      }
    }
    if (result.tab.is_valid() &&
        (!append_public(output, ",\"tab\":") || !append_public_id(output, result.tab))) {
      return {};
    }
    auto result_pane = result.pane;
    if (!result_pane.is_valid() && target_resolved && request.kind == api::CommandKind::pane_wait) {
      result_pane = request.pane.id;
    }
    if (result_pane.is_valid() &&
        (!append_public(output, ",\"pane\":") || !append_public_id(output, result_pane))) {
      return {};
    }
    if (result.terminal_generation > 0 &&
        (!append_public(output, R"(,"terminal_generation":)") ||
         !append_public_number(output, result.terminal_generation))) {
      return {};
    }
    if (result.process.has_value() && (!append_public(output, R"(,"process":)") ||
                                       !append_wait_process(output, *result.process))) {
      return {};
    }
    if (!result.value_field.empty() &&
        (!append_public(output, ",\"") || !append_public(output, result.value_field) ||
         !append_public(output, "\":") || !append_public(output, result.value_json))) {
      return {};
    }
    if (result.has_text && !result.has_capture &&
        (!append_public(output, ",\"text\":") || !api::append_json_string(output, result.text))) {
      return {};
    }
    if (!result.error_reason.empty()) {
      if (!append_public(output, R"(,"error":{"reason":)") ||
          !api::append_json_string(output, result.error_reason) ||
          !append_public(output, R"(,"retryable":)") ||
          !append_public(output, result.retryable ? "true" : "false")) {
        return {};
      }
      if (request.expected_session_revision.has_value() &&
          (!append_public(output, R"(,"expected":)") ||
           !append_public_number(output, *request.expected_session_revision) ||
           !append_public(output, R"(,"current":)") ||
           !append_public_number(output, result.session_revision))) {
        return {};
      }
      if (!append_public(output, "}")) {
        return {};
      }
    }
    if (result.has_capture &&
        (!append_public(output, R"(,"capture":)") ||
         !api::append_capture(output, result.capture_source, result.capture_format,
                              result.capture_wrap, result.terminal_generation,
                              result.capture_truncated, result.text))) {
      return {};
    }
    return append_public(output, "}\n") ? output : std::string{};
  } catch (...) {
    return {};
  }
}

[[nodiscard]] auto semantic_hash(const Sessions& sessions,
                                 const std::optional<api::SessionSelector>& filter) noexcept
    -> std::uint64_t {
  std::uint64_t value = 1469598103934665603ULL;
  const auto mix = [&value](const std::uint64_t part) {
    value ^= part;
    value *= 1099511628211ULL;
  };
  const auto mix_session = [&](const SessionRecord& session) {
    mix(session.id.slot());
    mix(session.id.generation());
    for (const char character : session.session_name()) {
      mix(static_cast<unsigned char>(character));
    }
    mix(session.mutation_generation);
    mix(session.attachment_runtime.client >= 0 ? 1U : 0U);
  };
  // Stable-ID subscriptions already identify one slot. Avoid scanning all Sessions on every
  // reactor turn; get() also validates the generation before exposing a replacement Session.
  if (filter.has_value() && filter->id.is_valid()) {
    const auto* const session = sessions.get(filter->id);
    if (session != nullptr && session->active) {
      mix_session(*session);
    }
    return value;
  }
  for (const auto& session : sessions) {
    if (session != nullptr && session->active &&
        (!filter.has_value() || filter->name == session->session_name())) {
      mix_session(*session);
    }
  }
  return value;
}

[[nodiscard]] auto observed_pane(const api::EventSubscription& subscription, Sessions& sessions,
                                 PaneRuntimeStore& runtimes, const std::size_t index) noexcept
    -> ObservedPane {
  if (!subscription.session.has_value() || index >= subscription.panes.size()) {
    return {};
  }
  auto* const session = public_session(sessions, *subscription.session);
  const auto pane_id = std::span(subscription.panes).subspan(index, 1).front().id;
  auto* const pane = session == nullptr ? nullptr : find_pane(*session, pane_id);
  auto* const runtime = pane == nullptr ? nullptr : find_pane_runtime(runtimes, *session, *pane);
  return {.session = session, .pane = pane, .runtime = runtime};
}

enum class PaneObservationChange : std::uint8_t { none, closed, process, terminal };

[[nodiscard]] auto pane_observation_change(const PublicObservedPaneState& observed,
                                           const ObservedPane target) noexcept
    -> PaneObservationChange {
  if (target.pane == nullptr || target.runtime == nullptr) {
    return observed.present ? PaneObservationChange::closed : PaneObservationChange::none;
  }
  const auto process = target.pane->process_exit.value_or(ProcessExit{});
  if (!observed.present || target.pane->process_exit.has_value() != observed.process_exited ||
      process.kind != observed.process.kind || process.value != observed.process.value) {
    return PaneObservationChange::process;
  }
  return target.runtime->observation_generation != observed.terminal_generation
             ? PaneObservationChange::terminal
             : PaneObservationChange::none;
}

// Examine only the subscription's bounded (at most eight) stable IDs on an existing reactor wake.
// Do not stop on an unchanged Pane: no future socket/timer wake is promised for a later changed ID.
[[nodiscard]] auto
next_changed_pane(const api::EventSubscription& subscription,
                  const std::array<PublicObservedPaneState, api::event_panes_max>& observed,
                  Sessions& sessions, PaneRuntimeStore& runtimes, std::size_t& cursor) noexcept
    -> std::optional<std::size_t> {
  for (std::size_t visited = 0; visited < subscription.panes.size(); ++visited) {
    const auto index = cursor % subscription.panes.size();
    cursor = (index + 1U) % subscription.panes.size();
    if (pane_observation_change(std::span(observed).subspan(index, 1).front(),
                                observed_pane(subscription, sessions, runtimes, index)) !=
        PaneObservationChange::none) {
      return index;
    }
  }
  return std::nullopt;
}

// Returns the in-scope Pane with the oldest signal stamp newer than watermark. Stamps are
// daemon-wide and monotonic, so Sessions whose newest stamp is already observed are skipped without
// visiting their Panes, and repeated changes to one Pane coalesce into its latest value. When no
// in-scope Pane changed, the watermark advances to the clock so later turns are O(1).
[[nodiscard]] auto signal_pane_selected(const api::EventSubscription& subscription,
                                        const PaneId pane) noexcept -> bool {
  return subscription.panes.empty() ||
         std::ranges::any_of(subscription.panes, [pane](const api::PaneSelector& selected) {
           return selected.id == pane;
         });
}

// Replaces oldest with this Session's Pane whose stamp is the smallest one after watermark.
void find_oldest_signal(const api::EventSubscription& subscription, SessionRecord& session,
                        PaneRuntimeStore& runtimes, const std::uint64_t watermark,
                        ObservedPane& oldest) noexcept {
  if (!session.active || session.signal_stamp <= watermark) {
    return;
  }
  for (auto& slot : session.panes) {
    auto* const runtime =
        slot.pane == nullptr ? nullptr : find_pane_runtime(runtimes, session, *slot.pane);
    if (runtime == nullptr || runtime->signal_stamp <= watermark ||
        (oldest.runtime != nullptr && runtime->signal_stamp >= oldest.runtime->signal_stamp) ||
        !signal_pane_selected(subscription, slot.pane->id)) {
      continue;
    }
    oldest = {.session = &session, .pane = slot.pane.get(), .runtime = runtime};
  }
}

[[nodiscard]] auto next_signal_pane(const api::EventSubscription& subscription, Sessions& sessions,
                                    PaneRuntimeStore& runtimes, std::uint64_t& watermark) noexcept
    -> ObservedPane {
  if (!subscription.signals || runtimes.signal_clock() <= watermark) {
    return {};
  }
  ObservedPane oldest;
  if (subscription.session.has_value()) {
    if (auto* const session = public_session(sessions, *subscription.session); session != nullptr) {
      find_oldest_signal(subscription, *session, runtimes, watermark, oldest);
    }
  } else {
    for (auto& session : sessions) {
      if (session != nullptr) {
        find_oldest_signal(subscription, *session, runtimes, watermark, oldest);
      }
    }
  }
  if (oldest.pane == nullptr) {
    watermark = runtimes.signal_clock();
  }
  return oldest;
}

[[nodiscard]] auto append_signal_event(std::string& output, const std::uint64_t sequence,
                                       const ObservedPane target) -> bool {
  return append_public(output, R"({"schema":"lemma.event/v1","sequence":)") &&
         append_public_number(output, sequence) &&
         append_public(output, R"(,"event":"pane.signal","session":)") &&
         append_public_id(output, target.session->id) && append_public(output, R"(,"pane":)") &&
         append_public_id(output, target.pane->id) && append_public(output, R"(,"signals":)") &&
         append_public_signals(output, *target.runtime) && append_public(output, "}");
}

[[nodiscard]] auto append_public_process(std::string& output, const Pane& pane,
                                         const PaneRuntime& runtime) -> bool {
  if (!pane.process_exit.has_value()) {
    return append_public(output,
                         runtime.child > 0 ? R"({"state":"running"})" : R"({"state":"exiting"})");
  }
  switch (pane.process_exit->kind) {
  case ProcessExitKind::unknown:
    return append_public(output, R"({"state":"exited_unknown"})");
  case ProcessExitKind::exited:
    return append_public(output, R"({"state":"exited","code":)") &&
           append_public_number(output, pane.process_exit->value) && append_public(output, "}");
  case ProcessExitKind::signaled:
    return append_public(output, R"({"state":"signaled","signal":)") &&
           append_public_number(output, pane.process_exit->value) && append_public(output, "}");
  }
  return false;
}

[[nodiscard]] auto append_event_header(std::string& output, PendingConnection& pending,
                                       const std::string_view event) -> bool {
  return append_public(output, R"({"schema":"lemma.event/v1","sequence":)") &&
         append_public_number(output, pending.event_sequence++) &&
         append_public(output, R"(,"event":)") && api::append_json_string(output, event);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto append_sessions_snapshot(std::string& output, const Sessions& sessions,
                                            const std::optional<api::SessionSelector>& filter)
    -> bool {
  ConnectionOutput encoded;
  bool appended = false;
  if (!filter.has_value()) {
    appended = append_structured_sessions(encoded, sessions);
  } else {
    const SessionRecord* session = filter->id.is_valid() ? sessions.get(filter->id) : nullptr;
    if (session == nullptr && !filter->id.is_valid()) {
      for (const auto& candidate : sessions) {
        if (candidate != nullptr && candidate->active &&
            candidate->session_name() == filter->name) {
          session = candidate.get();
          break;
        }
      }
    }
    appended =
        encoded.append_text("[") &&
        (session == nullptr || !session->active || append_structured_session(encoded, *session)) &&
        encoded.append_text("]");
  }
  std::string json;
  return appended && copy_connection_json(encoded, json) && append_public(output, json);
}

[[nodiscard]] auto append_screen_event(std::string& output, PendingConnection& pending,
                                       const ObservedPane target,
                                       const std::span<std::byte> scratch,
                                       const std::string_view event = "pane.screen") -> bool {
  if (target.session == nullptr || target.pane == nullptr || target.runtime == nullptr) {
    return false;
  }
  const auto formatted = format_public_visible(target.runtime->terminal, vt::ScreenFormat::plain,
                                               api::CaptureWrap::rendered, 0, scratch);
  const auto at_prompt = target.runtime->terminal.cursor_at_prompt();
  if (!formatted.result.has_value() || !at_prompt.has_value() ||
      !append_event_header(output, pending, event) || !append_public(output, R"(,"session":)") ||
      !append_public_id(output, target.session->id) || !append_public(output, R"(,"pane":)") ||
      !append_public_id(output, target.pane->id) || !append_public(output, R"(,"generation":)") ||
      !append_public_number(output, target.runtime->observation_generation) ||
      !append_public(output, R"(,"cursor_at_prompt":)") ||
      !append_public(output, *at_prompt ? "true" : "false")) {
    return false;
  }
  // Byte payloads and character storage have the same object representation.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view text(reinterpret_cast<const char*>(scratch.data()), *formatted.result);
  return append_public(output, R"(,"capture":)") &&
         api::append_capture(output, api::CaptureSource::visible, api::CaptureFormat::plain,
                             api::CaptureWrap::rendered, target.runtime->observation_generation,
                             formatted.truncated, text) &&
         append_public(output, "}\n");
}

[[nodiscard]] auto append_extension_screen_event(std::string& output, const std::uint32_t sequence,
                                                 const ObservedPane target,
                                                 const std::span<std::byte> scratch) -> bool {
  if (target.session == nullptr || target.pane == nullptr || target.runtime == nullptr) {
    return false;
  }
  const auto formatted = format_public_visible(target.runtime->terminal, vt::ScreenFormat::plain,
                                               api::CaptureWrap::rendered, 0, scratch);
  const auto at_prompt = target.runtime->terminal.cursor_at_prompt();
  if (!formatted.result.has_value() || !at_prompt.has_value() ||
      !append_public(output, R"({"schema":"lemma.event/v1","sequence":)") ||
      !append_public_number(output, sequence) ||
      !append_public(output, R"(,"event":"pane.screen","session":)") ||
      !append_public_id(output, target.session->id) || !append_public(output, R"(,"pane":)") ||
      !append_public_id(output, target.pane->id) || !append_public(output, R"(,"generation":)") ||
      !append_public_number(output, target.runtime->observation_generation) ||
      !append_public(output, R"(,"cursor_at_prompt":)") ||
      !append_public(output, *at_prompt ? "true" : "false")) {
    return false;
  }
  // Byte payloads and character storage have the same object representation.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view text(reinterpret_cast<const char*>(scratch.data()), *formatted.result);
  return append_public(output, R"(,"capture":)") &&
         api::append_capture(output, api::CaptureSource::visible, api::CaptureFormat::plain,
                             api::CaptureWrap::rendered, target.runtime->observation_generation,
                             formatted.truncated, text) &&
         append_public(output, "}");
}

// Presentation observation contains bounded semantic/editor state, never cells or terminal bytes.
// Consumers choose labels, layout and interaction policy in their own process.
[[nodiscard]] auto presentation_signature(const SessionRecord& session,
                                          const PaneRuntimeStore& runtimes) noexcept
    -> std::uint64_t {
  auto signature = current_status_signature(session, runtimes);
  const auto mix = [&](const std::uint64_t value) {
    signature = (signature ^ value) * 1'099'511'628'211ULL;
  };
  mix(session.mutation_generation);
  mix(session.attachment.columns);
  mix(session.attachment.rows);
  mix(session.attachment_runtime.connection_id.generation());
  mix(session.attachment_runtime.client >= 0 ? 1U : 0U);
  for (const char byte : session.session_name()) {
    mix(static_cast<unsigned char>(byte));
  }
  for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
    const auto id = session.tab_order.at(position);
    const auto* const tab = id.has_value() ? find_tab(session, *id) : nullptr;
    if (tab != nullptr) {
      for (const char byte : tab_title(session, *tab, runtimes)) {
        mix(static_cast<unsigned char>(byte));
      }
    }
  }
  return signature;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto append_presentation(std::string& output, const SessionRecord& session,
                                       const PaneRuntimeStore& runtimes) -> bool {
  const auto& attachment = session.attachment;
  if (!append_public(output, R"(,"presentation":{"session":)") ||
      !append_public_id(output, session.id) || !append_public(output, R"(,"name":)") ||
      !api::append_json_string(output, session.session_name()) ||
      !append_public(output, R"(,"connection":)") ||
      !(session.attachment_runtime.client >= 0
            ? append_public_id(output, session.attachment_runtime.connection_id)
            : append_public(output, "null")) ||
      !append_public(output, R"(,"columns":)") ||
      !append_public_number(output, attachment.columns) || !append_public(output, R"(,"rows":)") ||
      !append_public_number(output, attachment.rows) || !append_public(output, R"(,"tabs":[)")) {
    return false;
  }
  for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
    const auto id = session.tab_order.at(position);
    const auto* const tab = id.has_value() ? find_tab(session, *id) : nullptr;
    if (tab == nullptr || (position > 0 && !append_public(output, ",")) ||
        !append_public(output, R"({"id":)") || !append_public_id(output, tab->id) ||
        !append_public(output, R"(,"position":)") || !append_public_number(output, position + 1U) ||
        !append_public(output, R"(,"title":)") ||
        !api::append_json_string(output, tab_title(session, *tab, runtimes)) ||
        !append_public(output, R"(,"focused_pane":)") ||
        !append_public_id(output, tab->focused_pane) || !append_public(output, R"(,"active":)") ||
        !append_public(output, tab->id == session.active_tab ? "true}" : "false}")) {
      return false;
    }
  }
  constexpr std::array copy_feedback_names{"none",           "no_match",  "empty_selection",
                                           "clipboard_busy", "too_large", "failed"};
  const auto& copy = attachment.copy_mode;
  const auto label = copy.active() || attachment.rename_prompt.active() ||
                             attachment.command_line.active || attachment.message_view.active
                         ? session.interaction_router.active_label()
                         : session.input_router.active_label();
  const auto prompt = collect_status_prompt(session, copy.draft_query_view());
  const auto* const copy_runtime = copy_mode_runtime(session, runtimes);
  const auto viewport =
      copy_runtime != nullptr
          ? copy_runtime->terminal.viewport_state()
          : std::expected<vt::ViewportState, vt::Error>{std::unexpected(vt::Error::invalid_state)};
  const auto total = viewport.has_value() ? viewport->total_rows : 0U;
  const auto visible = viewport.has_value() ? viewport->visible_rows : 0U;
  const auto covered = viewport.has_value() ? viewport->offset + visible : 0U;
  return append_public(output, R"(],"mode":)") && api::append_json_string(output, label) &&
         append_public(output, R"(,"prompt":{"kind":)") &&
         api::append_json_string(output, prompt.target) && append_public(output, R"(,"value":)") &&
         api::append_json_string(output, prompt.value) && append_public(output, R"(,"cursor":)") &&
         append_public_number(output, prompt.cursor) && append_public(output, R"(,"feedback":)") &&
         api::append_json_string(output, prompt.feedback) &&
         append_public(output, R"(},"message":)") &&
         api::append_json_string(output, visible_status_message(session)) &&
         append_public(output, R"(,"copy":{"active":)") &&
         append_public(output, copy.active() ? "true" : "false") &&
         append_public(output, R"(,"searching":)") &&
         append_public(output, copy.phase == CopyModePhase::searching ? "true" : "false") &&
         append_public(output, R"(,"query":)") &&
         api::append_json_string(output, copy.query_view()) &&
         append_public(output, R"(,"backward":)") &&
         append_public(output,
                       copy.search_direction == CopySearchDirection::backward ? "true" : "false") &&
         append_public(output, R"(,"below":)") &&
         append_public_number(output, total > covered ? total - covered : 0U) &&
         append_public(output, R"(,"history":)") &&
         append_public_number(output, total > visible ? total - visible : 0U) &&
         append_public(output, R"(,"feedback":)") &&
         api::append_json_string(output,
                                 copy_feedback_names.at(static_cast<std::size_t>(copy.feedback))) &&
         append_public(output, "}}");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto encode_initial_snapshot(PendingConnection& pending, Sessions& sessions,
                                           PaneRuntimeStore& runtimes,
                                           const std::span<std::byte> scratch,
                                           const std::uint64_t sequence) -> std::string {
  std::string output;
  try {
    pending.event_sequence = sequence;
    pending.observed_pane_cursor = 0;
    pending.observed_signal_stamp = 0;
    if (!append_event_header(output, pending, "snapshot") ||
        !append_public(output, R"(,"sessions":)") ||
        !append_sessions_snapshot(output, sessions, pending.subscription.session)) {
      return {};
    }
    if (pending.subscription.presentation && pending.subscription.session.has_value()) {
      const auto* const target = public_session(sessions, *pending.subscription.session);
      if (target == nullptr || !append_presentation(output, *target, runtimes)) {
        return {};
      }
      pending.observed_presentation_hash = presentation_signature(*target, runtimes);
    }
    if (pending.subscription.panes.size() == 1U) {
      const auto target = observed_pane(pending.subscription, sessions, runtimes, 0);
      auto& observed = pending.observed_panes.front();
      const bool present =
          target.session != nullptr && target.pane != nullptr && target.runtime != nullptr;
      if (!append_public(output, R"(,"present":)") ||
          !append_public(output, present ? "true" : "false")) {
        return {};
      }
      if (present) {
        if (!append_public(output, R"(,"session":)") ||
            !append_public_id(output, target.session->id) ||
            !append_public(output, R"(,"pane":)") || !append_public_id(output, target.pane->id) ||
            !append_public(output, R"(,"generation":)") ||
            !append_public_number(output, target.runtime->observation_generation) ||
            !append_public(output, R"(,"process":)") ||
            !append_public_process(output, *target.pane, *target.runtime)) {
          return {};
        }
        if (pending.subscription.screen) {
          const auto formatted =
              format_public_visible(target.runtime->terminal, vt::ScreenFormat::plain,
                                    api::CaptureWrap::rendered, 0, scratch);
          const auto at_prompt = target.runtime->terminal.cursor_at_prompt();
          if (!formatted.result.has_value() || !at_prompt.has_value() ||
              !append_public(output, R"(,"cursor_at_prompt":)") ||
              !append_public(output, *at_prompt ? "true" : "false")) {
            return {};
          }
          // Byte payloads and character storage have the same object representation.
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          const std::string_view text(reinterpret_cast<const char*>(scratch.data()),
                                      *formatted.result);
          if (!append_public(output, R"(,"capture":)") ||
              !api::append_capture(output, api::CaptureSource::visible, api::CaptureFormat::plain,
                                   api::CaptureWrap::rendered,
                                   target.runtime->observation_generation, formatted.truncated,
                                   text)) {
            return {};
          }
        }
        observed.present = true;
        observed.terminal_generation = target.runtime->observation_generation;
        observed.process = target.pane->process_exit.value_or(ProcessExit{});
        observed.process_exited = target.pane->process_exit.has_value();
      } else {
        observed = {};
      }
    } else if (!pending.subscription.panes.empty()) {
      if (!append_public(output, R"(,"panes":[)")) {
        return {};
      }
      for (std::size_t index = 0; index < pending.subscription.panes.size(); ++index) {
        if (index > 0 && !append_public(output, ",")) {
          return {};
        }
        const auto target = observed_pane(pending.subscription, sessions, runtimes, index);
        auto& observed = std::span(pending.observed_panes).subspan(index, 1).front();
        const auto requested = std::span(pending.subscription.panes).subspan(index, 1).front().id;
        const bool present =
            target.session != nullptr && target.pane != nullptr && target.runtime != nullptr;
        if (!append_public(output, R"({"pane":)") || !append_public_id(output, requested) ||
            !append_public(output, R"(,"present":)") ||
            !append_public(output, present ? "true" : "false")) {
          return {};
        }
        if (present && (!append_public(output, R"(,"session":)") ||
                        !append_public_id(output, target.session->id) ||
                        !append_public(output, R"(,"generation":)") ||
                        !append_public_number(output, target.runtime->observation_generation) ||
                        !append_public(output, R"(,"process":)") ||
                        !append_public_process(output, *target.pane, *target.runtime))) {
          return {};
        }
        if (!append_public(output, "}")) {
          return {};
        }
        if (present) {
          observed.present = true;
          observed.terminal_generation =
              pending.subscription.screen ? 0 : target.runtime->observation_generation;
          observed.process = target.pane->process_exit.value_or(ProcessExit{});
          observed.process_exited = target.pane->process_exit.has_value();
        } else {
          observed = {};
        }
      }
      if (!append_public(output, "]")) {
        return {};
      }
    }
    pending.observed_semantic_hash = semantic_hash(sessions, pending.subscription.session);
    return append_public(output, "}\n") ? output : std::string{};
  } catch (...) {
    return {};
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto service_public_observers(PendingConnections& connections, Sessions& sessions,
                                            PaneRuntimeStore& runtimes,
                                            PublicScratch& scratch_owner,
                                            std::size_t& cursor) noexcept -> bool {
  bool formatted_screen = false;
  bool screen_work_pending = false;
  for (std::size_t visited = 0; visited < connections.size(); ++visited) {
    const auto slot = (cursor + visited) % connections.size();
    auto* const owner = std::span(connections).subspan(slot, 1).front().get();
    if (owner == nullptr ||
        (owner->state != PendingState::prepare_public_observer &&
         owner->state != PendingState::observe) ||
        !owner->public_output.empty()) {
      continue;
    }
    auto& pending = *owner;
    std::string output;
    try {
      if (pending.state == PendingState::prepare_public_observer) {
        const bool snapshot_screen =
            pending.subscription.screen && pending.subscription.panes.size() == 1U;
        if (snapshot_screen && formatted_screen) {
          screen_work_pending = true;
          continue;
        }
        auto scratch = std::span<std::byte>{};
        if (snapshot_screen) {
          scratch = acquire_public_scratch(scratch_owner);
        }
        if (snapshot_screen && scratch.empty()) {
          finish_public_output(pending,
                               encode_public_error({.reason = "resource_failure", .field = {}}),
                               PendingDisposition::close);
          continue;
        }
        auto snapshot = encode_initial_snapshot(pending, sessions, runtimes, scratch);
        if (snapshot.empty()) {
          finish_public_output(pending,
                               encode_public_error({.reason = "resource_failure", .field = {}}),
                               PendingDisposition::close);
          continue;
        }
        finish_public_output(pending, std::move(snapshot), PendingDisposition::keep_observe);
        if (snapshot_screen) {
          formatted_screen = true;
          cursor = (slot + 1U) % connections.size();
        }
        continue;
      }
      const auto hash = semantic_hash(sessions, pending.subscription.session);
      if (hash != pending.observed_semantic_hash) {
        if (!append_event_header(output, pending, "state.changed")) {
          pending.state = PendingState::unused;
          continue;
        }
        if (pending.subscription.session.has_value()) {
          const auto* const changed = public_session(sessions, *pending.subscription.session);
          if (changed != nullptr && changed->active &&
              (!append_public(output, R"(,"session":)") || !append_public_id(output, changed->id) ||
               !append_public(output, R"(,"revision":)") ||
               !append_public_number(output, changed->mutation_generation))) {
            pending.state = PendingState::unused;
            continue;
          }
        } else if (!append_public(output, R"(,"sessions":)") ||
                   !append_sessions_snapshot(output, sessions, std::nullopt)) {
          pending.state = PendingState::unused;
          continue;
        }
        if (!append_public(output, "}\n")) {
          pending.state = PendingState::unused;
          continue;
        }
        pending.observed_semantic_hash = hash;
      }
      if (pending.subscription.presentation && pending.subscription.session.has_value()) {
        const auto* const target = public_session(sessions, *pending.subscription.session);
        if (target == nullptr) {
          pending.state = PendingState::unused;
          continue;
        }
        const auto presentation_hash = presentation_signature(*target, runtimes);
        if (presentation_hash != pending.observed_presentation_hash) {
          if (!append_event_header(output, pending, "attachment.changed") ||
              !append_presentation(output, *target, runtimes) || !append_public(output, "}\n")) {
            pending.state = PendingState::unused;
            continue;
          }
          pending.observed_presentation_hash = presentation_hash;
        }
      }
      if (const auto changed = next_changed_pane(pending.subscription, pending.observed_panes,
                                                 sessions, runtimes, pending.observed_pane_cursor);
          changed.has_value()) {
        const auto pane_index = *changed;
        auto& observed = pending.observed_panes.at(pane_index);
        const auto target = observed_pane(pending.subscription, sessions, runtimes, pane_index);
        const bool present = target.pane != nullptr && target.runtime != nullptr;
        if (!present && observed.present) {
          if (!append_event_header(output, pending, "pane.closed") ||
              !append_public(output, R"(,"pane":)") ||
              !append_public_id(output, pending.subscription.panes.at(pane_index).id) ||
              !append_public(output, "}\n")) {
            pending.state = PendingState::unused;
            continue;
          }
          observed = {};
        } else if (present) {
          const auto process = target.pane->process_exit.value_or(ProcessExit{});
          const bool process_exited = target.pane->process_exit.has_value();
          if (pane_observation_change(observed, target) == PaneObservationChange::process) {
            if (!append_event_header(output, pending, "pane.process") ||
                !append_public(output, R"(,"session":)") ||
                !append_public_id(output, target.session->id) ||
                !append_public(output, R"(,"pane":)") ||
                !append_public_id(output, target.pane->id) ||
                !append_public(output, R"(,"process":)") ||
                !append_public_process(output, *target.pane, *target.runtime) ||
                !append_public(output, "}\n")) {
              pending.state = PendingState::unused;
              continue;
            }
            observed.process = process;
            observed.process_exited = process_exited;
          }
          if (target.runtime->observation_generation != observed.terminal_generation) {
            if (!output.empty() || (pending.subscription.screen && formatted_screen)) {
              screen_work_pending = screen_work_pending || pending.subscription.screen;
            } else if (pending.subscription.screen) {
              const auto scratch = acquire_public_scratch(scratch_owner);
              if (scratch.empty() || !append_screen_event(output, pending, target, scratch)) {
                pending.state = PendingState::unused;
                continue;
              }
              observed.terminal_generation = target.runtime->observation_generation;
              formatted_screen = true;
              cursor = (slot + 1U) % connections.size();
            } else {
              if (!append_event_header(output, pending, "pane.terminal") ||
                  !append_public(output, R"(,"session":)") ||
                  !append_public_id(output, target.session->id) ||
                  !append_public(output, R"(,"pane":)") ||
                  !append_public_id(output, target.pane->id) ||
                  !append_public(output, R"(,"generation":)") ||
                  !append_public_number(output, target.runtime->observation_generation) ||
                  !append_public(output, R"(,"changed":["screen"]})") ||
                  !append_public(output, "\n")) {
                pending.state = PendingState::unused;
                continue;
              }
              observed.terminal_generation = target.runtime->observation_generation;
            }
          }
          observed.present = true;
        }
      }
      if (const auto signal = next_signal_pane(pending.subscription, sessions, runtimes,
                                               pending.observed_signal_stamp);
          signal.pane != nullptr) {
        if (!append_signal_event(output, pending.event_sequence++, signal) ||
            !append_public(output, "\n")) {
          pending.state = PendingState::unused;
          continue;
        }
        pending.observed_signal_stamp = signal.runtime->signal_stamp;
      }
      if (!output.empty()) {
        finish_public_output(pending, std::move(output), PendingDisposition::keep_observe);
      }
    } catch (...) {
      pending.state = PendingState::unused;
    }
  }
  return screen_work_pending;
}

// One fair peer and at most one state or terminal Event is serialized per reactor turn.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void service_extension_observers(extension::Runtime& extensions, Sessions& sessions,
                                 PaneRuntimeStore& runtimes, PublicScratch& scratch_owner,
                                 ExtensionObservations& observations,
                                 std::size_t& cursor) noexcept {
  std::array<extension::PeerView, limits::extension_sessions_hard_max> peers{};
  const auto active = extensions.peer_views(peers);
  if (active.empty()) {
    cursor = 0;
    return;
  }
  cursor %= active.size();
  for (std::size_t visited = 0; visited < active.size(); ++visited) {
    const auto peer = active.subspan((cursor + visited) % active.size(), 1).front();
    const auto session_id = extensions.session(peer.owner);
    auto* const session = sessions.get(session_id);
    if (session_id.is_valid() && (session == nullptr || !session->active ||
                                  session->attachment.id != extensions.attachment(peer.owner))) {
      static_cast<void>(extensions.disconnect(peer.owner));
      continue;
    }
    if (!extensions.has_capability(peer.owner, extension::capability_observe)) {
      continue;
    }
    LEMMA_ASSERT(peer.slot < observations.size());
    auto& observed = std::span(observations).subspan(peer.slot, 1).front();
    const auto* const subscription = extensions.subscription(peer.owner);
    if (subscription == nullptr) {
      continue;
    }
    if (session_id.is_valid() && (session == nullptr || !session->active)) {
      static_cast<void>(extensions.disconnect(peer.owner));
      return;
    }
    const auto hash = semantic_hash(sessions, subscription->session);
    LEMMA_ASSERT(observed.owner == peer.owner);
    if (extensions.output_bytes(peer.owner) != 0) {
      continue;
    }
    const auto presentation_hash = subscription->presentation && session != nullptr
                                       ? presentation_signature(*session, runtimes)
                                       : std::uint64_t{0};
    if (hash != observed.semantic_hash || presentation_hash != observed.presentation_hash) {
      try {
        std::string event = R"({"schema":"lemma.event/v1","sequence":)" +
                            std::to_string(extensions.event_sequence(peer.owner)) +
                            R"(,"event":"state.changed","sessions":)";
        if (!append_sessions_snapshot(event, sessions, subscription->session) ||
            (subscription->presentation && !append_presentation(event, *session, runtimes)) ||
            !append_public(event, "}")) {
          static_cast<void>(extensions.disconnect(peer.owner));
          return;
        }
        if (extensions.send_event(peer.owner, event)) {
          observed.semantic_hash = hash;
          observed.presentation_hash = presentation_hash;
        }
        cursor = (cursor + visited + 1U) % active.size();
        return;
      } catch (...) {
        static_cast<void>(extensions.disconnect(peer.owner));
        return;
      }
    }
    // A signal turn looks for a signal before advancing the Pane cursor, so a skipped Pane change
    // stays next in rotation instead of being dropped.
    auto signal = observed.signal_turn
                      ? next_signal_pane(*subscription, sessions, runtimes, observed.signal_stamp)
                      : ObservedPane{};
    observed.signal_turn = false;
    const auto changed = signal.pane == nullptr
                             ? next_changed_pane(*subscription, observed.panes, sessions, runtimes,
                                                 observed.pane_cursor)
                             : std::nullopt;
    if (signal.pane == nullptr && !changed.has_value()) {
      signal = next_signal_pane(*subscription, sessions, runtimes, observed.signal_stamp);
    }
    if (signal.pane != nullptr) {
      try {
        std::string event;
        if (!append_signal_event(event, extensions.event_sequence(peer.owner), signal)) {
          static_cast<void>(extensions.disconnect(peer.owner));
          return;
        }
        if (extensions.send_event(peer.owner, event)) {
          observed.signal_stamp = signal.runtime->signal_stamp;
        }
      } catch (...) {
        static_cast<void>(extensions.disconnect(peer.owner));
        return;
      }
      cursor = (cursor + visited + 1U) % active.size();
      return;
    }
    if (!changed.has_value()) {
      continue;
    }
    const auto pane_index = *changed;
    observed.signal_turn = subscription->signals;
    auto& pane_state = std::span(observed.panes).subspan(pane_index, 1).front();
    const auto target = observed_pane(*subscription, sessions, runtimes, pane_index);
    auto* const pane = target.pane;
    auto* const runtime = target.runtime;
    const auto change = pane_observation_change(pane_state, target);
    try {
      std::string event;
      if (change == PaneObservationChange::closed || change == PaneObservationChange::process) {
        if (!append_public(event, R"({"schema":"lemma.event/v1","sequence":)") ||
            !append_public_number(event, extensions.event_sequence(peer.owner)) ||
            !append_public(event, change == PaneObservationChange::closed
                                      ? R"(,"event":"pane.closed","pane":)"
                                      : R"(,"event":"pane.process","pane":)") ||
            !append_public_id(event, subscription->panes.at(pane_index).id) ||
            (change == PaneObservationChange::process &&
             (!append_public(event, R"(,"session":)") || !append_public_id(event, session->id) ||
              !append_public(event, R"(,"process":)") ||
              !append_public_process(event, *pane, *runtime))) ||
            !append_public(event, "}")) {
          static_cast<void>(extensions.disconnect(peer.owner));
          return;
        }
      } else if (subscription->screen) {
        const auto scratch = acquire_public_scratch(scratch_owner);
        if (scratch.empty() ||
            !append_extension_screen_event(event, extensions.event_sequence(peer.owner),
                                           {.session = session, .pane = pane, .runtime = runtime},
                                           scratch)) {
          static_cast<void>(extensions.disconnect(peer.owner));
          return;
        }
      } else if (!append_public(event, R"({"schema":"lemma.event/v1","sequence":)") ||
                 !append_public_number(event, extensions.event_sequence(peer.owner)) ||
                 !append_public(event, R"(,"event":"pane.terminal","session":)") ||
                 !append_public_id(event, session->id) || !append_public(event, R"(,"pane":)") ||
                 !append_public_id(event, pane->id) || !append_public(event, R"(,"generation":)") ||
                 !append_public_number(event, runtime->observation_generation) ||
                 !append_public(event, R"(,"changed":["screen"]})")) {
        static_cast<void>(extensions.disconnect(peer.owner));
        return;
      }
      if (extensions.send_event(peer.owner, event)) {
        if (change == PaneObservationChange::closed) {
          pane_state = {};
        } else if (change == PaneObservationChange::process) {
          pane_state.present = true;
          pane_state.process = pane->process_exit.value_or(ProcessExit{});
          pane_state.process_exited = pane->process_exit.has_value();
        } else {
          pane_state.terminal_generation = runtime->observation_generation;
        }
      }
    } catch (...) {
      static_cast<void>(extensions.disconnect(peer.owner));
    }
    cursor = (cursor + visited + 1U) % active.size();
    return;
  }
}

// Once setup capacity is occupied, a small independent pool reads only the protocol discriminator
// needed to return a wire-compatible capacity response. These responders do not consume setup
// slots, so an attach peer can receive its framed rejection even while every setup slot is busy.

} // namespace lemma::core::engine_detail
