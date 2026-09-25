#include "core/engine_state.hpp"

#include "core/session.hpp"
#include "lemma/assert.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "platform/io.hpp"
#include "protocol/attachment.hpp"

#include <csignal> // IWYU pragma: keep -- owns SIGHUP on POSIX Clang
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace lemma::core::engine_detail {

using platform::close_descriptor;

PaneRuntime::PaneRuntime(vt::Terminal&& created_terminal,
                         const std::size_t scrollback_reservation) noexcept
    : terminal(std::move(created_terminal)), scrollback_bytes_reserved(scrollback_reservation) {}

PaneRuntime::~PaneRuntime() {
  if (child > 0) {
    static_cast<void>(::kill(child, SIGHUP));
    child = -1;
  }
  close_descriptor(pty);
}

void PaneRuntime::fail(const PaneRuntimeFailure reason) noexcept {
  if (!failure.has_value()) {
    failure = reason;
  }
}

auto PaneRuntimeStore::insert(const PaneAddress address,
                              std::unique_ptr<PaneRuntime> runtime) noexcept -> bool {
  if (!address_in_bounds(address) || runtime == nullptr || !runtime->publishable() ||
      size_ == limits::panes_hard_max ||
      runtime->scrollback_bytes_reserved >
          limits::terminal_scrollback_bytes_aggregate_max - scrollback_bytes_reserved_) {
    return false;
  }
  auto& session = std::span(sessions_).subspan(address.session.slot(), 1).front();
  if (session == nullptr) {
    try {
      session = std::make_unique<SessionSlots>(address.session.generation());
    } catch (const std::bad_alloc&) {
      return false;
    }
  } else if (session->generation != address.session.generation()) {
    return false;
  }
  auto& pane = std::span(session->panes).subspan(address.pane.slot(), 1).front();
  if (pane.runtime != nullptr) {
    return false;
  }
  pane.generation = address.pane.generation();
  scrollback_bytes_reserved_ += runtime->scrollback_bytes_reserved;
  pane.runtime = std::move(runtime);
  ++session->size;
  ++size_;
  return true;
}

auto PaneRuntimeStore::get(const PaneAddress address) noexcept -> PaneRuntime* {
  if (!address_in_bounds(address)) {
    return nullptr;
  }
  auto& session = std::span(sessions_).subspan(address.session.slot(), 1).front();
  if (session == nullptr || session->generation != address.session.generation()) {
    return nullptr;
  }
  auto& pane = std::span(session->panes).subspan(address.pane.slot(), 1).front();
  return pane.generation == address.pane.generation() ? pane.runtime.get() : nullptr;
}

auto PaneRuntimeStore::get(const PaneAddress address) const noexcept -> const PaneRuntime* {
  if (!address_in_bounds(address)) {
    return nullptr;
  }
  const auto& session = std::span(sessions_).subspan(address.session.slot(), 1).front();
  if (session == nullptr || session->generation != address.session.generation()) {
    return nullptr;
  }
  const auto& pane = std::span(session->panes).subspan(address.pane.slot(), 1).front();
  return pane.generation == address.pane.generation() ? pane.runtime.get() : nullptr;
}

auto PaneRuntimeStore::erase(const PaneAddress address) noexcept -> bool {
  if (!address_in_bounds(address)) {
    return false;
  }
  auto& session = std::span(sessions_).subspan(address.session.slot(), 1).front();
  if (session == nullptr || session->generation != address.session.generation()) {
    return false;
  }
  auto& pane = std::span(session->panes).subspan(address.pane.slot(), 1).front();
  if (pane.runtime == nullptr || pane.generation != address.pane.generation()) {
    return false;
  }
  release_scrollback(pane.runtime->scrollback_bytes_reserved);
  pane.runtime.reset();
  pane.generation = 0;
  --session->size;
  --size_;
  if (session->size == 0) {
    session.reset();
  }
  return true;
}

void PaneRuntimeStore::erase_session(const SessionId session_id) noexcept {
  if (!session_id.is_valid() || session_id.slot() >= limits::sessions_hard_max) {
    return;
  }
  auto& session = std::span(sessions_).subspan(session_id.slot(), 1).front();
  if (session == nullptr || session->generation != session_id.generation()) {
    return;
  }
  for (const auto& pane : session->panes) {
    if (pane.runtime != nullptr) {
      release_scrollback(pane.runtime->scrollback_bytes_reserved);
    }
  }
  LEMMA_ASSERT(size_ >= session->size);
  size_ -= session->size;
  session.reset();
}

AttachmentRuntime::~AttachmentRuntime() { close_descriptor(client); }

void AttachmentRuntime::reset_connection() noexcept {
  copy_mode = {};
  clipboard_write.reset();
  graphics.reset();
  clipboard = nullptr;
  clipboard_owner.reset();
  kitty_clipboard_supported.reset();
  osc52_read_retired = false;
  clipboard_paste.reset();
  interactive_paste.reset();
  close_descriptor(client);
  connection_id = {};
  hosted_command.reset();
  pending_geometry = {};
  decoder.release();
  output.reset();
  frame.release();
  server_sequence = 2;
  full_redraw_generation = 0;
  pending_attach_slot = std::numeric_limits<std::uint32_t>::max();
  pending_attach_generation = 0;
  status_signature = 0;
  outer_modes.reset();
  outer_title_size = 0;
  outer_title_presented = false;
  outer_attention = {};
  status_valid = false;
  outer_focused = true;
  bell_pending = false;
  input_backpressured = false;
  client_work_pending = false;
  client_close_state = ConnectionCloseState::none;
  client_close_reason = protocol::DisconnectReason::protocol_error;
  retained_input_offset.reset();
  surface_paste.reset();
  pending_routed_input_size = 0;
  status_message_deadline.reset();
  frame_scheduler.cancel();
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  decoded_input_trace_matcher.reset();
  frame_trace_correlation = 0;
#endif
}

SessionRecord::SessionRecord(const std::string_view session_name,
                             const std::string_view initial_working_directory,
                             const std::span<const std::byte> initial_environment,
                             const LaunchEnvironmentMode initial_environment_mode) noexcept
    : Session(session_name, initial_working_directory, initial_environment,
              initial_environment_mode),
      input_router(reactor_input_map()), interaction_router(reactor_input_map()),
      theme(vt::default_theme()) {}

} // namespace lemma::core::engine_detail
