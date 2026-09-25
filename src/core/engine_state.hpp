#ifndef LEMMA_CORE_ENGINE_STATE_HPP
#define LEMMA_CORE_ENGINE_STATE_HPP

#include "clipboard/png_file.hpp"
#include "clipboard/transaction.hpp"
#include "core/client_frame_output.hpp"
#include "core/frame_scheduler.hpp"
#include "core/input.hpp"
#include "core/presentation_gate.hpp"
#include "core/session.hpp"
#include "extension/commands.hpp"
#include "input/input_router.hpp"
#include "lemma/assert.hpp"
#include "lemma/generational_store.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "protocol/attachment.hpp"
#include "render/frame_buffer.hpp"
#include "render/graphics.hpp"
#include "render/pane_composition.hpp"

#ifdef LEMMA_ENABLE_LATENCY_TRACE
#include "diagnostic/latency_trace.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace lemma::core::engine_detail {

inline constexpr std::size_t process_name_bytes_max = 64;
inline constexpr std::size_t copy_escape_bytes_max = 16;

enum class PaneRuntimeFailure : std::uint8_t {
  child_exit,
  pty_read_error,
  pty_write_error,
  terminal_integrity_error,
  scrollback_compression_error,
  resize_consistency_lost,
};

enum class ConnectionCloseState : std::uint8_t {
  none,
  queue_disconnect,
  disconnect_queued,
};

struct WorkingDirectory final {
  std::array<char, protocol::working_directory_bytes_max + 1U> bytes{};
  std::size_t size{0};

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {bytes.data(), size}; }
};

struct SessionName final {
  std::array<char, protocol::session_name_bytes_max> bytes{};
  std::size_t size{0};

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {bytes.data(), size}; }
};

struct PaneRuntime final {
  PaneRuntime(vt::Terminal&& created_terminal, std::size_t scrollback_reservation) noexcept;
  PaneRuntime(const PaneRuntime&) = delete;
  auto operator=(const PaneRuntime&) -> PaneRuntime& = delete;
  PaneRuntime(PaneRuntime&&) = delete;
  auto operator=(PaneRuntime&&) -> PaneRuntime& = delete;
  ~PaneRuntime();

  [[nodiscard]] auto live() const noexcept -> bool { return !failure.has_value(); }
  [[nodiscard]] auto pollable() const noexcept -> bool { return live() && pty >= 0; }
  [[nodiscard]] auto accepts_input() const noexcept -> bool {
    return pollable() && child > 0 && !observed_exit.has_value();
  }
  [[nodiscard]] auto held() const noexcept -> bool { return live() && pty < 0; }
  [[nodiscard]] auto publishable() const noexcept -> bool {
    return accepts_input() && !terminal.integrity_failed();
  }
  void fail(PaneRuntimeFailure reason) noexcept;

  vt::Terminal terminal;
  int pty{-1};
  decltype(::getpid()) child{-1};
  std::array<char, process_name_bytes_max> process_name{};
  std::size_t process_name_size{0};
  std::chrono::steady_clock::time_point next_process_name_refresh;
  PanePtyWriteQueue pending_writes;
  InteractiveDamageLatch interactive_damage;
  PresentationGate presentation_gate;
  std::uint64_t compression_activity{0};
  std::uint64_t mutation_generation{1};
  std::uint64_t observation_generation{1};
  // PaneRuntimeStore::issue_signal_stamp() value of the latest terminal signal change; zero until
  // the application reports one. The signal values themselves remain owned by the terminal.
  std::uint64_t signal_stamp{0};
  std::size_t scrollback_bytes_reserved{0};
  std::chrono::steady_clock::time_point compression_deadline;
  bool compression_scheduled{false};
  // Last focus state queued to (or, without mode 1004, assumed by) the child. Reconciliation
  // derives the desired state from committed Session and Attachment state.
  bool focus_reported{false};
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  diagnostic::LatencyTraceMarkerMatcher input_trace_matcher;
  diagnostic::LatencyTraceMarkerMatcher output_trace_matcher;
#endif
  std::optional<PaneRuntimeFailure> failure;
  std::optional<ProcessExit> observed_exit;
};

struct PaneAddress final {
  SessionId session;
  PaneId pane;

  [[nodiscard]] constexpr auto valid() const noexcept -> bool {
    return session.is_valid() && pane.is_valid();
  }
};

class PaneRuntimeStore final {
  struct PaneSlot final {
    std::unique_ptr<PaneRuntime> runtime;
    std::uint32_t generation{0};
  };

  struct SessionSlots final {
    explicit SessionSlots(const std::uint32_t assigned_generation) noexcept
        : generation(assigned_generation) {}

    std::array<PaneSlot, panes_per_session_max> panes{};
    std::size_t size{0};
    std::uint32_t generation{0};
  };

  [[nodiscard]] static constexpr auto address_in_bounds(const PaneAddress address) noexcept
      -> bool {
    return address.valid() && address.session.slot() < limits::sessions_hard_max &&
           address.pane.slot() < panes_per_session_max;
  }

  void release_scrollback(const std::size_t bytes) noexcept {
    LEMMA_ASSERT(scrollback_bytes_reserved_ >= bytes);
    scrollback_bytes_reserved_ -= bytes;
  }

public:
  [[nodiscard]] auto insert(PaneAddress address, std::unique_ptr<PaneRuntime> runtime) noexcept
      -> bool;
  [[nodiscard]] auto get(PaneAddress address) noexcept -> PaneRuntime*;
  [[nodiscard]] auto get(PaneAddress address) const noexcept -> const PaneRuntime*;
  [[nodiscard]] auto erase(PaneAddress address) noexcept -> bool;
  void erase_session(SessionId session_id) noexcept;

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto scrollback_bytes_reserved() const noexcept -> std::size_t {
    return scrollback_bytes_reserved_;
  }
  [[nodiscard]] auto can_reserve_scrollback(const std::size_t bytes) const noexcept -> bool {
    return bytes <= limits::terminal_scrollback_bytes_aggregate_max - scrollback_bytes_reserved_;
  }
  // Daemon-wide monotonic stamps order Pane signal changes, so an observer retains one watermark
  // and scans only Sessions and Panes stamped after it.
  [[nodiscard]] auto issue_signal_stamp() noexcept -> std::uint64_t { return ++signal_clock_; }
  [[nodiscard]] auto signal_clock() const noexcept -> std::uint64_t { return signal_clock_; }

private:
  std::array<std::unique_ptr<SessionSlots>, limits::sessions_hard_max> sessions_{};
  std::size_t size_{0};
  std::size_t scrollback_bytes_reserved_{0};
  std::uint64_t signal_clock_{0};
};

struct CopySearchTask final {
  vt::SearchCursor cursor;
  vt::TerminalPoint stop_before;
  std::chrono::steady_clock::time_point deadline;
  CopySearchDirection direction{CopySearchDirection::forward};
  std::uint64_t terminal_generation{0};
  bool wrapped{false};
};

struct CopyModeRuntimeState final {
  std::array<std::byte, copy_escape_bytes_max> pending_escape{};
  std::size_t pending_escape_size{0};
  std::optional<CopySearchTask> search_task;
  std::optional<vt::SearchMatch> last_search_match;
  std::optional<std::uint64_t> search_restore_viewport_offset;
  std::chrono::steady_clock::time_point pending_escape_deadline;
  std::uint64_t last_search_generation{0};
  bool preview_match{false};
};

// Runtime-sized bounded clipboard staging cannot use std::array.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
using ClipboardStorage = std::unique_ptr<std::byte[]>;

struct PendingClipboardWrite final {
  enum class Format : std::uint8_t { selection, osc52, kitty };
  ClipboardStorage bytes;
  std::size_t size{0};
  std::size_t offset{0};
  Format format{Format::selection};
  bool interleave_frame{false};
  bool redraw_after_write{false};

  void reset() noexcept {
    bytes.reset();
    size = 0;
    offset = 0;
    format = Format::selection;
    interleave_frame = false;
    redraw_after_write = false;
  }
};

struct ClipboardPaste final {
  SessionId session;
  PaneId pane;
  ConnectionId connection;
  std::unique_ptr<clipboard::Transaction> reply;
  std::unique_ptr<clipboard::PngFile> file;
};

struct AttachmentRuntime final {
  AttachmentRuntime() noexcept = default;
  AttachmentRuntime(const AttachmentRuntime&) = delete;
  auto operator=(const AttachmentRuntime&) -> AttachmentRuntime& = delete;
  AttachmentRuntime(AttachmentRuntime&&) = delete;
  auto operator=(AttachmentRuntime&&) -> AttachmentRuntime& = delete;
  ~AttachmentRuntime();

  void reset_connection() noexcept;

  render::FrameBuffer frame;
  render::GraphicsProjection graphics;
  protocol::CellSize cell_size;
  // Newest client geometry not yet applied. Consecutive geometry messages supersede one another;
  // it is applied before the next non-geometry client message, so input still follows it.
  struct PendingGeometry final {
    std::optional<protocol::CellSize> cell_size;
    std::optional<protocol::Dimensions> dimensions;
    [[nodiscard]] auto pending() const noexcept -> bool {
      return cell_size.has_value() || dimensions.has_value();
    }
  };
  PendingGeometry pending_geometry;
  protocol::ClientDecoder decoder;
  ClientFrameOutput output;
  PendingClipboardWrite clipboard_write;
  std::unique_ptr<clipboard::Transaction> clipboard;
  std::optional<PaneId> clipboard_owner;
  std::optional<bool> kitty_clipboard_supported;
  // OSC 52 has no reply IDs. An abandoned read retires this channel for the connection.
  bool osc52_read_retired{false};
  std::weak_ptr<ClipboardPaste> clipboard_paste;
  std::shared_ptr<ClipboardPaste> interactive_paste;
  CopyModeRuntimeState copy_mode;
  FrameScheduler frame_scheduler;
  ConnectionId connection_id;
  struct PendingHostedCommand final {
    extension::InvocationContext context;
    std::uint8_t index{0};
  };
  std::optional<PendingHostedCommand> hosted_command;
  std::uint32_t server_sequence{2};
  std::uint32_t full_redraw_generation{0};
  std::uint32_t pending_attach_slot{std::numeric_limits<std::uint32_t>::max()};
  std::uint32_t pending_attach_generation{0};
  std::uint64_t status_signature{0};
  std::optional<render::OuterModeProjection> outer_modes;
  // Last title presented through OSC 2; the client restores the user's title on exit.
  std::array<char, limits::outer_title_bytes_max> outer_title{};
  std::size_t outer_title_size{0};
  bool outer_title_presented{false};
  int client{-1};
  bool status_valid{false};
  // Outer-terminal focus from mode 1004 reports. A new connection assumes the user is typing into
  // it.
  bool outer_focused{true};
  bool bell_pending{false};
  bool input_backpressured{false};
  bool client_work_pending{false};
  ConnectionCloseState client_close_state{ConnectionCloseState::none};
  protocol::DisconnectReason client_close_reason{protocol::DisconnectReason::protocol_error};
  std::optional<std::size_t> retained_input_offset;
  struct SurfacePasteProgress final {
    SurfaceId surface;
    std::size_t offset{0};
    std::optional<std::chrono::steady_clock::time_point> deadline;
  };
  std::optional<SurfacePasteProgress> surface_paste;
  std::optional<std::chrono::steady_clock::time_point> status_message_deadline;
  std::array<std::byte, input::deferred_input_bytes_max + 1U> pending_routed_input{};
  std::uint8_t pending_routed_input_size{0};
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  diagnostic::LatencyTraceMarkerMatcher decoded_input_trace_matcher;
  std::uint64_t frame_trace_correlation{0};
#endif
};

struct SessionRecord final : Session {
  SessionRecord(std::string_view session_name, std::string_view initial_working_directory,
                std::span<const std::byte> initial_environment,
                LaunchEnvironmentMode initial_environment_mode) noexcept;
  SessionRecord(const SessionRecord&) = delete;
  auto operator=(const SessionRecord&) -> SessionRecord& = delete;
  SessionRecord(SessionRecord&&) = delete;
  auto operator=(SessionRecord&&) -> SessionRecord& = delete;
  ~SessionRecord() = default;

  input::InputRouter input_router;
  input::InputRouter interaction_router;
  AttachmentRuntime attachment_runtime;
  vt::TerminalTheme theme;
  // Newest signal stamp of any Pane in this Session; lets observers skip unchanged Sessions.
  std::uint64_t signal_stamp{0};
};

class Sessions final {
  using Store = BoundedGenerationalStore<SessionRecord, SessionId, limits::sessions_hard_max>;

public:
  [[nodiscard]] auto insert(std::unique_ptr<SessionRecord> session) noexcept
      -> std::optional<SessionId> {
    if (session != nullptr) {
      session->attachment_runtime.frame.bind_capacity_budget(frame_capacity_budget_);
    }
    return sessions_.insert(std::move(session));
  }
  [[nodiscard]] auto get(const SessionId id) noexcept -> SessionRecord* {
    return sessions_.get(id);
  }
  [[nodiscard]] auto get(const SessionId id) const noexcept -> const SessionRecord* {
    return sessions_.get(id);
  }
  [[nodiscard]] auto connection_available(const SessionId id) const noexcept -> bool {
    // A live Session proves the slot is within the generation array.
    return get(id) != nullptr && std::span(connection_generations_).subspan(id.slot(), 1).front() <
                                     std::numeric_limits<std::uint32_t>::max();
  }
  [[nodiscard]] auto allocate_connection(const SessionId id) noexcept -> ConnectionId {
    LEMMA_ASSERT(connection_available(id));
    return ConnectionId::from_parts(
        id.slot(), ++std::span(connection_generations_).subspan(id.slot(), 1).front());
  }
  [[nodiscard]] auto erase(const SessionId id) noexcept -> bool { return sessions_.erase(id); }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return sessions_.size(); }
  [[nodiscard]] static constexpr auto capacity() noexcept -> std::size_t {
    return Store::capacity();
  }
  [[nodiscard]] auto begin() noexcept { return sessions_.begin(); }
  [[nodiscard]] auto end() noexcept { return sessions_.end(); }
  [[nodiscard]] auto begin() const noexcept { return sessions_.begin(); }
  [[nodiscard]] auto end() const noexcept { return sessions_.end(); }

private:
  render::FrameCapacityBudget frame_capacity_budget_;
  Store sessions_;
  // Connection identities must survive Session slot reuse, not just detach/reattach.
  std::array<std::uint32_t, limits::sessions_hard_max> connection_generations_{};
};

static_assert(sizeof(PaneRuntimeStore) <= std::size_t{4} * 1'024U);
static_assert(sizeof(AttachmentRuntime) <= std::size_t{16} * 1'024U);
static_assert(sizeof(SessionRecord) <= std::size_t{96} * 1'024U);

[[nodiscard]] auto reactor_now() noexcept -> std::chrono::steady_clock::time_point;
[[nodiscard]] auto reactor_input_map() noexcept -> const input::CompiledInputMap&;
[[nodiscard]] auto reactor_status_line() noexcept -> bool;
[[nodiscard]] auto reactor_outer_title() noexcept -> bool;

[[nodiscard]] inline auto pane_rows(const std::uint16_t viewport_rows) noexcept -> std::uint16_t {
  return viewport_rows;
}

[[nodiscard]] inline auto find_pane(SessionRecord& session, const PaneId id) noexcept -> Pane* {
  if (!id.is_valid() || id.slot() >= session.panes.size()) {
    return nullptr;
  }
  auto& slot = std::span(session.panes).subspan(id.slot(), 1).front();
  return slot.generation == id.generation() ? slot.pane.get() : nullptr;
}

[[nodiscard]] inline auto find_pane(const SessionRecord& session, const PaneId id) noexcept
    -> const Pane* {
  if (!id.is_valid() || id.slot() >= session.panes.size()) {
    return nullptr;
  }
  const auto& slot = std::span(session.panes).subspan(id.slot(), 1).front();
  return slot.generation == id.generation() ? slot.pane.get() : nullptr;
}

[[nodiscard]] inline auto find_pane(SessionRecord& session, const Tab& tab,
                                    const PaneId id) noexcept -> Pane* {
  auto* const pane = find_pane(session, id);
  return pane != nullptr && pane->tab == tab.id ? pane : nullptr;
}

[[nodiscard]] inline auto find_pane(const SessionRecord& session, const Tab& tab,
                                    const PaneId id) noexcept -> const Pane* {
  const auto* const pane = find_pane(session, id);
  return pane != nullptr && pane->tab == tab.id ? pane : nullptr;
}

[[nodiscard]] inline auto find_tab(SessionRecord& session, const TabId id) noexcept -> Tab* {
  if (!id.is_valid() || id.slot() >= session.tabs.size()) {
    return nullptr;
  }
  auto& slot = std::span(session.tabs).subspan(id.slot(), 1).front();
  return slot.generation == id.generation() ? slot.tab.get() : nullptr;
}

[[nodiscard]] inline auto find_tab(const SessionRecord& session, const TabId id) noexcept
    -> const Tab* {
  if (!id.is_valid() || id.slot() >= session.tabs.size()) {
    return nullptr;
  }
  const auto& slot = std::span(session.tabs).subspan(id.slot(), 1).front();
  return slot.generation == id.generation() ? slot.tab.get() : nullptr;
}

[[nodiscard]] inline auto active_tab(SessionRecord& session) noexcept -> Tab* {
  return find_tab(session, session.active_tab);
}

[[nodiscard]] inline auto active_tab(const SessionRecord& session) noexcept -> const Tab* {
  return find_tab(session, session.active_tab);
}

[[nodiscard]] constexpr auto pane_address(const SessionRecord& session, const Pane& pane) noexcept
    -> PaneAddress {
  return {.session = session.id, .pane = pane.id};
}

[[nodiscard]] inline auto find_pane_runtime(PaneRuntimeStore& runtimes,
                                            const SessionRecord& session, const Pane& pane) noexcept
    -> PaneRuntime* {
  return runtimes.get(pane_address(session, pane));
}

[[nodiscard]] inline auto find_pane_runtime(const PaneRuntimeStore& runtimes,
                                            const SessionRecord& session, const Pane& pane) noexcept
    -> const PaneRuntime* {
  return runtimes.get(pane_address(session, pane));
}

[[nodiscard]] inline auto find_pane_runtime(PaneRuntimeStore& runtimes,
                                            const SessionRecord& session, const Tab& tab,
                                            const Pane& pane) noexcept -> PaneRuntime* {
  LEMMA_ASSERT(pane.tab == tab.id);
  return find_pane_runtime(runtimes, session, pane);
}

[[nodiscard]] inline auto find_pane_runtime(const PaneRuntimeStore& runtimes,
                                            const SessionRecord& session, const Tab& tab,
                                            const Pane& pane) noexcept -> const PaneRuntime* {
  LEMMA_ASSERT(pane.tab == tab.id);
  return find_pane_runtime(runtimes, session, pane);
}

[[nodiscard]] constexpr auto pane_count(const Tab& tab) noexcept -> std::size_t {
  return tab.layout.pane_count();
}

[[nodiscard]] inline auto pane_count(const SessionRecord& session) noexcept -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(
      session.panes, [](const PaneSlot& slot) { return slot.pane != nullptr; }));
}

[[nodiscard]] constexpr auto tab_count(const SessionRecord& session) noexcept -> std::size_t {
  return session.tab_order.size();
}

[[nodiscard]] inline auto tab_at_position(SessionRecord& session,
                                          const std::size_t position) noexcept -> Tab* {
  const auto id = session.tab_order.at(position);
  return id.has_value() ? find_tab(session, *id) : nullptr;
}
[[nodiscard]] auto find_session(Sessions& sessions, std::string_view name) noexcept
    -> SessionRecord*;
[[nodiscard]] auto copy_mode_runtime(const SessionRecord& session,
                                     const PaneRuntimeStore& runtimes) noexcept
    -> const PaneRuntime*;

} // namespace lemma::core::engine_detail

#endif // LEMMA_CORE_ENGINE_STATE_HPP
