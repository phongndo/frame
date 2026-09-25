#include "core/engine.hpp"
#include "clipboard/png_file.hpp"
#include "clipboard/transaction.hpp"
#include "config/config.hpp"

#include "core/engine_connection_state.hpp"
#include "core/engine_projection.hpp"
#include "core/engine_state.hpp"

#include "extension/commands.hpp"
#include "extension/protocol.hpp"
#include "extension/runtime.hpp"

#include "api/command.hpp"
#include "api/json.hpp"
#include "api/proc.hpp"
#include "core/client_frame_output.hpp"
#include "core/command_history.hpp"
#include "core/command_line.hpp"
#include "core/connection_output.hpp"
#include "core/copy_mode.hpp"
#include "core/frame_scheduler.hpp"
#include "core/input.hpp"
#include "core/layout.hpp"
#include "core/presentation_gate.hpp"
#include "core/pty_writer.hpp"
#include "core/session.hpp"
#include "core/session_machine.hpp"
#include "core/terminal_resize.hpp"
#include "diagnostic/latency_trace.hpp"
#include "input/input_router.hpp"
#include "lemma/assert.hpp"
#include "lemma/command.hpp"
#include "lemma/geometry.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"
#include "platform/io.hpp"
#include "platform/pty.hpp"
#include "platform/readiness.hpp"
#include "protocol/attachment.hpp"
#include "render/frame_buffer.hpp"
#include "render/scene.hpp"
#include "render/status_line.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal> // IWYU pragma: keep -- owns SIGHUP on POSIX Clang
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lemma::core {
namespace engine_detail {

constexpr auto command_create = protocol::wire_byte(protocol::ControlCommand::create);
constexpr auto command_create_with_context =
    protocol::wire_byte(protocol::ControlCommand::create_with_context);
constexpr auto command_create_auto_with_context =
    protocol::wire_byte(protocol::ControlCommand::create_auto_with_context);
constexpr auto command_list = protocol::wire_byte(protocol::ControlCommand::list);
constexpr auto command_list_session = protocol::wire_byte(protocol::ControlCommand::list_session);
constexpr auto command_list_tabs = protocol::wire_byte(protocol::ControlCommand::list_tabs);
constexpr auto command_list_panes = protocol::wire_byte(protocol::ControlCommand::list_panes);
constexpr auto command_query_sessions =
    protocol::wire_byte(protocol::ControlCommand::query_sessions);
constexpr auto command_query_session = protocol::wire_byte(protocol::ControlCommand::query_session);
constexpr auto command_query_tabs = protocol::wire_byte(protocol::ControlCommand::query_tabs);
constexpr auto command_query_panes = protocol::wire_byte(protocol::ControlCommand::query_panes);
constexpr auto command_rename_session =
    protocol::wire_byte(protocol::ControlCommand::rename_session);
constexpr auto command_rename_tab = protocol::wire_byte(protocol::ControlCommand::rename_tab);
constexpr auto command_kill = protocol::wire_byte(protocol::ControlCommand::kill);
constexpr auto command_surface_create =
    protocol::wire_byte(protocol::ControlCommand::surface_create);
constexpr auto command_semantic_action =
    protocol::wire_byte(protocol::ControlCommand::semantic_action);
constexpr auto command_send_pane = protocol::wire_byte(protocol::ControlCommand::send_pane);
constexpr auto command_capture_pane = protocol::wire_byte(protocol::ControlCommand::capture_pane);
constexpr auto command_pane_status = protocol::wire_byte(protocol::ControlCommand::pane_status);
constexpr auto command_kill_all = protocol::wire_byte(protocol::ControlCommand::kill_all);
constexpr auto command_shutdown = protocol::wire_byte(protocol::ControlCommand::shutdown);
constexpr auto response_ready = protocol::wire_byte(protocol::ControlResponse::ready);
constexpr auto response_no_effect = protocol::wire_byte(protocol::ControlResponse::no_effect);
constexpr auto response_unavailable = protocol::wire_byte(protocol::ControlResponse::unavailable);
constexpr auto response_missing = protocol::wire_byte(protocol::ControlResponse::missing);
constexpr auto response_capacity = protocol::wire_byte(protocol::ControlResponse::capacity);
constexpr auto response_conflict = protocol::wire_byte(protocol::ControlResponse::conflict);
constexpr auto response_failed = protocol::wire_byte(protocol::ControlResponse::failed);
constexpr auto process_name_refresh_interval = std::chrono::milliseconds{100};
constexpr auto copy_escape_flush_delay = std::chrono::milliseconds{50};
constexpr auto copy_search_slice_delay = std::chrono::milliseconds{2};
constexpr auto scrollback_compression_slice_delay = std::chrono::milliseconds{2};
constexpr auto mouse_repeat_click_interval = std::chrono::milliseconds{500};
// SGR wheel reports are already normalized by the outer terminal. Applying another multiplier
// here makes Ghostty's default three-report discrete wheel step move nine rows.
constexpr std::int64_t mouse_wheel_scroll_rows = 1;
constexpr double mouse_repeat_click_distance = 1.0;
// Decoder capacity is a storage bound, not a CPU bound. Every session gets a fresh bounded slice
// each reactor turn; geometry work is tighter because one message can reflow every pane in a tab.
constexpr std::size_t client_messages_per_turn_max = 16;
constexpr std::size_t client_geometry_messages_per_turn_max = 1;
constexpr std::size_t client_input_steps_per_turn_max = 16;
static_assert(client_messages_per_turn_max > 0 && client_geometry_messages_per_turn_max > 0 &&
              client_input_steps_per_turn_max > 0);
static_assert(tabs_per_session_max <= render::status_tabs_max);
using platform::close_descriptor;
using platform::set_nonblocking;
using render::FrameBuffer;

struct ReloadResult final {
  std::string diagnostic;
  ConfigurationReloadError error{ConfigurationReloadError::invalid_configuration};
  bool complete{false};
  bool success{false};
};

struct ReloadState final {
  std::weak_ptr<ReloadResult> current;
  std::shared_ptr<ReloadResult> interactive;
  ConnectionId connection;
};

thread_local const ReactorEnvironment* active_reactor_environment = nullptr;
thread_local ReloadState* active_reload = nullptr;
thread_local CommandLineHistory* active_command_history = nullptr;

[[nodiscard]] auto start_reload() -> std::shared_ptr<ReloadResult> {
  auto* const owner = active_reactor_environment->configuration_reloader;
  if (owner == nullptr || owner->descriptor() >= 0) {
    return {};
  }
  auto result = std::make_shared<ReloadResult>();
  if (!owner->start()) {
    return {};
  }
  active_reload->current = result;
  return result;
}

class ReactorEnvironmentGuard final {
public:
  explicit ReactorEnvironmentGuard(const ReactorEnvironment& environment,
                                   ReloadState& reload) noexcept
      : previous_(active_reactor_environment), previous_reload_(active_reload) {
    active_reload = &reload;
    LEMMA_ASSERT(environment.valid());
    active_reactor_environment = &environment;
  }

  ReactorEnvironmentGuard(const ReactorEnvironmentGuard&) = delete;
  ReactorEnvironmentGuard(ReactorEnvironmentGuard&&) = delete;
  auto operator=(const ReactorEnvironmentGuard&) -> ReactorEnvironmentGuard& = delete;
  auto operator=(ReactorEnvironmentGuard&&) -> ReactorEnvironmentGuard& = delete;

  ~ReactorEnvironmentGuard() {
    active_reactor_environment = previous_;
    active_reload = previous_reload_;
  }

private:
  const ReactorEnvironment* previous_;
  ReloadState* previous_reload_;
};

class CommandHistoryGuard final {
public:
  explicit CommandHistoryGuard(const std::string_view path) noexcept
      : previous_(active_command_history), path_size_(path.size()) {
    LEMMA_ASSERT(path.size() <= path_.size());
    std::ranges::copy(path, path_.begin());
    const auto loaded = load_command_line_history(path);
    history_ = loaded.history;
    replace_on_shutdown_ = loaded.replace_on_shutdown;
    active_command_history = path.empty() ? nullptr : &history_;
  }

  CommandHistoryGuard(const CommandHistoryGuard&) = delete;
  CommandHistoryGuard(CommandHistoryGuard&&) = delete;
  auto operator=(const CommandHistoryGuard&) -> CommandHistoryGuard& = delete;
  auto operator=(CommandHistoryGuard&&) -> CommandHistoryGuard& = delete;

  ~CommandHistoryGuard() {
    if (replace_on_shutdown_) {
      static_cast<void>(
          save_command_line_history(std::string_view(path_.data(), path_size_), history_));
    }
    active_command_history = previous_;
  }

private:
  CommandLineHistory* previous_{nullptr};
  std::array<char, config::configuration_path_bytes_max> path_{};
  std::size_t path_size_{0};
  CommandLineHistory history_;
  bool replace_on_shutdown_{false};
};

[[nodiscard]] auto reactor_now() noexcept -> ReactorClock::time_point {
  LEMMA_ASSERT(active_reactor_environment != nullptr);
  return active_reactor_environment->now(active_reactor_environment->context);
}

[[nodiscard]] auto reactor_input_map() noexcept -> const input::CompiledInputMap& {
  LEMMA_ASSERT(active_reactor_environment != nullptr);
  return active_reactor_environment->input_map == nullptr ? input::default_input_map()
                                                          : *active_reactor_environment->input_map;
}

[[nodiscard]] auto reactor_scrollback_lines() noexcept -> std::optional<std::size_t> {
  LEMMA_ASSERT(active_reactor_environment != nullptr);
  return active_reactor_environment->scrollback_lines;
}

[[nodiscard]] auto reactor_default_program() noexcept -> std::span<const std::byte> {
  LEMMA_ASSERT(active_reactor_environment != nullptr);
  return active_reactor_environment->default_program;
}

[[nodiscard]] auto reactor_default_cwd() noexcept -> std::string_view {
  LEMMA_ASSERT(active_reactor_environment != nullptr);
  return active_reactor_environment->default_cwd;
}

[[nodiscard]] auto reactor_status_line() noexcept -> bool {
  LEMMA_ASSERT(active_reactor_environment != nullptr);
  return active_reactor_environment->status_line;
}

[[nodiscard]] auto reactor_poll(const std::span<pollfd> descriptors,
                                const int timeout_milliseconds) noexcept -> int {
  LEMMA_ASSERT(active_reactor_environment != nullptr);
  return active_reactor_environment->poll(active_reactor_environment->context, descriptors,
                                          timeout_milliseconds);
}

[[nodiscard]] auto reactor_send(const int descriptor, const std::span<const std::byte> bytes,
                                const int flags) noexcept -> ReactorIoResult {
  LEMMA_ASSERT(active_reactor_environment != nullptr);
  return active_reactor_environment->send(active_reactor_environment->context, descriptor, bytes,
                                          flags);
}

[[nodiscard]] auto production_poll([[maybe_unused]] void* context,
                                   const std::span<pollfd> descriptors,
                                   const int timeout_milliseconds) noexcept -> int {
  return ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), timeout_milliseconds);
}

[[nodiscard]] auto production_now([[maybe_unused]] void* context) noexcept
    -> ReactorClock::time_point {
  return ReactorClock::now();
}

[[nodiscard]] auto production_send([[maybe_unused]] void* context, const int descriptor,
                                   const std::span<const std::byte> bytes, const int flags) noexcept
    -> ReactorIoResult {
  const auto sent = ::send(descriptor, bytes.data(), bytes.size(), flags);
  return {.bytes = sent, .error = sent < 0 ? errno : 0};
}

class EndpointReleaseGuard final {
public:
  EndpointReleaseGuard(const EndpointRelease release, void* const context) noexcept
      : release_(release), context_(context) {}

  EndpointReleaseGuard(const EndpointReleaseGuard&) = delete;
  auto operator=(const EndpointReleaseGuard&) -> EndpointReleaseGuard& = delete;
  EndpointReleaseGuard(EndpointReleaseGuard&&) = delete;
  auto operator=(EndpointReleaseGuard&&) -> EndpointReleaseGuard& = delete;

  ~EndpointReleaseGuard() { release(); }

  void release() noexcept {
    if (release_ == nullptr) {
      return;
    }
    release_(context_);
    release_ = nullptr;
  }

private:
  EndpointRelease release_;
  void* context_;
};

[[nodiscard]] auto resize_pty_for_transaction(void* const context,
                                              const vt::TerminalSize& size) noexcept -> bool {
  const auto pty = *static_cast<const int*>(context);
  return pty < 0 || platform::resize_pty(pty, size.columns, size.rows, size.cell_width_px,
                                         size.cell_height_px);
}

[[nodiscard]] auto resize_pane_terminal(const int pty, vt::Terminal& terminal,
                                        const std::uint16_t requested_columns,
                                        const std::uint16_t requested_rows) noexcept
    -> TerminalResizeStatus {
  const auto columns = std::clamp(requested_columns, std::uint16_t{1}, protocol::columns_max);
  const auto rows = std::clamp(requested_rows, std::uint16_t{1}, protocol::rows_max);
  const auto previous = terminal.size();
  const vt::TerminalSize requested{
      .columns = columns,
      .rows = rows,
      .cell_width_px = previous.cell_width_px,
      .cell_height_px = previous.cell_height_px,
  };
  auto descriptor = pty;
  return resize_terminal_transaction(terminal, requested, &resize_pty_for_transaction, &descriptor);
}

struct PtyDrainResult final {
  std::optional<PaneRuntimeFailure> failure;
  bool changed{false};
  bool render_damage{false};
  bool presentation_deferred{false};
  bool presentation_released{false};
  bool force_full{false};
  bool damage_capture_failed{false};
  bool bell{false};
  bool title_changed{false};
  bool signal{false};
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  std::uint64_t correlation{0};
#endif
};

[[nodiscard]] auto
trace_pty_output([[maybe_unused]] PtyDrainResult& drain,
                 [[maybe_unused]] diagnostic::LatencyTraceMarkerMatcher* const trace_matcher,
                 [[maybe_unused]] const std::span<const std::byte> bytes) noexcept
    -> std::uint64_t {
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  LEMMA_ASSERT(trace_matcher != nullptr);
  const auto correlation = trace_matcher->observe(bytes);
  if (correlation != 0) {
    drain.correlation = correlation;
  }
  return correlation;
#else
  return 0;
#endif
}

void write_pty_output(vt::Terminal& terminal, PresentationGate& presentation_gate,
                      const std::span<const std::byte> bytes, bool& capture_damage,
                      PtyDrainResult& drain) noexcept {
  bool damage_acquired = true;
  if (!capture_damage) {
    terminal.write(bytes);
  } else {
    const auto damage = terminal.write_and_report_damage(bytes);
    if (!damage.has_value()) {
      drain.damage_capture_failed = true;
      capture_damage = false;
    } else {
      damage_acquired = *damage != vt::DirtyState::clean;
      if (damage_acquired) {
        capture_damage = false;
      }
    }
  }

  const auto synchronized = terminal.synchronized_output();
  if (!synchronized.has_value()) {
    drain.damage_capture_failed = true;
    return;
  }
  const auto gate = presentation_gate.observe(*synchronized, damage_acquired, reactor_now());
  drain.render_damage = drain.render_damage || gate.visible_damage;
  drain.presentation_deferred = presentation_gate.presentation_suppressed();
  drain.presentation_released = drain.presentation_released || gate.urgent_render;
  drain.force_full = drain.force_full || gate.force_full;
}

[[nodiscard]] auto
process_pty_output(const int pty, vt::Terminal& terminal, PresentationGate& presentation_gate,
                   PanePtyWriteQueue& pending_writes, const std::span<const std::byte> bytes,
                   bool& capture_damage, PtyDrainResult& drain,
                   diagnostic::LatencyTraceMarkerMatcher* const trace_matcher) noexcept -> bool {
  const auto trace_correlation = trace_pty_output(drain, trace_matcher, bytes);
  diagnostic::record_latency_trace(diagnostic::LatencyTraceStage::daemon_pty_output_read,
                                   static_cast<std::uint32_t>(pty), bytes.size(),
                                   trace_correlation);
  write_pty_output(terminal, presentation_gate, bytes, capture_damage, drain);
  const auto effects = terminal.take_effects();
  // Desktop notifications use the same bounded visible-attention policy as BEL. Title, PWD, and
  // progress changes invalidate status metadata; denied clipboard and unknown sequences are
  // intentionally drained and dropped by policy at the adapter boundary.
  drain.bell = drain.bell || effects.bells > 0 || effects.desktop_notifications > 0;
  drain.title_changed = drain.title_changed || effects.title_changes > 0 ||
                        effects.pwd_changes > 0 || effects.progress_reports > 0;
  // Signal values stay in the terminal's latest-value record; the drain only notes a change.
  drain.signal = drain.signal || effects.signal_changes > 0;
  drain.changed = true;
  return queue_terminal_responses(pending_writes, terminal);
}

[[nodiscard]] auto
drain_pty(const int pty, vt::Terminal& terminal, PresentationGate& presentation_gate,
          PanePtyWriteQueue& pending_writes, std::size_t& global_budget,
          const bool capture_interactive_damage,
          [[maybe_unused]] diagnostic::LatencyTraceMarkerMatcher* const trace_matcher) noexcept
    -> PtyDrainResult {
  constexpr std::size_t reads_per_turn_max = 4;
  // read() initializes the returned prefix; no consumer observes the unused capacity.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  std::array<std::byte, std::size_t{64} * 1'024U> output;
  PtyDrainResult drain{};
  bool capture_damage = capture_interactive_damage;
  for (std::size_t read_count = 0; read_count < reads_per_turn_max && global_budget > 0;
       ++read_count) {
    const auto available = std::min(output.size(), global_budget);
    const auto bytes_read = ::read(pty, output.data(), available);
    if (bytes_read > 0) {
      const auto size = static_cast<std::size_t>(bytes_read);
      global_budget -= size;
      const auto bytes = std::span(output).first(size);
      if (!process_pty_output(pty, terminal, presentation_gate, pending_writes, bytes,
                              capture_damage, drain, trace_matcher)) {
        drain.failure = PaneRuntimeFailure::terminal_integrity_error;
        return drain;
      }
      continue;
    }
    // Linux PTY masters report EIO instead of a zero-byte EOF after the final slave closes.
    if (bytes_read == 0 || errno == EIO) {
      drain.failure = PaneRuntimeFailure::child_exit;
      return drain;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      drain.failure = PaneRuntimeFailure::pty_read_error;
    }
    return drain;
  }
  return drain;
}

[[nodiscard]] constexpr auto platform_environment_mode(const LaunchEnvironmentMode mode) noexcept
    -> platform::EnvironmentMode {
  return mode == LaunchEnvironmentMode::inherit ? platform::EnvironmentMode::inherit
                                                : platform::EnvironmentMode::replace;
}

// Environment framing validation is exhaustive and bounded by entry/payload limits.
[[nodiscard]] auto valid_environment(const std::span<const std::byte> environment) noexcept
    -> bool {
  if (environment.empty()) {
    return true;
  }
  std::size_t offset = 0;
  std::size_t entries = 0;
  while (offset < environment.size()) {
    const auto remaining = environment.subspan(offset);
    const auto terminator = std::ranges::find(remaining, std::byte{0});
    if (terminator == remaining.end()) {
      return false;
    }
    const auto entry_size = static_cast<std::size_t>(std::distance(remaining.begin(), terminator));
    const auto entry = remaining.first(entry_size);
    const auto separator = std::ranges::find(entry, std::byte{'='});
    if (separator == entry.begin() || separator == entry.end()) {
      return false;
    }
    ++entries;
    if (entries > protocol::environment_entries_max) {
      return false;
    }
    offset += entry_size + 1U;
  }
  return true;
}

[[nodiscard]] auto valid_launch_command(const std::span<const std::byte> command) noexcept -> bool {
  if (command.empty() || command.size() > protocol::command_bytes_max) {
    return command.empty();
  }
  std::size_t offset = 0;
  std::size_t arguments = 0;
  while (offset < command.size()) {
    const auto remaining = command.subspan(offset);
    const auto terminator = std::ranges::find(remaining, std::byte{0});
    if (terminator == remaining.end() || (arguments == 0 && terminator == remaining.begin())) {
      return false;
    }
    ++arguments;
    if (arguments > protocol::command_arguments_max) {
      return false;
    }
    offset += static_cast<std::size_t>(std::distance(remaining.begin(), terminator)) + 1U;
  }
  return true;
}

[[nodiscard]] constexpr auto valid_session_name(const std::string_view session) noexcept -> bool {
  if (session.empty() || session.front() == '-' ||
      session.size() > protocol::session_name_bytes_max) {
    return false;
  }
  return std::ranges::all_of(session, [](const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
  });
}

void record_terminal_observation(PaneRuntime& runtime) noexcept {
  runtime.observation_generation =
      runtime.observation_generation == std::numeric_limits<std::uint64_t>::max()
          ? std::uint64_t{1}
          : runtime.observation_generation + 1U;
}

void record_terminal_mutation(PaneRuntime& runtime) noexcept {
  runtime.mutation_generation =
      runtime.mutation_generation == std::numeric_limits<std::uint64_t>::max()
          ? std::uint64_t{1}
          : runtime.mutation_generation + 1U;
  record_terminal_observation(runtime);
}

[[nodiscard]] constexpr auto terminal_search_direction(const CopySearchDirection direction) noexcept
    -> vt::SearchDirection {
  return direction == CopySearchDirection::forward ? vt::SearchDirection::forward
                                                   : vt::SearchDirection::backward;
}

// Runtime-sized bounded clipboard staging cannot use std::array.
[[nodiscard]] auto allocate_clipboard_storage(const std::size_t bytes) noexcept
    -> ClipboardStorage {
  try {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    return std::make_unique_for_overwrite<std::byte[]>(bytes);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

void record_session_mutation(SessionRecord& session) noexcept {
  session.mutation_generation =
      session.mutation_generation == std::numeric_limits<std::uint64_t>::max()
          ? std::uint64_t{1}
          : session.mutation_generation + 1U;
}

void finish_live_divider_resize(SessionRecord& session, bool discard_release = false) noexcept;

void schedule_frame(SessionRecord& session, FrameUrgency urgency, bool force_full,
                    PaneId source) noexcept;

auto apply_cell_size(SessionRecord& session, PaneRuntimeStore& runtimes,
                     const protocol::CellSize size) noexcept -> bool {
  session.attachment_runtime.cell_size = size;
  bool changed = false;
  for (const auto& slot : session.panes) {
    if (slot.pane == nullptr) {
      continue;
    }
    auto* runtime = find_pane_runtime(runtimes, session, *slot.pane);
    LEMMA_ASSERT(runtime != nullptr);
    auto requested = runtime->terminal.size();
    if (requested.cell_width_px == size.width && requested.cell_height_px == size.height) {
      continue;
    }
    requested.cell_width_px = size.width;
    requested.cell_height_px = size.height;
    auto descriptor = runtime->pty;
    const auto result = resize_terminal_transaction(runtime->terminal, requested,
                                                    resize_pty_for_transaction, &descriptor);
    if (result != TerminalResizeStatus::applied && result != TerminalResizeStatus::unchanged) {
      return false;
    }
    record_terminal_mutation(*runtime);
    changed = true;
  }
  if (changed) {
    schedule_frame(session, FrameUrgency::state_change, true, {});
  }
  return true;
}

auto begin_clipboard_transaction(SessionRecord& session, const PaneId pane,
                                 const vt::ClipboardRequest& request) noexcept
    -> std::expected<void, vt::ClipboardStatus> {
  auto& attachment = session.attachment_runtime;
  if ((request.protocol == vt::ClipboardProtocol::kitty &&
       attachment.kitty_clipboard_supported == false) ||
      (request.protocol == vt::ClipboardProtocol::osc52 && request.read &&
       attachment.osc52_read_retired)) {
    return std::unexpected(vt::ClipboardStatus::unsupported);
  }
  if (attachment.clipboard != nullptr || attachment.clipboard_write.bytes != nullptr ||
      !attachment.clipboard_paste.expired()) {
    return std::unexpected(vt::ClipboardStatus::busy);
  }
  try {
    auto transaction = std::make_unique<clipboard::Transaction>();
    auto encoded = transaction->begin(request, reactor_now());
    if (!encoded) {
      return std::unexpected(encoded.error());
    }
    auto storage = allocate_clipboard_storage(encoded->size());
    if (storage == nullptr) {
      return std::unexpected(vt::ClipboardStatus::io_error);
    }
    std::memcpy(storage.get(), encoded->data(), encoded->size());
    attachment.clipboard_write.bytes = std::move(storage);
    attachment.clipboard_write.size = encoded->size();
    attachment.clipboard_write.offset = 0;
    attachment.clipboard_write.format = request.protocol == vt::ClipboardProtocol::osc52
                                            ? PendingClipboardWrite::Format::osc52
                                            : PendingClipboardWrite::Format::kitty;
    attachment.clipboard = std::move(transaction);
    attachment.clipboard_owner = pane;
    schedule_frame(session, FrameUrgency::state_change, false, {});
    return {};
  } catch (const std::bad_alloc&) {
    // Reject a clipboard job before publication; the active terminal remains usable.
    return std::unexpected(vt::ClipboardStatus::io_error);
  }
}

void start_clipboard_request(SessionRecord& session, const Pane& pane,
                             PaneRuntime& runtime) noexcept {
  const auto request = runtime.terminal.clipboard_request();
  if (!request) {
    return;
  }
  const auto& attachment = session.attachment_runtime;
  if (attachment.clipboard != nullptr && attachment.clipboard_owner == pane.id &&
      attachment.clipboard->request_id() == request->id) {
    return;
  }
  const auto result = begin_clipboard_transaction(session, pane.id, *request);
  if (result) {
    return;
  }
  if (!runtime.terminal.complete_clipboard(request->id, result.error(), {}) ||
      !queue_terminal_responses(runtime.pending_writes, runtime.terminal)) {
    runtime.fail(PaneRuntimeFailure::terminal_integrity_error);
  }
}

// Completion is one owner/policy-checked publication boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void service_clipboard(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept {
  auto& attachment = session.attachment_runtime;
  if (attachment.clipboard == nullptr) {
    return;
  }
  auto& transaction = *attachment.clipboard;
  auto* runtime = attachment.clipboard_owner
                      ? runtimes.get({.session = session.id, .pane = *attachment.clipboard_owner})
                      : nullptr;
  const auto* tab = find_tab(session, session.active_tab);
  const bool eligible = runtime != nullptr && runtime->live() && attachment.client >= 0 &&
                        tab != nullptr && attachment.clipboard_owner == tab->focused_pane;
  auto request = runtime != nullptr ? runtime->terminal.clipboard_request() : std::nullopt;
  const auto paste = attachment.clipboard_paste.lock();
  const bool explicit_paste = transaction.request_id() == 0;
  const bool allowed =
      eligible &&
      (explicit_paste ? paste != nullptr && paste->connection == attachment.connection_id
                      : request && request->id == transaction.request_id() &&
                            (request->read ? active_reactor_environment->clipboard_read
                                           : active_reactor_environment->clipboard_write));
  if (!allowed) {
    transaction.fail(vt::ClipboardStatus::denied);
  } else if (transaction.expired(reactor_now())) {
    transaction.fail(vt::ClipboardStatus::io_error);
  }
  if (!transaction.done()) {
    return;
  }
  if (transaction.uncorrelated_read_outstanding()) {
    attachment.osc52_read_retired = true;
  }
  auto& staged = attachment.clipboard_write;
  if (transaction.status() != vt::ClipboardStatus::success &&
      staged.format != PendingClipboardWrite::Format::selection) {
    const auto abort_size =
        staged.offset > 0 ? transaction.abort_write(std::span(staged.bytes.get(), staged.size)) : 0;
    if (abort_size == 0) {
      staged.reset();
    } else {
      staged.size = abort_size;
      staged.offset = 0;
      staged.interleave_frame = false;
      schedule_frame(session, FrameUrgency::state_change, false, {});
    }
  }
  if (explicit_paste && paste != nullptr) {
    paste->reply = std::move(attachment.clipboard);
  } else if (!explicit_paste && runtime != nullptr && request &&
             request->id == transaction.request_id()) {
    if (!runtime->terminal.complete_clipboard(request->id, transaction.status(),
                                              transaction.contents()) ||
        !queue_terminal_responses(runtime->pending_writes, runtime->terminal)) {
      runtime->fail(PaneRuntimeFailure::terminal_integrity_error);
    }
  }
  attachment.clipboard = nullptr;
  attachment.clipboard_owner.reset();
}

// Connection teardown resets only Attachment and AttachmentRuntime state. Session and PaneRuntime
// lifetimes remain independent, while the direct aggregate layout avoids a connection hot-path
// allocation or lookup.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void detach_attachment(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept {
  finish_live_divider_resize(session);
  for (auto& pane_slot : session.panes) {
    if (pane_slot.pane == nullptr) {
      continue;
    }
    auto* const runtime = runtimes.get({.session = session.id, .pane = pane_slot.pane->id});
    LEMMA_ASSERT(runtime != nullptr);
    runtime->terminal.set_clipboard_access(false, false);
    if (!queue_terminal_responses(runtime->pending_writes, runtime->terminal)) {
      runtime->fail(PaneRuntimeFailure::terminal_integrity_error);
    }
    runtime->terminal.reset_selection_gesture();
    runtime->terminal.clear_selection_checkpoint();
    runtime->terminal.clear_selection();
    runtime->terminal.scroll_viewport(vt::ViewportScroll::bottom);
    record_terminal_observation(*runtime);
    runtime->interactive_damage.reset();
  }

  session.attachment.copy_mode = {};
  session.attachment.rename_prompt = {};
  session.attachment.command_line = {};
  session.attachment.message_view = {};
  session.attachment.status_message_visible = false;
  session.input_router.reset();
  session.interaction_router.reset();
  session.attachment.selection_target.reset();
  session.attachment.mouse_capture.reset();
  session.attachment_runtime.reset_connection();
}

[[nodiscard]] constexpr auto terminal_color(const protocol::RgbColor color) noexcept
    -> vt::RgbColor {
  return {.red = color.red, .green = color.green, .blue = color.blue};
}

[[nodiscard]] constexpr auto terminal_mouse_button(const protocol::MouseInputButton button) noexcept
    -> std::optional<vt::MouseButton> {
  switch (button) {
  case protocol::MouseInputButton::none:
    return std::nullopt;
  case protocol::MouseInputButton::left:
    return vt::MouseButton::left;
  case protocol::MouseInputButton::right:
    return vt::MouseButton::right;
  case protocol::MouseInputButton::middle:
    return vt::MouseButton::middle;
  case protocol::MouseInputButton::four:
    return vt::MouseButton::four;
  case protocol::MouseInputButton::five:
    return vt::MouseButton::five;
  case protocol::MouseInputButton::six:
    return vt::MouseButton::six;
  case protocol::MouseInputButton::seven:
    return vt::MouseButton::seven;
  case protocol::MouseInputButton::eight:
    return vt::MouseButton::eight;
  case protocol::MouseInputButton::nine:
    return vt::MouseButton::nine;
  case protocol::MouseInputButton::ten:
    return vt::MouseButton::ten;
  case protocol::MouseInputButton::eleven:
    return vt::MouseButton::eleven;
  }
  return std::nullopt;
}

[[nodiscard]] auto rollback_session_theme(const std::span<PaneRuntime* const> changed,
                                          const vt::TerminalTheme& previous) noexcept -> bool {
  bool restored = true;
  for (std::size_t remaining = changed.size(); remaining > 0; --remaining) {
    auto* const runtime = changed.subspan(remaining - 1U, 1).front();
    LEMMA_ASSERT(runtime != nullptr);
    if (!runtime->terminal.set_theme(previous).has_value()) {
      runtime->fail(PaneRuntimeFailure::terminal_integrity_error);
      restored = false;
    }
  }
  return restored;
}

// Theme application is one fixed bounded transaction across the session hierarchy.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto apply_session_theme(SessionRecord& session, PaneRuntimeStore& runtimes,
                                       const vt::TerminalTheme& theme) noexcept -> bool {
  std::array<PaneRuntime*, panes_per_session_max> changed{};
  std::size_t changed_count = 0;
  for (auto& pane_slot : session.panes) {
    if (pane_slot.pane == nullptr) {
      continue;
    }
    auto* const runtime = runtimes.get({.session = session.id, .pane = pane_slot.pane->id});
    LEMMA_ASSERT(runtime != nullptr && changed_count < changed.size());
    if (!runtime->terminal.set_theme(theme).has_value()) {
      const bool restored =
          rollback_session_theme(std::span(changed).first(changed_count), session.theme);
      session.active = session.active && restored;
      return false;
    }
    std::span(changed).subspan(changed_count, 1).front() = runtime;
    ++changed_count;
  }
  session.theme = theme;
  session.theme_bound = true;
  return true;
}

[[nodiscard]] auto bind_session_theme(SessionRecord& session, PaneRuntimeStore& runtimes,
                                      const protocol::HostTerminalTheme& host_theme) noexcept
    -> bool {
  auto theme = session.theme;
  if (host_theme.foreground.has_value()) {
    theme.foreground = terminal_color(*host_theme.foreground);
    theme.cursor = theme.foreground;
  }
  if (host_theme.background.has_value()) {
    theme.background = terminal_color(*host_theme.background);
  }
  if (host_theme.selection_foreground.has_value()) {
    theme.selection_foreground = terminal_color(*host_theme.selection_foreground);
  }
  if (host_theme.selection_background.has_value()) {
    theme.selection_background = terminal_color(*host_theme.selection_background);
  }
  for (std::size_t index = 0; index < theme.palette.size(); ++index) {
    if (host_theme.has_palette_color(index)) {
      std::span(theme.palette).subspan(index, 1).front() =
          terminal_color(std::span(host_theme.palette).subspan(index, 1).front());
    }
  }
  return apply_session_theme(session, runtimes, theme);
}

struct EncodedContextId final {
  std::array<char, 32> bytes{};
  std::size_t size{0};

  [[nodiscard]] auto view() const noexcept -> std::string_view { return {bytes.data(), size}; }
};

[[nodiscard]] auto encode_context_id(const auto id) noexcept -> EncodedContextId {
  EncodedContextId result;
  const auto slot = std::to_chars(result.bytes.begin(), result.bytes.end(), id.slot());
  if (slot.ec != std::errc{} || slot.ptr == result.bytes.end()) {
    return {};
  }
  const auto used = static_cast<std::size_t>(std::distance(result.bytes.begin(), slot.ptr));
  auto remaining = std::span(result.bytes).subspan(used);
  remaining.front() = ':';
  remaining = remaining.subspan(1);
  const auto generation = std::to_chars(remaining.data(), result.bytes.end(), id.generation());
  if (generation.ec != std::errc{}) {
    return {};
  }
  result.size = static_cast<std::size_t>(std::distance(result.bytes.begin(), generation.ptr));
  return result;
}

[[nodiscard]] auto create_pane_runtime(
    const std::uint16_t columns, const std::uint16_t rows, const std::string_view working_directory,
    const std::span<const std::byte> environment, const LaunchEnvironmentMode environment_mode,
    const vt::TerminalTheme& theme, const SessionId session_id, const std::string_view session_name,
    const TabId tab_id, const PaneId pane_id, const std::span<const std::byte> launch_command = {},
    const protocol::CellSize cell = {}) noexcept -> std::unique_ptr<PaneRuntime> {
  vt::TerminalOptions options;
  options.size = {
      .columns = columns, .rows = rows, .cell_width_px = cell.width, .cell_height_px = cell.height};
  options.theme = theme;
  options.scrollback_lines_max = reactor_scrollback_lines();
  auto terminal_result = vt::Terminal::create(options);
  if (!terminal_result.has_value()) {
    return nullptr;
  }
  std::unique_ptr<PaneRuntime> runtime;
  try {
    runtime =
        std::make_unique<PaneRuntime>(std::move(*terminal_result), options.scrollback_bytes_max);
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
  const auto encoded_session = encode_context_id(session_id);
  const auto encoded_tab = encode_context_id(tab_id);
  const auto encoded_pane = encode_context_id(pane_id);
  if (encoded_session.size == 0 || encoded_tab.size == 0 || encoded_pane.size == 0) {
    return nullptr;
  }
  const std::array overlay{
      platform::EnvironmentVariable{.name = "LEMMA_SESSION_ID", .value = encoded_session.view()},
      platform::EnvironmentVariable{.name = "LEMMA_SESSION_NAME", .value = session_name},
      platform::EnvironmentVariable{.name = "LEMMA_TAB_ID", .value = encoded_tab.view()},
      platform::EnvironmentVariable{.name = "LEMMA_PANE_ID", .value = encoded_pane.view()},
  };
  runtime->child =
      platform::spawn_process(runtime->pty, working_directory, environment,
                              platform_environment_mode(environment_mode), launch_command, overlay);
  if (runtime->child <= 0 || !set_nonblocking(runtime->pty) ||
      !platform::resize_pty(runtime->pty, columns, rows, options.size.cell_width_px,
                            options.size.cell_height_px)) {
    return nullptr;
  }
  const auto compression_activity = runtime->terminal.compression_activity();
  if (!compression_activity.has_value()) {
    return nullptr;
  }
  runtime->compression_activity = *compression_activity;
  return runtime;
}

[[nodiscard]] auto refresh_process_name(PaneRuntime& runtime) noexcept -> bool {
  std::array<char, process_name_bytes_max> current{};
  const auto size = platform::foreground_process_name(runtime.pty, current);
  if (size == 0 ||
      (size == runtime.process_name_size &&
       std::ranges::equal(std::span(current).first(size),
                          std::span(runtime.process_name).first(runtime.process_name_size)))) {
    return false;
  }
  std::ranges::copy(std::span(current).first(size), runtime.process_name.begin());
  runtime.process_name_size = size;
  return true;
}

[[nodiscard]] auto
refresh_process_name_if_due(PaneRuntime& runtime,
                            const std::chrono::steady_clock::time_point now) noexcept -> bool {
  if (now < runtime.next_process_name_refresh) {
    return false;
  }
  runtime.next_process_name_refresh = now + process_name_refresh_interval;
  return refresh_process_name(runtime);
}

[[nodiscard]] constexpr auto next_generation(const std::uint32_t generation) noexcept
    -> std::uint32_t {
  LEMMA_ASSERT(generation < std::numeric_limits<std::uint32_t>::max());
  return generation + 1U;
}

[[nodiscard]] auto create_session(
    const std::string_view name, const std::string_view working_directory = {},
    const std::span<const std::byte> environment = {},
    const LaunchEnvironmentMode environment_mode = LaunchEnvironmentMode::inherit) noexcept
    -> std::unique_ptr<SessionRecord> {
  try {
    auto created =
        std::make_unique<SessionRecord>(name, working_directory, environment, environment_mode);
    if (active_command_history != nullptr) {
      created->attachment.command_history = *active_command_history;
    }
    return created;
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

void note_compression_activity(PaneRuntime& runtime) noexcept;
[[nodiscard]] auto update_copy_viewport_offset(SessionRecord& session,
                                               PaneRuntime& runtime) noexcept -> bool;
void leave_copy_mode(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept;

enum class LayoutResolutionStatus : std::uint8_t {
  applied,
  rejected,
  consistency_lost,
};

struct PaneResizePlanEntry final {
  Pane* pane{nullptr};
  PaneRuntime* runtime{nullptr};
  render::PaneRectangle previous;
  render::PaneRectangle target;
  bool runtime_resized{false};
  bool runtime_touched{false};
};

using PaneResizePlan = std::array<PaneResizePlanEntry, panes_per_tab_max>;

[[nodiscard]] auto resize_runtime_for_layout(PaneRuntime& runtime,
                                             const render::PaneRectangle target) noexcept
    -> TerminalResizeStatus {
  if (runtime.held()) {
    const auto previous = runtime.terminal.size();
    if (previous.columns == target.columns && previous.rows == target.rows) {
      return TerminalResizeStatus::unchanged;
    }
    const vt::TerminalSize requested{.columns = target.columns,
                                     .rows = target.rows,
                                     .cell_width_px = previous.cell_width_px,
                                     .cell_height_px = previous.cell_height_px};
    if (!runtime.terminal.resize(requested).has_value()) {
      runtime.fail(PaneRuntimeFailure::resize_consistency_lost);
      return TerminalResizeStatus::consistency_lost;
    }
    runtime.pending_writes.clear();
    return TerminalResizeStatus::applied;
  }
  const auto status =
      resize_pane_terminal(runtime.pty, runtime.terminal, target.columns, target.rows);
  if (status == TerminalResizeStatus::consistency_lost) {
    runtime.fail(PaneRuntimeFailure::resize_consistency_lost);
    return status;
  }
  if (status != TerminalResizeStatus::applied) {
    return status;
  }
  // Ghostty may emit mode-2048 size reports from resize itself. Those bytes must enter the
  // pane's ordered PTY queue immediately; they are not left in the adapter until the next PTY
  // read or keystroke.
  if (!queue_terminal_responses(runtime.pending_writes, runtime.terminal)) {
    runtime.fail(PaneRuntimeFailure::terminal_integrity_error);
    return TerminalResizeStatus::consistency_lost;
  }
  const auto synchronized = runtime.terminal.synchronized_output();
  if (!synchronized.has_value()) {
    runtime.fail(PaneRuntimeFailure::terminal_integrity_error);
    return TerminalResizeStatus::consistency_lost;
  }
  static_cast<void>(runtime.presentation_gate.observe(*synchronized, true, reactor_now()));
  return status;
}

void finish_resize_mutation(PaneResizePlanEntry& entry) noexcept {
  if (!entry.runtime_touched) {
    return;
  }
  LEMMA_ASSERT(entry.runtime != nullptr);
  record_terminal_mutation(*entry.runtime);
  note_compression_activity(*entry.runtime);
}

[[nodiscard]] auto rollback_layout_resize(PaneResizePlan& plan, const std::size_t count) noexcept
    -> LayoutResolutionStatus {
  bool consistency_lost = false;
  for (std::size_t remaining = count; remaining > 0; --remaining) {
    auto& entry = std::span(plan).subspan(remaining - 1U, 1).front();
    if (!entry.runtime_resized) {
      finish_resize_mutation(entry);
      continue;
    }
    LEMMA_ASSERT(entry.runtime != nullptr);
    const auto status = resize_runtime_for_layout(*entry.runtime, entry.previous);
    entry.runtime_touched = true;
    if (status != TerminalResizeStatus::applied && status != TerminalResizeStatus::unchanged) {
      entry.runtime->fail(PaneRuntimeFailure::resize_consistency_lost);
      consistency_lost = true;
    }
    finish_resize_mutation(entry);
  }
  return consistency_lost ? LayoutResolutionStatus::consistency_lost
                          : LayoutResolutionStatus::rejected;
}

[[nodiscard]] auto apply_layout_resize_plan(PaneResizePlan& plan, const std::size_t count) noexcept
    -> LayoutResolutionStatus {
  for (std::size_t index = 0; index < count; ++index) {
    auto& entry = std::span(plan).subspan(index, 1).front();
    LEMMA_ASSERT(entry.runtime != nullptr);
    if (entry.previous.columns == entry.target.columns &&
        entry.previous.rows == entry.target.rows) {
      continue;
    }
    const auto status = resize_runtime_for_layout(*entry.runtime, entry.target);
    entry.runtime_touched = status == TerminalResizeStatus::applied ||
                            status == TerminalResizeStatus::rolled_back ||
                            status == TerminalResizeStatus::consistency_lost;
    entry.runtime_resized = status == TerminalResizeStatus::applied;
    if (status == TerminalResizeStatus::rejected || status == TerminalResizeStatus::rolled_back) {
      return rollback_layout_resize(plan, index + 1U);
    }
    if (status == TerminalResizeStatus::consistency_lost) {
      static_cast<void>(rollback_layout_resize(plan, index));
      finish_resize_mutation(entry);
      return LayoutResolutionStatus::consistency_lost;
    }
  }
  return LayoutResolutionStatus::applied;
}

void commit_layout_resize_plan(PaneResizePlan& plan, const std::size_t count) noexcept {
  for (auto& entry : std::span(plan).first(count)) {
    LEMMA_ASSERT(entry.pane != nullptr);
    entry.pane->rectangle = entry.target;
    finish_resize_mutation(entry);
  }
}

// Building and applying the fixed transaction handles zoomed and tiled plans explicitly.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto resolve_layout(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                                  const PaneLayout* const proposed_layout = nullptr) noexcept
    -> LayoutResolutionStatus {
  LEMMA_ASSERT(proposed_layout == nullptr || !tab.zoomed);
  const render::PaneRectangle viewport{
      .column = tab.layout_column,
      .row = tab.layout_row,
      .columns = tab.layout_columns,
      .rows = tab.layout_rows,
  };
  LayoutProjection projection;
  if (tab.zoomed) {
    auto* const focused = find_pane(session, tab, tab.focused_pane);
    if (focused == nullptr) {
      return LayoutResolutionStatus::consistency_lost;
    }
    std::span(projection.rectangles).subspan(focused->id.slot(), 1).front() = viewport;
    std::span(projection.panes).subspan(focused->id.slot(), 1).front() = focused->id;
    std::span(projection.included).subspan(focused->id.slot(), 1).front() = true;
    projection.pane_count = 1;
  } else {
    const auto& layout = proposed_layout == nullptr ? tab.layout : *proposed_layout;
    const auto projected = layout.project(viewport);
    if (!projected.has_value()) {
      return LayoutResolutionStatus::consistency_lost;
    }
    projection = *projected;
    for (std::size_t index = 0; index < session.panes.size(); ++index) {
      const auto& pane_slot = std::span(session.panes).subspan(index, 1).front();
      const bool belongs = pane_slot.pane != nullptr && pane_slot.pane->tab == tab.id;
      const bool included = std::span(projection.included).subspan(index, 1).front();
      if (belongs != included ||
          (included &&
           std::span(projection.panes).subspan(index, 1).front() != pane_slot.pane->id)) {
        return LayoutResolutionStatus::consistency_lost;
      }
    }
  }

  PaneResizePlan plan{};
  std::size_t count = 0;
  for (std::size_t index = 0; index < session.panes.size(); ++index) {
    auto& pane_slot = std::span(session.panes).subspan(index, 1).front();
    if (pane_slot.pane == nullptr || pane_slot.pane->tab != tab.id ||
        !std::span(projection.included).subspan(index, 1).front()) {
      continue;
    }
    auto* const runtime = find_pane_runtime(runtimes, session, *pane_slot.pane);
    LEMMA_ASSERT(runtime != nullptr);
    std::span(plan).subspan(count, 1).front() = {
        .pane = pane_slot.pane.get(),
        .runtime = runtime,
        .previous = pane_slot.pane->rectangle,
        .target = std::span(projection.rectangles).subspan(index, 1).front(),
    };
    ++count;
  }

  const auto status = apply_layout_resize_plan(plan, count);
  if (status != LayoutResolutionStatus::applied) {
    return status;
  }
  if (proposed_layout != nullptr) {
    LEMMA_ASSERT(proposed_layout->valid());
    tab.layout = *proposed_layout;
  }
  commit_layout_resize_plan(plan, count);
  return LayoutResolutionStatus::applied;
}

void refresh_copy_selection_after_layout(SessionRecord& session, Tab& tab,
                                         PaneRuntimeStore& runtimes) noexcept {
  const auto target = session.attachment.selection_target;
  if (!session.attachment.copy_mode.active() || !target.has_value() || target->tab != tab.id) {
    return;
  }
  auto* const pane = find_pane(session, tab, target->pane);
  if (pane == nullptr) {
    leave_copy_mode(session, runtimes);
    return;
  }
  auto* const runtime = find_pane_runtime(runtimes, session, tab, *pane);
  LEMMA_ASSERT(runtime != nullptr);
  const auto refreshed = runtime->terminal.refresh_selection();
  // Reflow updates Ghostty's tracked endpoints, but the renderer requires a freshly installed
  // selection snapshot. Re-anchor Ghostty's canonical viewport to that endpoint.
  const auto scrolled =
      refreshed.has_value() && *refreshed
          ? runtime->terminal.scroll_selection_into_view()
          : std::expected<bool, vt::Error>{std::unexpected(vt::Error::invalid_state)};
  if (!scrolled.has_value() || !update_copy_viewport_offset(session, *runtime)) {
    leave_copy_mode(session, runtimes);
  }
}

[[nodiscard]] auto
resolve_session_layout(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                       const PaneLayout* const proposed_layout = nullptr) noexcept -> bool {
  const auto status = resolve_layout(session, tab, runtimes, proposed_layout);
  if (status == LayoutResolutionStatus::consistency_lost) {
    session.active = false;
    return false;
  }
  refresh_copy_selection_after_layout(session, tab, runtimes);
  return status == LayoutResolutionStatus::applied;
}

[[nodiscard]] auto frame_sink_state(const SessionRecord& session) noexcept -> FrameSinkState {
  if (session.attachment_runtime.client < 0) {
    return FrameSinkState::unavailable;
  }
  return session.attachment_runtime.output.busy() ? FrameSinkState::blocked : FrameSinkState::ready;
}

void schedule_frame(SessionRecord& session, const FrameUrgency urgency, const bool force_full,
                    const PaneId source = {}) noexcept {
  session.attachment_runtime.frame_scheduler.request(urgency, force_full, reactor_now(),
                                                     frame_sink_state(session), source);
}

struct ProductionSessionRuntimeContext final {
  SessionRecord* session{nullptr};
  PaneRuntimeStore* runtimes{nullptr};
};

[[nodiscard]] auto production_spawn_pane(void* const context,
                                         const SpawnPaneEffect& effect) noexcept
    -> RuntimeEffectStatus {
  auto& owner = *static_cast<ProductionSessionRuntimeContext*>(context);
  LEMMA_ASSERT(owner.session != nullptr && owner.runtimes != nullptr);
  auto& session = *owner.session;
  if (effect.session != session.id ||
      !owner.runtimes->can_reserve_scrollback(limits::terminal_scrollback_bytes_default)) {
    return RuntimeEffectStatus::rejected;
  }
  auto runtime =
      create_pane_runtime(effect.rectangle.columns, effect.rectangle.rows, effect.working_directory,
                          session.launch_environment(), session.environment_mode, session.theme,
                          session.id, session.session_name(), effect.tab, effect.pane,
                          effect.command, session.attachment_runtime.cell_size);
  if (runtime == nullptr) {
    return RuntimeEffectStatus::rejected;
  }
  return owner.runtimes->insert({.session = effect.session, .pane = effect.pane},
                                std::move(runtime))
             ? RuntimeEffectStatus::applied
             : RuntimeEffectStatus::rejected;
}

[[nodiscard]] auto production_resize_panes(void* const context,
                                           const std::span<const ResizePaneEffect> effects) noexcept
    -> RuntimeEffectStatus {
  auto& owner = *static_cast<ProductionSessionRuntimeContext*>(context);
  LEMMA_ASSERT(owner.session != nullptr && owner.runtimes != nullptr);
  if (effects.size() > panes_per_session_max) {
    return RuntimeEffectStatus::consistency_lost;
  }
  PaneResizePlan plan{};
  std::size_t count = 0;
  for (const auto& effect : effects) {
    if (effect.session != owner.session->id) {
      return RuntimeEffectStatus::consistency_lost;
    }
    auto* const runtime = owner.runtimes->get({.session = effect.session, .pane = effect.pane});
    if (runtime == nullptr) {
      return RuntimeEffectStatus::consistency_lost;
    }
    std::span(plan).subspan(count, 1).front() = {
        .pane = nullptr,
        .runtime = runtime,
        .previous = effect.previous,
        .target = effect.target,
    };
    ++count;
  }
  const auto status = apply_layout_resize_plan(plan, count);
  if (status == LayoutResolutionStatus::applied) {
    for (auto& entry : std::span(plan).first(count)) {
      finish_resize_mutation(entry);
    }
    return RuntimeEffectStatus::applied;
  }
  return status == LayoutResolutionStatus::rejected ? RuntimeEffectStatus::rejected
                                                    : RuntimeEffectStatus::consistency_lost;
}

void production_retire_pane(void* const context, const SessionId session,
                            const PaneId pane) noexcept {
  auto& owner = *static_cast<ProductionSessionRuntimeContext*>(context);
  LEMMA_ASSERT(owner.session != nullptr && owner.runtimes != nullptr &&
               owner.session->id == session);
  const bool erased = owner.runtimes->erase({.session = session, .pane = pane});
  LEMMA_ASSERT(erased);
}

void production_hold_pane(void* const context, const SessionId session, const PaneId pane,
                          [[maybe_unused]] const ProcessExit process) noexcept {
  auto& owner = *static_cast<ProductionSessionRuntimeContext*>(context);
  LEMMA_ASSERT(owner.session != nullptr && owner.runtimes != nullptr &&
               owner.session->id == session);
  auto* const runtime = owner.runtimes->get({.session = session, .pane = pane});
  LEMMA_ASSERT(runtime != nullptr);
  close_descriptor(runtime->pty);
  runtime->pending_writes.clear();
  runtime->failure.reset();
  runtime->observed_exit.reset();
  runtime->child = -1;
  runtime->process_name = {};
  constexpr std::string_view exited_title = "exited";
  std::ranges::copy(exited_title, runtime->process_name.begin());
  runtime->process_name_size = exited_title.size();
  record_terminal_mutation(*runtime);
}

[[nodiscard]] auto production_session_options(ProductionSessionRuntimeContext& context,
                                              const SessionNameConflict name_conflict = nullptr,
                                              void* const name_conflict_context = nullptr) noexcept
    -> SessionMachineOptions {
  return {
      .runtime = {.context = &context,
                  .spawn = &production_spawn_pane,
                  .resize = &production_resize_panes,
                  .retire = &production_retire_pane,
                  .hold = &production_hold_pane},
      .name_conflict = name_conflict,
      .name_conflict_context = name_conflict_context,
  };
}

void apply_session_change(SessionRecord& session, PaneRuntimeStore& runtimes,
                          const SessionChange change) noexcept {
  if (change.status_changed) {
    session.attachment_runtime.status_valid = false;
  }
  if (change.invalidate_terminal.is_valid()) {
    auto* const runtime = runtimes.get({.session = session.id, .pane = change.invalidate_terminal});
    if (runtime != nullptr) {
      runtime->terminal.invalidate_ansi_render_state();
    }
  }
  if (change.frame_requested && session.active) {
    schedule_frame(session, FrameUrgency::state_change, change.force_full_frame);
  }
}

void finish_live_divider_resize(SessionRecord& session, const bool discard_release) noexcept {
  if (!session.attachment.mouse_capture.has_value() ||
      session.attachment.mouse_capture->owner != MouseCaptureOwner::divider) {
    return;
  }
  if (discard_release) {
    session.attachment.mouse_capture->owner = MouseCaptureOwner::discard_until_release;
  } else {
    session.attachment.mouse_capture.reset();
  }
}

[[nodiscard]] auto selection_pane(SessionRecord& session) noexcept -> Pane* {
  const auto target = session.attachment.selection_target;
  if (!target.has_value()) {
    return nullptr;
  }
  auto* const tab = find_tab(session, target->tab);
  return tab == nullptr ? nullptr : find_pane(session, *tab, target->pane);
}

[[nodiscard]] auto selection_runtime(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept
    -> PaneRuntime* {
  const auto target = session.attachment.selection_target;
  if (!target.has_value()) {
    return nullptr;
  }
  auto* const tab = find_tab(session, target->tab);
  auto* const pane = tab == nullptr ? nullptr : find_pane(session, *tab, target->pane);
  return pane == nullptr ? nullptr : find_pane_runtime(runtimes, session, *tab, *pane);
}

[[nodiscard]] auto selection_runtime(const SessionRecord& session,
                                     const PaneRuntimeStore& runtimes) noexcept
    -> const PaneRuntime* {
  const auto target = session.attachment.selection_target;
  if (!target.has_value()) {
    return nullptr;
  }
  const auto* const tab = find_tab(session, target->tab);
  const auto* const pane = tab == nullptr ? nullptr : find_pane(session, *tab, target->pane);
  return pane == nullptr ? nullptr : find_pane_runtime(runtimes, session, *tab, *pane);
}

[[nodiscard]] auto copy_mode_pane(SessionRecord& session) noexcept -> Pane* {
  return session.attachment.copy_mode.active() ? selection_pane(session) : nullptr;
}

[[nodiscard]] auto copy_mode_runtime(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept
    -> PaneRuntime* {
  return session.attachment.copy_mode.active() ? selection_runtime(session, runtimes) : nullptr;
}

[[nodiscard]] auto copy_mode_runtime(const SessionRecord& session,
                                     const PaneRuntimeStore& runtimes) noexcept
    -> const PaneRuntime* {
  return session.attachment.copy_mode.active() ? selection_runtime(session, runtimes) : nullptr;
}

void note_compression_activity(PaneRuntime& runtime) noexcept {
  const auto activity = runtime.terminal.compression_activity();
  if (!activity.has_value()) {
    runtime.fail(PaneRuntimeFailure::scrollback_compression_error);
    return;
  }
  if (*activity == runtime.compression_activity && runtime.compression_scheduled) {
    return;
  }
  if (*activity != runtime.compression_activity) {
    runtime.compression_activity = *activity;
  }
  runtime.compression_scheduled = true;
  runtime.compression_deadline = reactor_now() + limits::scrollback_compression_idle_delay;
}

[[nodiscard]] auto scroll_viewport_for_application_input(SessionRecord& session,
                                                         PaneRuntime& runtime) noexcept -> bool {
  const auto scrolled = runtime.terminal.scroll_viewport_to_bottom();
  if (!scrolled.has_value()) {
    runtime.fail(PaneRuntimeFailure::terminal_integrity_error);
    return false;
  }
  if (*scrolled) {
    record_terminal_observation(runtime);
    note_compression_activity(runtime);
    schedule_frame(session, FrameUrgency::interactive, true);
  }
  return true;
}

void invalidate_copy_presentation(SessionRecord& session, PaneRuntime& runtime) noexcept {
  runtime.terminal.invalidate_ansi_render_state();
  schedule_frame(session, FrameUrgency::state_change, false);
}

[[nodiscard]] auto update_copy_viewport_offset(SessionRecord& session,
                                               PaneRuntime& runtime) noexcept -> bool {
  const auto viewport = runtime.terminal.viewport_state();
  if (!viewport.has_value()) {
    return false;
  }
  session.attachment.copy_mode.viewport_offset = viewport->offset;
  return true;
}

void reset_selection_capture(Attachment& attachment,
                             const std::optional<AttachmentPaneTarget> target) noexcept {
  if (target.has_value() && attachment.mouse_capture.has_value() &&
      attachment.mouse_capture->owner == MouseCaptureOwner::selection &&
      attachment.mouse_capture->target == *target) {
    attachment.mouse_capture.reset();
  }
}

void leave_copy_mode(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept {
  const bool changed = session.attachment.copy_mode.active();
  const auto target = session.attachment.selection_target;
  auto* const runtime = selection_runtime(session, runtimes);
  if (runtime != nullptr) {
    runtime->terminal.reset_selection_gesture();
    runtime->terminal.clear_selection_checkpoint();
    runtime->terminal.clear_selection();
    if (changed) {
      runtime->terminal.scroll_viewport(vt::ViewportScroll::bottom);
      record_terminal_observation(*runtime);
      note_compression_activity(*runtime);
    }
  }
  reset_selection_capture(session.attachment, target);
  session.attachment.selection_target.reset();
  session.attachment.copy_mode = {};
  session.attachment_runtime.copy_mode = {};
  if (changed) {
    schedule_frame(session, FrameUrgency::state_change, true);
  }
}

void reset_rename_prompt(SessionRecord& session, const bool redraw = true) noexcept {
  const bool changed = session.attachment.rename_prompt.active();
  session.attachment.rename_prompt = {};
  if (changed && redraw) {
    schedule_frame(session, FrameUrgency::state_change, false);
  }
}

void reset_command_line(SessionRecord& session, const bool redraw = true) noexcept {
  const bool changed = session.attachment.command_line.active;
  session.attachment.command_line = {};
  if (changed && redraw) {
    schedule_frame(session, FrameUrgency::state_change, false);
  }
}

void leave_message_view(SessionRecord& session, const bool redraw = true) noexcept {
  if (!session.attachment.message_view.active) {
    return;
  }
  session.attachment.message_view = {};
  session.interaction_router.reset();
  if (redraw) {
    schedule_frame(session, FrameUrgency::state_change, true);
  }
}

void clear_status_message(SessionRecord& session, const bool redraw = true) noexcept {
  if (!session.attachment.status_message_visible) {
    return;
  }
  session.attachment.status_message_visible = false;
  session.attachment_runtime.status_message_deadline.reset();
  if (redraw) {
    schedule_frame(session, FrameUrgency::state_change, false);
  }
}

// Status retention is clamped to the fixed entry array before access.
// NOLINTNEXTLINE(bugprone-exception-escape)
void publish_status_message(SessionRecord& session, const StatusMessageKind kind,
                            const std::string_view text) noexcept {
  if (text.empty()) {
    return;
  }
  auto& log = session.attachment.status_messages;
  const auto retained = std::min<std::size_t>(log.size, log.entries.size() - 1U);
  auto entries = std::span(log.entries).first(retained + 1U);
  std::ranges::copy_backward(entries.first(retained), entries.end());
  auto& entry = log.entries.front();
  entry = {};
  const auto retained_text = text.substr(0, std::min(text.size(), entry.text.size()));
  std::ranges::transform(retained_text, entry.text.begin(), [](const char character) {
    const auto value = static_cast<unsigned char>(character);
    return value >= 0x20U && value < 0x7fU ? character : '?';
  });
  entry.size = static_cast<std::uint16_t>(retained_text.size());
  entry.unix_seconds = static_cast<std::int64_t>(std::time(nullptr));
  entry.kind = kind;
  log.size = static_cast<std::uint8_t>(
      std::min<std::size_t>(log.entries.size(), static_cast<std::size_t>(log.size) + 1U));
  session.attachment.status_message_visible = true;
  session.attachment_runtime.status_message_deadline =
      reactor_now() + limits::status_message_display_time;
  schedule_frame(session, FrameUrgency::state_change, false);
}

void begin_message_view(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept {
  if (session.attachment.status_messages.size == 0) {
    return;
  }
  leave_copy_mode(session, runtimes);
  reset_rename_prompt(session, false);
  reset_command_line(session, false);
  clear_status_message(session, false);
  session.attachment.message_view = {.offset = 0, .active = true};
  session.interaction_router.select_base(input::ConfiguredInputContext::messages);
  schedule_frame(session, FrameUrgency::state_change, true);
}

void clear_command_line_requests(CommandLineState& state) noexcept {
  state.submit_requested = false;
  state.completion_requested = false;
}

void assign_command_line_text(CommandLineState& state, const std::string_view text) noexcept {
  LEMMA_ASSERT(text.size() <= state.text.size());
  state.text = {};
  std::ranges::copy(text, state.text.begin());
  state.size = text.size();
  state.cursor = state.size;
  clear_command_line_requests(state);
}

void begin_command_line(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept {
  if (!reactor_status_line()) {
    return;
  }
  leave_message_view(session, false);
  leave_copy_mode(session, runtimes);
  reset_rename_prompt(session, false);
  auto& state = session.attachment.command_line;
  state = {};
  state.active = true;
  clear_status_message(session, false);
  session.interaction_router.select_base(input::ConfiguredInputContext::command_line);
  schedule_frame(session, FrameUrgency::state_change, false);
}

[[nodiscard]] constexpr auto rename_prompt_character(const RenamePromptKind kind,
                                                     const std::uint8_t byte) noexcept -> bool {
  if (kind == RenamePromptKind::session) {
    return (byte >= static_cast<std::uint8_t>('a') && byte <= static_cast<std::uint8_t>('z')) ||
           (byte >= static_cast<std::uint8_t>('A') && byte <= static_cast<std::uint8_t>('Z')) ||
           (byte >= static_cast<std::uint8_t>('0') && byte <= static_cast<std::uint8_t>('9')) ||
           byte == static_cast<std::uint8_t>('_') || byte == static_cast<std::uint8_t>('-');
  }
  return kind == RenamePromptKind::tab && byte >= 0x20U && byte <= 0x7eU;
}

[[nodiscard]] constexpr auto rename_prompt_capacity(const RenamePromptKind kind) noexcept
    -> std::size_t {
  return kind == RenamePromptKind::session ? limits::session_name_bytes_max
                                           : limits::tab_title_bytes_max;
}

[[nodiscard]] auto begin_rename_prompt(SessionRecord& session, const RenamePromptKind kind,
                                       const TabId tab, const std::string_view initial) noexcept
    -> bool {
  if (kind == RenamePromptKind::inactive) {
    return false;
  }
  const auto capacity = rename_prompt_capacity(kind);
  auto prompt = RenamePromptState{.tab = tab, .kind = kind};
  for (const char character : initial) {
    if (prompt.size == capacity) {
      break;
    }
    const auto byte = static_cast<std::uint8_t>(static_cast<unsigned char>(character));
    if (rename_prompt_character(kind, byte)) {
      std::span(prompt.text).subspan(prompt.size, 1).front() = character;
      ++prompt.size;
    } else if (kind == RenamePromptKind::tab) {
      // Derived process and terminal titles are child-controlled. Preserve their bounded shape for
      // editing without allowing control or non-ASCII bytes to enter the outer-terminal stream.
      std::span(prompt.text).subspan(prompt.size, 1).front() = '?';
      ++prompt.size;
    }
  }
  prompt.cursor = prompt.size;
  session.attachment.rename_prompt = prompt;
  session.interaction_router.select_base(input::ConfiguredInputContext::rename);
  schedule_frame(session, FrameUrgency::state_change, false);
  return true;
}

void clear_mouse_selection(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept {
  if (session.attachment.copy_mode.active() || !session.attachment.selection_target.has_value()) {
    return;
  }
  const auto target = session.attachment.selection_target;
  auto* const runtime = selection_runtime(session, runtimes);
  bool changed = false;
  if (runtime != nullptr) {
    const auto active = runtime->terminal.selection_active();
    changed = !active.has_value() || *active;
    runtime->terminal.reset_selection_gesture();
    runtime->terminal.clear_selection();
  }
  reset_selection_capture(session.attachment, target);
  session.attachment.selection_target.reset();
  if (changed) {
    schedule_frame(session, FrameUrgency::interactive, false);
  }
}

void preserve_copy_viewport_after_mutation(SessionRecord& session, Pane& pane, PaneRuntime& runtime,
                                           PaneRuntimeStore& runtimes) noexcept {
  if (copy_mode_pane(session) != &pane) {
    return;
  }
  runtime.terminal.scroll_viewport(
      vt::ViewportScroll::row,
      static_cast<std::int64_t>(session.attachment.copy_mode.viewport_offset));
  if (!update_copy_viewport_offset(session, runtime)) {
    leave_copy_mode(session, runtimes);
  }
}

[[nodiscard]] auto enter_copy_mode(SessionRecord& session, Tab& tab, Pane& pane,
                                   PaneRuntime& runtime, PaneRuntimeStore& runtimes) noexcept
    -> bool {
  leave_copy_mode(session, runtimes);
  const auto update = runtime.terminal.update_render_state();
  if (!update.has_value()) {
    return false;
  }
  const vt::TerminalPoint cursor{
      .space = vt::PointSpace::viewport,
      .column = update->cursor_in_viewport ? update->cursor_column : std::uint16_t{0},
      .row = update->cursor_in_viewport ? update->cursor_row
                                        : static_cast<std::uint32_t>(pane.rectangle.rows - 1U),
  };
  const auto selected = runtime.terminal.select(vt::SelectionUnit::cell, cursor);
  if (!selected.has_value() || !*selected) {
    return false;
  }
  session.attachment.copy_mode = {};
  session.attachment.selection_target = AttachmentPaneTarget{.tab = tab.id, .pane = pane.id};
  session.attachment.copy_mode.phase = CopyModePhase::navigation;
  session.interaction_router.select_base(input::ConfiguredInputContext::copy);
  if (!update_copy_viewport_offset(session, runtime)) {
    runtime.terminal.clear_selection();
    session.attachment.selection_target.reset();
    session.attachment.copy_mode = {};
    return false;
  }
  runtime.terminal.invalidate_ansi_render_state();
  schedule_frame(session, FrameUrgency::state_change, true);
  return true;
}

[[nodiscard]] auto scroll_viewport_with_mouse(SessionRecord& session, PaneRuntime& runtime,
                                              const protocol::MouseInputButton button,
                                              const bool copy_selection) noexcept -> bool {
  const auto viewport = runtime.terminal.viewport_state();
  if (!viewport.has_value()) {
    return false;
  }

  const bool upward = button == protocol::MouseInputButton::four;
  runtime.terminal.scroll_viewport(vt::ViewportScroll::delta,
                                   upward ? -mouse_wheel_scroll_rows : mouse_wheel_scroll_rows);
  const auto scrolled = runtime.terminal.viewport_state();
  if (!scrolled.has_value()) {
    return false;
  }
  const bool changed =
      scrolled->offset != viewport->offset || scrolled->follows_output != viewport->follows_output;
  if (copy_selection) {
    session.attachment.copy_mode.viewport_offset = scrolled->offset;
  }
  if (changed) {
    record_terminal_observation(runtime);
    note_compression_activity(runtime);
    schedule_frame(session, FrameUrgency::interactive, true);
  }
  return true;
}

[[nodiscard]] constexpr auto copy_selection_unit(const CopyModePhase phase) noexcept
    -> vt::SelectionUnit {
  switch (phase) {
  case CopyModePhase::visual_line:
    return vt::SelectionUnit::line;
  case CopyModePhase::visual_block:
    return vt::SelectionUnit::block;
  case CopyModePhase::inactive:
  case CopyModePhase::navigation:
  case CopyModePhase::visual_character:
  case CopyModePhase::search_prompt:
  case CopyModePhase::searching:
    return vt::SelectionUnit::cell;
  }
  return vt::SelectionUnit::cell;
}

[[nodiscard]] auto adjust_copy_selection(SessionRecord& session, PaneRuntime& runtime,
                                         PaneRuntimeStore& runtimes,
                                         const vt::SelectionAdjustment adjustment) noexcept
    -> bool {
  const auto phase = session.attachment.copy_mode.phase;
  const auto adjusted =
      runtime.terminal.selection_adjust(adjustment, session.attachment.copy_mode.selecting());
  if (!adjusted.has_value()) {
    leave_copy_mode(session, runtimes);
    return false;
  }
  if (*adjusted && session.attachment.copy_mode.selecting()) {
    const auto normalized = runtime.terminal.selection_normalize_unit(copy_selection_unit(phase));
    if (!normalized.has_value()) {
      leave_copy_mode(session, runtimes);
      return false;
    }
  }
  if (*adjusted) {
    const auto scrolled = runtime.terminal.scroll_selection_into_view();
    if (!scrolled.has_value() || !update_copy_viewport_offset(session, runtime)) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    if (*scrolled) {
      record_terminal_observation(runtime);
    }
    session.attachment.copy_mode.feedback = CopyModeFeedback::none;
    note_compression_activity(runtime);
    invalidate_copy_presentation(session, runtime);
  }
  return true;
}

[[nodiscard]] auto advance_copy_search_cursor(PaneRuntime& runtime, vt::TerminalPoint& point,
                                              CopySearchDirection direction) noexcept -> bool;

[[nodiscard]] auto copy_search_boundary(PaneRuntime& runtime,
                                        const CopySearchDirection direction) noexcept
    -> std::optional<vt::TerminalPoint> {
  const auto viewport = runtime.terminal.viewport_state();
  const auto columns = runtime.terminal.size().columns;
  if (!viewport.has_value() || viewport->total_rows == 0 || columns == 0) {
    return std::nullopt;
  }
  return vt::TerminalPoint{
      .space = vt::PointSpace::screen,
      .column = direction == CopySearchDirection::forward
                    ? std::uint16_t{0}
                    : static_cast<std::uint16_t>(columns - 1U),
      .row = direction == CopySearchDirection::forward
                 ? std::uint32_t{0}
                 : static_cast<std::uint32_t>(viewport->total_rows - 1U),
  };
}

[[nodiscard]] auto begin_copy_search(SessionRecord& session, PaneRuntime& runtime,
                                     const CopySearchDirection direction,
                                     const vt::TerminalPoint anchor) noexcept -> bool {
  auto& search = session.attachment_runtime.copy_mode;
  auto first = anchor;
  const bool has_first = advance_copy_search_cursor(runtime, first, direction);
  if (!has_first) {
    const auto boundary = copy_search_boundary(runtime, direction);
    if (!boundary.has_value()) {
      return false;
    }
    first = *boundary;
  }
  search.search_task = CopySearchTask{
      .cursor = {.candidate = first},
      .stop_before = anchor,
      .deadline = reactor_now(),
      .direction = direction,
      .terminal_generation = runtime.mutation_generation,
      .wrapped = !has_first,
  };
  search.preview_match = false;
  session.attachment.copy_mode.feedback = CopyModeFeedback::none;
  invalidate_copy_presentation(session, runtime);
  return true;
}

enum class CopySelectionResult : std::uint8_t {
  applied,
  no_effect,
  unavailable,
  failed,
};

[[nodiscard]] constexpr auto
command_status_for_copy_selection(const CopySelectionResult result) noexcept -> CommandStatus {
  switch (result) {
  case CopySelectionResult::applied:
    return CommandStatus::applied;
  case CopySelectionResult::no_effect:
    return CommandStatus::no_effect;
  case CopySelectionResult::unavailable:
    return CommandStatus::unavailable;
  case CopySelectionResult::failed:
    return CommandStatus::failed;
  }
  return CommandStatus::failed;
}

[[nodiscard]] constexpr auto copy_selection_result(const CopyModeFeedback feedback) noexcept
    -> CopySelectionResult {
  switch (feedback) {
  case CopyModeFeedback::empty_selection:
    return CopySelectionResult::no_effect;
  case CopyModeFeedback::clipboard_busy:
  case CopyModeFeedback::too_large:
    return CopySelectionResult::unavailable;
  case CopyModeFeedback::none:
  case CopyModeFeedback::no_match:
  case CopyModeFeedback::failed:
    return CopySelectionResult::failed;
  }
  return CopySelectionResult::failed;
}

[[nodiscard]] auto copy_selection_to_outer_clipboard(SessionRecord& session, PaneRuntime& runtime,
                                                     PaneRuntimeStore& runtimes) noexcept
    -> CopySelectionResult {
  constexpr std::size_t osc_overhead = 9;
  const auto fail = [&](const CopyModeFeedback feedback) noexcept {
    if (session.attachment.copy_mode.active()) {
      session.attachment.copy_mode.feedback = feedback;
      invalidate_copy_presentation(session, runtime);
    }
    return copy_selection_result(feedback);
  };
  if (session.attachment_runtime.clipboard_write.bytes != nullptr) {
    return fail(CopyModeFeedback::clipboard_busy);
  }
  if (session.attachment_runtime.frame.capacity() <= osc_overhead) {
    return fail(CopyModeFeedback::failed);
  }
  const auto payload_groups = (session.attachment_runtime.frame.capacity() - osc_overhead) / 4U;
  const auto delivery_capacity = std::min(payload_groups * 3U, limits::selection_format_bytes_max);
  auto storage = allocate_clipboard_storage(delivery_capacity);
  if (storage == nullptr) {
    return fail(CopyModeFeedback::failed);
  }
  const auto formatted = runtime.terminal.format_selection(
      vt::ScreenFormat::plain, std::span(storage.get(), delivery_capacity));
  if (!formatted.has_value()) {
    return fail(formatted.error() == vt::Error::out_of_space ? CopyModeFeedback::too_large
                                                             : CopyModeFeedback::failed);
  }
  if (*formatted == 0) {
    return fail(CopyModeFeedback::empty_selection);
  }
  if (clipboard_base64_bytes(*formatted) + osc_overhead >
      session.attachment_runtime.frame.capacity()) {
    return fail(CopyModeFeedback::too_large);
  }
  session.attachment_runtime.clipboard_write.bytes = std::move(storage);
  session.attachment_runtime.clipboard_write.size = *formatted;
  if (session.attachment.copy_mode.active()) {
    leave_copy_mode(session, runtimes);
  } else {
    schedule_frame(session, FrameUrgency::interactive, false);
  }
  return CopySelectionResult::applied;
}

enum class CopyEscapeStatus : std::uint8_t {
  pending,
  complete,
  unsupported,
  invalid,
};

struct CopyEscapeDecode final {
  CopyEscapeStatus status{CopyEscapeStatus::invalid};
  CopyKey key;
};

// Escape-sequence grammar deliberately keeps validation and mapping in one bounded pass.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto decode_copy_escape(const std::span<const std::byte> sequence) noexcept
    -> CopyEscapeDecode {
  LEMMA_ASSERT(!sequence.empty() && sequence.front() == std::byte{0x1B});
  if (sequence.size() == 1U) {
    return {.status = CopyEscapeStatus::pending, .key = {}};
  }
  const auto second = sequence.subspan(1, 1).front();
  if (second != std::byte{'['} && second != std::byte{'O'}) {
    return {.status = CopyEscapeStatus::invalid, .key = {}};
  }
  if (sequence.size() == 2U) {
    return {.status = CopyEscapeStatus::pending, .key = {}};
  }

  const auto final_byte = std::to_integer<std::uint8_t>(sequence.back());
  if (final_byte < 0x40U || final_byte > 0x7EU) {
    const bool continuation = (final_byte >= 0x20U && final_byte <= 0x3FU);
    return {.status = continuation && sequence.size() < copy_escape_bytes_max
                          ? CopyEscapeStatus::pending
                          : CopyEscapeStatus::unsupported,
            .key = {}};
  }

  if (sequence.size() == 3U) {
    switch (final_byte) {
    case static_cast<std::uint8_t>('A'):
      return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::arrow_up}};
    case static_cast<std::uint8_t>('B'):
      return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::arrow_down}};
    case static_cast<std::uint8_t>('C'):
      return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::arrow_right}};
    case static_cast<std::uint8_t>('D'):
      return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::arrow_left}};
    case static_cast<std::uint8_t>('H'):
      return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::home}};
    case static_cast<std::uint8_t>('F'):
      return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::end}};
    default:
      return {.status = CopyEscapeStatus::unsupported, .key = {}};
    }
  }

  if (second != std::byte{'['} || sequence.size() != 4U || final_byte != 0x7EU) {
    return {.status = CopyEscapeStatus::unsupported, .key = {}};
  }
  const auto parameter = std::to_integer<std::uint8_t>(sequence.subspan(2, 1).front());
  switch (parameter) {
  case static_cast<std::uint8_t>('1'):
  case static_cast<std::uint8_t>('7'):
    return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::home}};
  case static_cast<std::uint8_t>('4'):
  case static_cast<std::uint8_t>('8'):
    return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::end}};
  case static_cast<std::uint8_t>('5'):
    return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::page_up}};
  case static_cast<std::uint8_t>('6'):
    return {.status = CopyEscapeStatus::complete, .key = {.kind = CopyKeyKind::page_down}};
  default:
    return {.status = CopyEscapeStatus::unsupported, .key = {}};
  }
}

[[nodiscard]] auto restore_copy_search_selection(SessionRecord& session,
                                                 PaneRuntime& runtime) noexcept -> bool {
  auto& search = session.attachment_runtime.copy_mode;
  const auto restored = runtime.terminal.restore_selection_checkpoint();
  if (!restored.has_value() || !*restored || !search.search_restore_viewport_offset.has_value()) {
    return false;
  }
  runtime.terminal.scroll_viewport(
      vt::ViewportScroll::row, static_cast<std::int64_t>(*search.search_restore_viewport_offset));
  record_terminal_observation(runtime);
  return update_copy_viewport_offset(session, runtime);
}

void reset_copy_search_task(CopyModeRuntimeState& search) noexcept {
  search.search_task.reset();
  search.preview_match = false;
}

void discard_copy_search_restore(PaneRuntime& runtime, CopyModeRuntimeState& search) noexcept {
  runtime.terminal.clear_selection_checkpoint();
  search.search_restore_viewport_offset.reset();
}

[[nodiscard]] auto begin_copy_search_prompt(SessionRecord& session, PaneRuntime& runtime,
                                            const CopySearchDirection direction) noexcept -> bool {
  if (!reactor_status_line()) {
    return false;
  }
  const auto endpoint = runtime.terminal.selection_endpoint(vt::PointSpace::screen);
  const auto viewport = runtime.terminal.viewport_state();
  const auto checkpointed = runtime.terminal.checkpoint_selection();
  if (!endpoint.has_value() || !endpoint->has_value() || !viewport.has_value() ||
      !checkpointed.has_value() || !*checkpointed) {
    return false;
  }
  auto& state = session.attachment.copy_mode;
  auto& search = session.attachment_runtime.copy_mode;
  if (state.phase == CopyModePhase::searching) {
    state.phase_before_search = CopyModePhase::navigation;
  } else if (state.phase != CopyModePhase::search_prompt) {
    state.phase_before_search = state.phase;
  }
  state.phase = CopyModePhase::search_prompt;
  state.prompt_search_direction = direction;
  state.draft_query_size = 0;
  state.feedback = CopyModeFeedback::none;
  reset_copy_search_task(search);
  search.search_restore_viewport_offset = viewport->offset;
  invalidate_copy_presentation(session, runtime);
  return true;
}

[[nodiscard]] auto restart_incremental_copy_search(SessionRecord& session,
                                                   PaneRuntime& runtime) noexcept -> bool {
  auto& state = session.attachment.copy_mode;
  auto& search = session.attachment_runtime.copy_mode;
  reset_copy_search_task(search);
  state.feedback = CopyModeFeedback::none;
  if (state.draft_query_size == 0) {
    if (!restore_copy_search_selection(session, runtime)) {
      return false;
    }
    invalidate_copy_presentation(session, runtime);
    return true;
  }
  // Keep the previous preview installed while the refined query is searched. The tracked
  // checkpoint remains the stable search origin, so no temporary restore frame can flash the
  // original copy cursor between previews.
  const auto endpoint = runtime.terminal.selection_checkpoint_endpoint(vt::PointSpace::screen);
  if (!endpoint.has_value() || !endpoint->has_value()) {
    return false;
  }
  return begin_copy_search(session, runtime, state.prompt_search_direction, **endpoint);
}

[[nodiscard]] auto cancel_copy_search(SessionRecord& session, PaneRuntime& runtime) noexcept
    -> bool {
  auto& state = session.attachment.copy_mode;
  auto& search = session.attachment_runtime.copy_mode;
  const bool prompt = state.phase == CopyModePhase::search_prompt;
  reset_copy_search_task(search);
  if (!restore_copy_search_selection(session, runtime)) {
    return false;
  }
  discard_copy_search_restore(runtime, search);
  state.phase = prompt ? state.phase_before_search : CopyModePhase::navigation;
  if (prompt) {
    state.draft_query_size = 0;
  }
  state.feedback = CopyModeFeedback::none;
  invalidate_copy_presentation(session, runtime);
  return true;
}

void commit_draft_copy_query(CopyModeState& state) noexcept {
  std::ranges::copy(std::span(state.draft_query).first(state.draft_query_size),
                    state.query.begin());
  state.query_size = state.draft_query_size;
  state.search_direction = state.prompt_search_direction;
}

[[nodiscard]] auto commit_copy_search_prompt(SessionRecord& session, PaneRuntime& runtime) noexcept
    -> bool {
  auto& state = session.attachment.copy_mode;
  auto& search = session.attachment_runtime.copy_mode;
  if (state.draft_query_size == 0) {
    return cancel_copy_search(session, runtime);
  }
  commit_draft_copy_query(state);
  if (search.preview_match) {
    const auto selected = runtime.terminal.selection_range(vt::PointSpace::screen);
    if (!selected.has_value() || !selected->has_value()) {
      return false;
    }
    search.last_search_match =
        vt::SearchMatch{.start = (**selected).start, .end = (**selected).end};
    search.last_search_generation = runtime.mutation_generation;
    reset_copy_search_task(search);
    discard_copy_search_restore(runtime, search);
    state.phase = CopyModePhase::navigation;
    state.feedback = CopyModeFeedback::none;
    invalidate_copy_presentation(session, runtime);
    return true;
  }
  if (search.search_task.has_value()) {
    search.last_search_match.reset();
    search.last_search_generation = 0;
    state.phase = CopyModePhase::searching;
    invalidate_copy_presentation(session, runtime);
    return true;
  }
  search.last_search_match.reset();
  search.last_search_generation = 0;
  discard_copy_search_restore(runtime, search);
  state.phase = CopyModePhase::navigation;
  state.feedback = CopyModeFeedback::no_match;
  invalidate_copy_presentation(session, runtime);
  return true;
}

[[nodiscard]] auto begin_repeated_copy_search(SessionRecord& session, PaneRuntime& runtime,
                                              const CopySearchDirection direction) noexcept
    -> bool {
  auto& state = session.attachment.copy_mode;
  auto& search = session.attachment_runtime.copy_mode;
  if (state.query_size == 0) {
    return true;
  }
  const auto endpoint = runtime.terminal.selection_endpoint(vt::PointSpace::screen);
  const auto viewport = runtime.terminal.viewport_state();
  const auto checkpointed = runtime.terminal.checkpoint_selection();
  if (!endpoint.has_value() || !endpoint->has_value() || !viewport.has_value() ||
      !checkpointed.has_value() || !*checkpointed) {
    return false;
  }
  auto anchor = **endpoint;
  if (search.last_search_generation == runtime.mutation_generation &&
      search.last_search_match.has_value()) {
    anchor = search.last_search_match->start;
  }
  reset_copy_search_task(search);
  search.search_restore_viewport_offset = viewport->offset;
  state.phase = CopyModePhase::searching;
  return begin_copy_search(session, runtime, direction, anchor);
}

[[nodiscard]] auto set_copy_visual_phase(SessionRecord& session, PaneRuntime& runtime,
                                         const CopyModePhase phase) noexcept -> bool {
  auto& state = session.attachment.copy_mode;
  if (state.phase == phase) {
    const auto collapsed = runtime.terminal.collapse_selection_to_endpoint();
    if (!collapsed.has_value() || !*collapsed) {
      return false;
    }
    state.phase = CopyModePhase::navigation;
  } else {
    const auto collapsed = runtime.terminal.collapse_selection_to_endpoint();
    const auto selected =
        collapsed.has_value() && *collapsed
            ? runtime.terminal.selection_set_unit(copy_selection_unit(phase))
            : std::expected<bool, vt::Error>{std::unexpected(vt::Error::invalid_state)};
    if (!selected.has_value() || !*selected) {
      return false;
    }
    state.phase = phase;
  }
  state.feedback = CopyModeFeedback::none;
  invalidate_copy_presentation(session, runtime);
  return true;
}

[[nodiscard]] constexpr auto copy_motion(const CopyActionKind kind) noexcept
    -> std::optional<vt::SelectionAdjustment> {
  switch (kind) {
  case CopyActionKind::move_left:
    return vt::SelectionAdjustment::left;
  case CopyActionKind::move_down:
    return vt::SelectionAdjustment::down;
  case CopyActionKind::move_up:
    return vt::SelectionAdjustment::up;
  case CopyActionKind::move_right:
    return vt::SelectionAdjustment::right;
  case CopyActionKind::word_left:
    return vt::SelectionAdjustment::word_left;
  case CopyActionKind::word_right:
    return vt::SelectionAdjustment::word_right;
  case CopyActionKind::word_end:
    return vt::SelectionAdjustment::word_end;
  case CopyActionKind::line_start:
    return vt::SelectionAdjustment::beginning_of_line;
  case CopyActionKind::line_first_nonblank:
    return vt::SelectionAdjustment::first_nonblank;
  case CopyActionKind::line_end:
    return vt::SelectionAdjustment::end_of_line;
  case CopyActionKind::history_top:
    return vt::SelectionAdjustment::history_top;
  case CopyActionKind::history_bottom:
    return vt::SelectionAdjustment::history_bottom;
  case CopyActionKind::viewport_top:
    return vt::SelectionAdjustment::viewport_top;
  case CopyActionKind::viewport_middle:
    return vt::SelectionAdjustment::viewport_middle;
  case CopyActionKind::viewport_bottom:
    return vt::SelectionAdjustment::viewport_bottom;
  case CopyActionKind::half_page_up:
    return vt::SelectionAdjustment::half_page_up;
  case CopyActionKind::half_page_down:
    return vt::SelectionAdjustment::half_page_down;
  case CopyActionKind::page_up:
    return vt::SelectionAdjustment::page_up;
  case CopyActionKind::page_down:
    return vt::SelectionAdjustment::page_down;
  case CopyActionKind::none:
  case CopyActionKind::leave:
  case CopyActionKind::cancel_selection:
  case CopyActionKind::visual_character:
  case CopyActionKind::visual_line:
  case CopyActionKind::visual_block:
  case CopyActionKind::swap_endpoint:
  case CopyActionKind::copy:
  case CopyActionKind::begin_search_forward:
  case CopyActionKind::begin_search_backward:
  case CopyActionKind::repeat_search:
  case CopyActionKind::reverse_search:
  case CopyActionKind::cancel_search:
  case CopyActionKind::commit_search:
  case CopyActionKind::query_backspace:
  case CopyActionKind::query_append:
    return std::nullopt;
  }
  return std::nullopt;
}

void pop_copy_query_codepoint(CopyModeState& state) noexcept {
  if (state.draft_query_size == 0) {
    return;
  }
  --state.draft_query_size;
  while (state.draft_query_size > 0) {
    const auto value = static_cast<std::uint8_t>(static_cast<unsigned char>(
        std::span(state.draft_query).subspan(state.draft_query_size, 1).front()));
    if ((value & 0xC0U) != 0x80U) {
      break;
    }
    --state.draft_query_size;
  }
}

// Returns false only when the action leaves copy mode or terminal state can no longer support it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto apply_copy_action(SessionRecord& session, PaneRuntime& runtime,
                                     PaneRuntimeStore& runtimes, const CopyAction action) noexcept
    -> bool {
  auto& state = session.attachment.copy_mode;
  if (const auto motion = copy_motion(action.kind); motion.has_value()) {
    return adjust_copy_selection(session, runtime, runtimes, *motion) && state.active();
  }
  switch (action.kind) {
  case CopyActionKind::none:
    return true;
  case CopyActionKind::leave:
    leave_copy_mode(session, runtimes);
    return false;
  case CopyActionKind::cancel_selection: {
    const auto collapsed = runtime.terminal.collapse_selection_to_endpoint();
    if (!collapsed.has_value() || !*collapsed) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    state.phase = CopyModePhase::navigation;
    state.feedback = CopyModeFeedback::none;
    invalidate_copy_presentation(session, runtime);
    return true;
  }
  case CopyActionKind::visual_character:
  case CopyActionKind::visual_line:
  case CopyActionKind::visual_block: {
    auto phase = CopyModePhase::visual_block;
    if (action.kind == CopyActionKind::visual_character) {
      phase = CopyModePhase::visual_character;
    } else if (action.kind == CopyActionKind::visual_line) {
      phase = CopyModePhase::visual_line;
    }
    if (!set_copy_visual_phase(session, runtime, phase)) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    return true;
  }
  case CopyActionKind::swap_endpoint: {
    if (!state.selecting()) {
      return true;
    }
    const auto swapped = runtime.terminal.swap_selection_endpoints();
    if (!swapped.has_value() || !*swapped) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    invalidate_copy_presentation(session, runtime);
    return true;
  }
  case CopyActionKind::copy:
    return copy_selection_to_outer_clipboard(session, runtime, runtimes) !=
           CopySelectionResult::applied;
  case CopyActionKind::begin_search_forward:
  case CopyActionKind::begin_search_backward:
    if (!reactor_status_line()) {
      return true;
    }
    if (!begin_copy_search_prompt(session, runtime,
                                  action.kind == CopyActionKind::begin_search_forward
                                      ? CopySearchDirection::forward
                                      : CopySearchDirection::backward)) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    return true;
  case CopyActionKind::repeat_search:
  case CopyActionKind::reverse_search: {
    auto direction = state.search_direction;
    if (action.kind == CopyActionKind::reverse_search) {
      direction = state.search_direction == CopySearchDirection::forward
                      ? CopySearchDirection::backward
                      : CopySearchDirection::forward;
    }
    if (!begin_repeated_copy_search(session, runtime, direction)) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    return true;
  }
  case CopyActionKind::cancel_search:
    if (!cancel_copy_search(session, runtime)) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    return true;
  case CopyActionKind::commit_search:
    if (!commit_copy_search_prompt(session, runtime)) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    return true;
  case CopyActionKind::query_backspace:
    pop_copy_query_codepoint(state);
    if (!restart_incremental_copy_search(session, runtime)) {
      leave_copy_mode(session, runtimes);
      return false;
    }
    return true;
  case CopyActionKind::query_append:
    if (state.draft_query_size < state.draft_query.size()) {
      std::span(state.draft_query).subspan(state.draft_query_size, 1).front() =
          static_cast<char>(action.byte);
      ++state.draft_query_size;
      if (!restart_incremental_copy_search(session, runtime)) {
        leave_copy_mode(session, runtimes);
        return false;
      }
    }
    return true;
  case CopyActionKind::move_left:
  case CopyActionKind::move_down:
  case CopyActionKind::move_up:
  case CopyActionKind::move_right:
  case CopyActionKind::word_left:
  case CopyActionKind::word_right:
  case CopyActionKind::word_end:
  case CopyActionKind::line_start:
  case CopyActionKind::line_first_nonblank:
  case CopyActionKind::line_end:
  case CopyActionKind::history_top:
  case CopyActionKind::history_bottom:
  case CopyActionKind::viewport_top:
  case CopyActionKind::viewport_middle:
  case CopyActionKind::viewport_bottom:
  case CopyActionKind::half_page_up:
  case CopyActionKind::half_page_down:
  case CopyActionKind::page_up:
  case CopyActionKind::page_down:
    break;
  }
  return true;
}

// Command translation uses validated enum values and fixed lookup storage.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] auto copy_action_from_input(const input::InputCommand command) noexcept
    -> std::optional<CopyActionKind> {
  if (command == input::InputCommand::copy_selection) {
    return CopyActionKind::copy;
  }
  if (command == input::InputCommand::copy_cancel_or_leave) {
    return std::nullopt;
  }
  if (command == input::InputCommand::enter_copy_search_forward) {
    return CopyActionKind::begin_search_forward;
  }
  if (command == input::InputCommand::enter_copy_search_backward) {
    return CopyActionKind::begin_search_backward;
  }
  constexpr std::array actions{
      CopyActionKind::leave,
      CopyActionKind::cancel_selection,
      CopyActionKind::move_left,
      CopyActionKind::move_down,
      CopyActionKind::move_up,
      CopyActionKind::move_right,
      CopyActionKind::word_left,
      CopyActionKind::word_right,
      CopyActionKind::word_end,
      CopyActionKind::line_start,
      CopyActionKind::line_first_nonblank,
      CopyActionKind::line_end,
      CopyActionKind::history_top,
      CopyActionKind::history_bottom,
      CopyActionKind::viewport_top,
      CopyActionKind::viewport_middle,
      CopyActionKind::viewport_bottom,
      CopyActionKind::half_page_up,
      CopyActionKind::half_page_down,
      CopyActionKind::page_up,
      CopyActionKind::page_down,
      CopyActionKind::visual_character,
      CopyActionKind::visual_line,
      CopyActionKind::visual_block,
      CopyActionKind::swap_endpoint,
      CopyActionKind::repeat_search,
      CopyActionKind::reverse_search,
      CopyActionKind::cancel_search,
      CopyActionKind::commit_search,
      CopyActionKind::query_backspace,
  };
  if (command < input::InputCommand::copy_leave ||
      command > input::InputCommand::copy_query_backspace) {
    return std::nullopt;
  }
  const auto index =
      static_cast<std::size_t>(command) - static_cast<std::size_t>(input::InputCommand::copy_leave);
  return actions.at(index);
}

[[nodiscard]] auto copy_action_for_state(const CopyModeState& state,
                                         const input::InputCommand command) noexcept
    -> std::optional<CopyActionKind> {
  if (command != input::InputCommand::copy_cancel_or_leave) {
    return copy_action_from_input(command);
  }
  return state.selecting() ? CopyActionKind::cancel_selection : CopyActionKind::leave;
}

[[nodiscard]] auto copy_input_context(const CopyModePhase phase) noexcept
    -> input::ConfiguredInputContext {
  if (phase == CopyModePhase::search_prompt) {
    return input::ConfiguredInputContext::copy_search;
  }
  if (phase == CopyModePhase::searching) {
    return input::ConfiguredInputContext::copy_searching;
  }
  return input::ConfiguredInputContext::copy;
}

[[nodiscard]] auto copy_key_event(const CopyKey key, std::array<std::byte, 1>& text) noexcept
    -> input::KeyEvent {
  input::KeyEvent event{.action = input::KeyAction::press,
                        .key = input::PhysicalKey::unidentified,
                        .modifiers = 0,
                        .unshifted_codepoint = 0,
                        .text = {}};
  switch (key.kind) {
  case CopyKeyKind::byte:
    if (key.byte == 0x0DU) {
      event.key = input::PhysicalKey::enter;
    } else if (key.byte == 0x09U) {
      event.key = input::PhysicalKey::tab;
    } else if (key.byte >= 1U && key.byte <= 26U) {
      event.modifiers = input::key_modifier_control;
      event.unshifted_codepoint =
          static_cast<std::uint32_t>('a') + static_cast<std::uint32_t>(key.byte - 1U);
    } else {
      text.front() = static_cast<std::byte>(key.byte);
      event.unshifted_codepoint = key.byte;
      event.text = text;
    }
    break;
  case CopyKeyKind::escape:
    event.key = input::PhysicalKey::escape;
    break;
  case CopyKeyKind::enter:
    event.key = input::PhysicalKey::enter;
    break;
  case CopyKeyKind::backspace:
    event.key = input::PhysicalKey::backspace;
    break;
  case CopyKeyKind::arrow_up:
    event.key = input::PhysicalKey::arrow_up;
    break;
  case CopyKeyKind::arrow_down:
    event.key = input::PhysicalKey::arrow_down;
    break;
  case CopyKeyKind::arrow_left:
    event.key = input::PhysicalKey::arrow_left;
    break;
  case CopyKeyKind::arrow_right:
    event.key = input::PhysicalKey::arrow_right;
    break;
  case CopyKeyKind::home:
    event.key = input::PhysicalKey::home;
    break;
  case CopyKeyKind::end:
    event.key = input::PhysicalKey::end;
    break;
  case CopyKeyKind::page_up:
    event.key = input::PhysicalKey::page_up;
    break;
  case CopyKeyKind::page_down:
    event.key = input::PhysicalKey::page_down;
    break;
  }
  return event;
}

template <typename Effect>
[[nodiscard]] auto queue_hosted_command(SessionRecord& session, const Effect& effect) noexcept
    -> bool {
  const auto* const command = std::get_if<input::RoutedHostedCommand>(&effect);
  if (command == nullptr) {
    return false;
  }
  auto& runtime = session.attachment_runtime;
  auto* const tab = active_tab(session);
  if (runtime.hosted_command.has_value() || tab == nullptr) {
    publish_status_message(session, StatusMessageKind::error, "Error: Extension capacity reached");
    return true;
  }
  runtime.hosted_command =
      AttachmentRuntime::PendingHostedCommand{.context = {.session = session.id,
                                                          .tab = tab->id,
                                                          .pane = tab->focused_pane,
                                                          .connection = runtime.connection_id},
                                              .index = command->index};
  return true;
}

[[nodiscard]] auto route_copy_event(SessionRecord& session, PaneRuntimeStore& runtimes,
                                    input::KeyEvent event,
                                    const std::optional<std::uint8_t> query_byte) noexcept -> bool {
  auto* const runtime = copy_mode_runtime(session, runtimes);
  if (runtime == nullptr) {
    leave_copy_mode(session, runtimes);
    return false;
  }
  session.interaction_router.select_base(copy_input_context(session.attachment.copy_mode.phase));
  const auto routed = session.interaction_router.route_key(event);
  event.action = input::KeyAction::release;
  static_cast<void>(session.interaction_router.route_key(event));
  if (const auto* const command = std::get_if<input::RoutedCommand>(&routed.effect);
      command != nullptr) {
    const auto action = copy_action_for_state(session.attachment.copy_mode, command->command);
    const bool active =
        !action.has_value() || apply_copy_action(session, *runtime, runtimes, {.kind = *action});
    if (active && session.attachment.copy_mode.active()) {
      session.interaction_router.select_base(
          copy_input_context(session.attachment.copy_mode.phase));
    }
    return active;
  }
  if (std::holds_alternative<input::ForwardCurrentKey>(routed.effect) &&
      session.attachment.copy_mode.phase == CopyModePhase::search_prompt &&
      query_byte.has_value() && *query_byte >= 0x20U) {
    const bool active = apply_copy_action(
        session, *runtime, runtimes, {.kind = CopyActionKind::query_append, .byte = *query_byte});
    if (active && session.attachment.copy_mode.active()) {
      session.interaction_router.select_base(
          copy_input_context(session.attachment.copy_mode.phase));
    }
    return active;
  }
  static_cast<void>(queue_hosted_command(session, routed.effect));
  return session.attachment.copy_mode.active();
}

[[nodiscard]] auto route_copy_key(SessionRecord& session, PaneRuntimeStore& runtimes,
                                  const CopyKey key) noexcept -> bool {
  std::array<std::byte, 1> text{};
  const auto event = copy_key_event(key, text);
  const auto query_byte = key.kind == CopyKeyKind::byte ? std::optional{key.byte} : std::nullopt;
  return route_copy_event(session, runtimes, event, query_byte);
}

// Legacy byte input and structured keys converge on the compiled interaction policy.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto process_copy_mode_input(SessionRecord& session, PaneRuntimeStore& runtimes,
                                           const std::span<const std::byte> input) noexcept
    -> std::size_t {
  auto* runtime = copy_mode_runtime(session, runtimes);
  if (runtime == nullptr) {
    leave_copy_mode(session, runtimes);
    return input.size();
  }
  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto byte = input.subspan(index, 1).front();
    auto key = CopyKey{.byte = std::to_integer<std::uint8_t>(byte)};
    if (session.attachment_runtime.copy_mode.pending_escape_size > 0) {
      LEMMA_ASSERT(session.attachment_runtime.copy_mode.pending_escape_size <
                   session.attachment_runtime.copy_mode.pending_escape.size());
      std::span(session.attachment_runtime.copy_mode.pending_escape)
          .subspan(session.attachment_runtime.copy_mode.pending_escape_size, 1)
          .front() = byte;
      ++session.attachment_runtime.copy_mode.pending_escape_size;
      const auto decoded =
          decode_copy_escape(std::span(session.attachment_runtime.copy_mode.pending_escape)
                                 .first(session.attachment_runtime.copy_mode.pending_escape_size));
      if (decoded.status == CopyEscapeStatus::pending) {
        session.attachment_runtime.copy_mode.pending_escape_deadline =
            reactor_now() + copy_escape_flush_delay;
        continue;
      }
      session.attachment_runtime.copy_mode.pending_escape_size = 0;
      if (decoded.status == CopyEscapeStatus::complete) {
        key = decoded.key;
      } else if (decoded.status == CopyEscapeStatus::unsupported) {
        continue;
      } else {
        if (!route_copy_key(session, runtimes, CopyKey{.kind = CopyKeyKind::escape})) {
          return index;
        }
        key = CopyKey{.byte = std::to_integer<std::uint8_t>(byte)};
      }
    } else if (byte == std::byte{0x1B}) {
      session.attachment_runtime.copy_mode.pending_escape.front() = byte;
      session.attachment_runtime.copy_mode.pending_escape_size = 1;
      session.attachment_runtime.copy_mode.pending_escape_deadline =
          reactor_now() + copy_escape_flush_delay;
      continue;
    }
    if (!route_copy_key(session, runtimes, key)) {
      return index + 1U;
    }
    runtime = copy_mode_runtime(session, runtimes);
    if (runtime == nullptr) {
      leave_copy_mode(session, runtimes);
      return input.size();
    }
  }
  return input.size();
}

[[nodiscard]] auto routed_key_event(const protocol::KeyInput& key,
                                    std::span<const std::byte> text) noexcept -> input::KeyEvent;

void process_typed_copy_mode_input(SessionRecord& session, PaneRuntimeStore& runtimes,
                                   const protocol::KeyInput& key,
                                   const std::span<const std::byte> text) noexcept {
  if (key.action == protocol::KeyInputAction::release) {
    return;
  }
  std::optional<std::uint8_t> query_byte;
  if (text.size() == 1U) {
    query_byte = std::to_integer<std::uint8_t>(text.front());
  } else if (key.key == protocol::KeyInputKey::space) {
    query_byte = static_cast<std::uint8_t>(' ');
  }
  static_cast<void>(route_copy_event(session, runtimes, routed_key_event(key, text), query_byte));
}

void service_copy_input_timeout(SessionRecord& session, PaneRuntimeStore& runtimes,
                                const std::chrono::steady_clock::time_point now) noexcept {
  if (session.attachment.copy_mode.active() &&
      session.attachment_runtime.copy_mode.pending_escape_size > 0 &&
      now >= session.attachment_runtime.copy_mode.pending_escape_deadline) {
    session.attachment_runtime.copy_mode.pending_escape_size = 0;
    static_cast<void>(route_copy_key(session, runtimes, CopyKey{.kind = CopyKeyKind::escape}));
  }
}

[[nodiscard]] auto fit_tab_to_viewport(SessionRecord& session, Tab& tab,
                                       PaneRuntimeStore& runtimes) noexcept -> bool {
  const render::PaneRectangle viewport{
      .columns = session.attachment.columns,
      .rows = pane_rows(session.attachment.rows),
  };
  if (!tab.layout.project(viewport).has_value()) {
    tab.layout_suspended = true;
    return true;
  }
  const auto previous_suspended = tab.layout_suspended;
  const auto previous_column = tab.layout_column;
  const auto previous_row = tab.layout_row;
  const auto previous_columns = tab.layout_columns;
  const auto previous_rows = tab.layout_rows;
  tab.layout_suspended = false;
  tab.layout_column = viewport.column;
  tab.layout_row = viewport.row;
  tab.layout_columns = viewport.columns;
  tab.layout_rows = viewport.rows;
  if (resolve_session_layout(session, tab, runtimes)) {
    return true;
  }
  if (session.active) {
    tab.layout_suspended = previous_suspended;
    tab.layout_column = previous_column;
    tab.layout_row = previous_row;
    tab.layout_columns = previous_columns;
    tab.layout_rows = previous_rows;
  }
  return false;
}

[[nodiscard]] auto select_tab(SessionRecord& session, PaneRuntimeStore& runtimes,
                              const TabId id) noexcept -> bool {
  auto* const selected = find_tab(session, id);
  if (selected == nullptr) {
    return false;
  }
  if (session.active_tab == id) {
    return true;
  }
  const auto previous_active = session.active_tab;
  const auto previous_previous = session.previous_tab;
  session.previous_tab = session.active_tab;
  session.active_tab = id;
  if (!fit_tab_to_viewport(session, *selected, runtimes)) {
    if (session.active) {
      session.active_tab = previous_active;
      session.previous_tab = previous_previous;
    }
    return false;
  }
  schedule_frame(session, FrameUrgency::state_change, true);
  return true;
}

void cycle_tab(SessionRecord& session, PaneRuntimeStore& runtimes, const bool forward) noexcept {
  const auto current = session.tab_order.position_of(session.active_tab);
  if (!current.has_value() || session.tab_order.size() <= 1U) {
    return;
  }
  const auto count = session.tab_order.size();
  const auto candidate = forward ? (*current + 1U) % count : (*current + count - 1U) % count;
  const auto id = session.tab_order.at(candidate);
  LEMMA_ASSERT(id.has_value());
  static_cast<void>(select_tab(session, runtimes, *id));
}

void erase_tab_panes(SessionRecord& session, const Tab& tab, PaneRuntimeStore& runtimes) noexcept {
  for (auto& pane_slot : session.panes) {
    if (pane_slot.pane == nullptr || pane_slot.pane->tab != tab.id) {
      continue;
    }
    const bool erased = runtimes.erase(pane_address(session, *pane_slot.pane));
    LEMMA_ASSERT(erased);
    pane_slot.pane.reset();
  }
}

void reset_removed_tab_attachment_state(SessionRecord& session, PaneRuntimeStore& runtimes,
                                        const TabId id) noexcept {
  if (session.attachment.selection_target.has_value() &&
      session.attachment.selection_target->tab == id) {
    leave_copy_mode(session, runtimes);
  }
  if (session.attachment.rename_prompt.kind == RenamePromptKind::tab &&
      session.attachment.rename_prompt.tab == id) {
    reset_rename_prompt(session, false);
  }
  if (session.attachment.mouse_capture.has_value() &&
      session.attachment.mouse_capture->owner == MouseCaptureOwner::status_tab &&
      (session.attachment.mouse_capture->target.tab == id ||
       session.attachment.mouse_capture->status_tab_before == id)) {
    session.attachment.mouse_capture.reset();
  }
}

void remove_tab(SessionRecord& session, PaneRuntimeStore& runtimes, const TabId id) noexcept {
  auto* const tab = find_tab(session, id);
  const auto removed_position = session.tab_order.position_of(id);
  if (tab == nullptr || !removed_position.has_value()) {
    return;
  }
  reset_removed_tab_attachment_state(session, runtimes, id);
  erase_tab_panes(session, *tab, runtimes);
  std::span(session.tabs).subspan(id.slot(), 1).front().tab.reset();
  const bool order_erased = session.tab_order.erase(id);
  LEMMA_ASSERT(order_erased);
  record_session_mutation(session);
  if (tab_count(session) == 0) {
    session.active = false;
    return;
  }
  if (session.active_tab != id) {
    if (session.previous_tab == id) {
      session.previous_tab = session.active_tab;
    }
    schedule_frame(session, FrameUrgency::state_change, false);
    return;
  }
  const auto next_position = std::min(*removed_position, session.tab_order.size() - 1U);
  const auto selected_id = session.tab_order.at(next_position);
  LEMMA_ASSERT(selected_id.has_value());
  auto* const selected = find_tab(session, *selected_id);
  LEMMA_ASSERT(selected != nullptr);
  session.active_tab = *selected_id;
  session.previous_tab = session.active_tab;
  if (!fit_tab_to_viewport(session, *selected, runtimes)) {
    if (session.active) {
      selected->layout_suspended = true;
      schedule_frame(session, FrameUrgency::state_change, true);
    }
    return;
  }
  schedule_frame(session, FrameUrgency::state_change, true);
}

[[nodiscard]] auto create_tab(SessionRecord& session, PaneRuntimeStore& runtimes,
                              const std::span<const std::byte> launch_command = {},
                              const std::string_view working_directory = {},
                              const PaneExitPolicy exit_policy = PaneExitPolicy::close,
                              const bool activate = true) noexcept -> Tab* {
  ProductionSessionRuntimeContext runtime_context{.session = &session, .runtimes = &runtimes};
  SessionMachine machine(session, production_session_options(runtime_context));
  const auto command = launch_command.empty() ? reactor_default_program() : launch_command;
  const auto directory = working_directory.empty() ? reactor_default_cwd() : working_directory;
  const auto transition = machine.create_tab({.command = command,
                                              .working_directory = directory,
                                              .exit_policy = exit_policy,
                                              .activate = activate});
  apply_session_change(session, runtimes, transition.change);
  return transition.result.status == CommandStatus::applied
             ? find_tab(session, transition.created_tab)
             : nullptr;
}

// Runtime creation and all dependent resize effects complete before Core publishes the new Pane.
[[nodiscard]] auto split_pane(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                              const PaneId source_pane, const SplitAxis axis,
                              const std::span<const std::byte> launch_command = {},
                              const std::string_view working_directory = {},
                              const PaneExitPolicy exit_policy = PaneExitPolicy::close,
                              const bool focus_created = true) noexcept -> Pane* {
  ProductionSessionRuntimeContext runtime_context{.session = &session, .runtimes = &runtimes};
  SessionMachine machine(session, production_session_options(runtime_context));
  const auto command = launch_command.empty() ? reactor_default_program() : launch_command;
  const auto directory = working_directory.empty() ? reactor_default_cwd() : working_directory;
  const auto transition = machine.split_pane(tab.id, source_pane, axis,
                                             {.command = command,
                                              .working_directory = directory,
                                              .exit_policy = exit_policy,
                                              .focus_created = focus_created});
  apply_session_change(session, runtimes, transition.change);
  return transition.result.status == CommandStatus::applied
             ? find_pane(session, transition.created_pane)
             : nullptr;
}

[[nodiscard]] auto close_pane(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                              const PaneId pane_id) noexcept -> bool {
  auto* const pane = find_pane(session, tab, pane_id);
  if (pane == nullptr) {
    return false;
  }
  if (session.attachment.selection_target ==
      std::optional{AttachmentPaneTarget{.tab = tab.id, .pane = pane_id}}) {
    leave_copy_mode(session, runtimes);
  }
  const auto pane_index = static_cast<std::size_t>(pane_id.slot());
  const bool was_focused = pane_id == tab.focused_pane;
  if (pane_count(tab) == 1) {
    remove_tab(session, runtimes, tab.id);
    return true;
  }
  auto proposed_layout = tab.layout;
  const auto focus_candidate = proposed_layout.remove(pane_id);
  if (!focus_candidate.has_value()) {
    return false;
  }
  // Process teardown is irreversible, so publish the already-valid reduced topology before
  // removing its runtime counterpart. Any later external resize rejection suspends this layout.
  tab.layout = proposed_layout;
  const bool runtime_erased = runtimes.erase(pane_address(session, *pane));
  LEMMA_ASSERT(runtime_erased);
  std::span(session.panes).subspan(pane_index, 1).front().pane.reset();
  record_session_mutation(session);
  if (was_focused) {
    tab.focused_pane = *focus_candidate;
  }
  if (tab.previous_pane == pane_id || find_pane(session, tab, tab.previous_pane) == nullptr) {
    tab.previous_pane = tab.focused_pane;
  }
  tab.zoomed = false;
  if (!resolve_session_layout(session, tab, runtimes)) {
    if (session.active) {
      tab.layout_suspended = true;
      schedule_frame(session, FrameUrgency::state_change, true);
      return true;
    }
    return false;
  }
  schedule_frame(session, FrameUrgency::state_change, true);
  return true;
}

void focus_pane(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                const PaneId pane_id) noexcept {
  if (pane_id == tab.focused_pane || find_pane(session, tab, pane_id) == nullptr) {
    return;
  }
  const auto previous_focused = tab.focused_pane;
  const auto previous_previous = tab.previous_pane;
  tab.previous_pane = tab.focused_pane;
  tab.focused_pane = pane_id;
  if (tab.zoomed && !resolve_session_layout(session, tab, runtimes)) {
    if (session.active) {
      tab.focused_pane = previous_focused;
      tab.previous_pane = previous_previous;
    }
    return;
  }
  auto* const focused = find_pane(session, tab, tab.focused_pane);
  LEMMA_ASSERT(focused != nullptr);
  auto* const focused_runtime = find_pane_runtime(runtimes, session, tab, *focused);
  LEMMA_ASSERT(focused_runtime != nullptr);
  focused_runtime->terminal.invalidate_ansi_render_state();
  schedule_frame(session, FrameUrgency::state_change, tab.zoomed);
}

void focus_next(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                const PaneId source_pane) noexcept {
  for (std::size_t offset = 1; offset <= session.panes.size(); ++offset) {
    const auto candidate =
        (static_cast<std::size_t>(source_pane.slot()) + offset) % session.panes.size();
    // candidate is reduced modulo the fixed Session pane capacity.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto& pane = session.panes[candidate].pane;
    if (pane != nullptr && pane->tab == tab.id) {
      focus_pane(session, tab, runtimes, pane->id);
      return;
    }
  }
}

enum class FocusDirection : std::uint8_t {
  left,
  right,
  up,
  down,
};

// Focus and swap share one spatial-neighbor rule so modifier changes never retarget a different
// pane. Directional scoring handles each axis explicitly and remains bounded by pane capacity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto pane_in_direction(const SessionRecord& session, const Tab& tab,
                                     const PaneId source_pane,
                                     const FocusDirection direction) noexcept
    -> std::optional<PaneId> {
  // Zoom resizes focused panes to the viewport, so derive stable tiled geometry from the tree.
  const render::PaneRectangle viewport{
      .column = tab.layout_column,
      .row = tab.layout_row,
      .columns = tab.layout_columns,
      .rows = tab.layout_rows,
  };
  const auto projection = tab.layout.project(viewport);
  const auto current_rectangle =
      projection.has_value() ? projection->rectangle(source_pane) : std::nullopt;
  if (!projection.has_value() || !current_rectangle.has_value()) {
    return std::nullopt;
  }
  const auto& rectangles = projection->rectangles;
  const auto current = *current_rectangle;
  const auto current_right = static_cast<std::uint32_t>(current.column) + current.columns;
  const auto current_bottom = static_cast<std::uint32_t>(current.row) + current.rows;
  const auto current_x = (static_cast<std::uint32_t>(current.column) * 2U) + current.columns;
  const auto current_y = (static_cast<std::uint32_t>(current.row) * 2U) + current.rows;
  std::uint64_t best_score = std::numeric_limits<std::uint64_t>::max();
  std::optional<PaneId> best;
  for (std::size_t index = 0; index < session.panes.size(); ++index) {
    // index is bounded by the fixed Session pane capacity.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto& candidate = session.panes[index].pane;
    if (candidate == nullptr || candidate->tab != tab.id || candidate->id == source_pane) {
      continue;
    }
    const auto& candidate_rectangle = std::span(rectangles).subspan(index, 1).front();
    const auto right =
        static_cast<std::uint32_t>(candidate_rectangle.column) + candidate_rectangle.columns;
    const auto bottom =
        static_cast<std::uint32_t>(candidate_rectangle.row) + candidate_rectangle.rows;
    const auto x =
        (static_cast<std::uint32_t>(candidate_rectangle.column) * 2U) + candidate_rectangle.columns;
    const auto y =
        (static_cast<std::uint32_t>(candidate_rectangle.row) * 2U) + candidate_rectangle.rows;
    bool eligible = false;
    std::uint32_t primary = 0;
    std::uint32_t secondary = 0;
    switch (direction) {
    case FocusDirection::left:
      eligible = right <= current.column;
      primary = eligible ? current.column - right : 0;
      secondary = y > current_y ? y - current_y : current_y - y;
      break;
    case FocusDirection::right:
      eligible = candidate_rectangle.column >= current_right;
      primary = eligible ? candidate_rectangle.column - current_right : 0;
      secondary = y > current_y ? y - current_y : current_y - y;
      break;
    case FocusDirection::up:
      eligible = bottom <= current.row;
      primary = eligible ? current.row - bottom : 0;
      secondary = x > current_x ? x - current_x : current_x - x;
      break;
    case FocusDirection::down:
      eligible = candidate_rectangle.row >= current_bottom;
      primary = eligible ? candidate_rectangle.row - current_bottom : 0;
      secondary = x > current_x ? x - current_x : current_x - x;
      break;
    }
    const auto score = (static_cast<std::uint64_t>(primary) * 4'096U) + secondary;
    if (eligible && score < best_score) {
      best_score = score;
      best = candidate->id;
    }
  }
  return best;
}

void focus_direction(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                     const PaneId source_pane, const FocusDirection direction) noexcept {
  if (const auto target = pane_in_direction(session, tab, source_pane, direction);
      target.has_value()) {
    focus_pane(session, tab, runtimes, *target);
  }
}

[[nodiscard]] auto commit_layout_resize(SessionRecord& session, Tab& tab,
                                        PaneRuntimeStore& runtimes,
                                        const PaneLayout& proposed_layout) noexcept
    -> CommandResult {
  if (!resolve_session_layout(session, tab, runtimes, &proposed_layout)) {
    if (session.active) {
      // A rejected compensating transaction may still have reflowed terminals out and back.
      // Repair presentation without publishing the proposed ratio.
      schedule_frame(session, FrameUrgency::state_change, true);
      return {.status = CommandStatus::unavailable};
    }
    return {.status = CommandStatus::failed};
  }
  schedule_frame(session, FrameUrgency::state_change, true);
  return {.status = CommandStatus::applied};
}

[[nodiscard]] auto resize_split(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                                const PaneId pane, const ResizeDirection direction,
                                const std::uint16_t amount = 1) noexcept -> CommandResult {
  if (tab.zoomed || tab.layout_suspended) {
    return {.status = CommandStatus::unavailable};
  }
  auto proposed_layout = tab.layout;
  const render::PaneRectangle viewport{
      .column = tab.layout_column,
      .row = tab.layout_row,
      .columns = tab.layout_columns,
      .rows = tab.layout_rows,
  };
  const auto edit = proposed_layout.resize(pane, direction, viewport, amount);
  switch (edit) {
  case LayoutResizeStatus::no_effect:
    return {.status = CommandStatus::no_effect};
  case LayoutResizeStatus::unavailable:
    return {.status = CommandStatus::unavailable};
  case LayoutResizeStatus::invalid:
    LEMMA_ASSERT(false && "PaneLayout rejected an authoritative pane target");
  case LayoutResizeStatus::applied:
    break;
  }
  return commit_layout_resize(session, tab, runtimes, proposed_layout);
}

[[nodiscard]] auto resize_split_divider(SessionRecord& session, Tab& tab,
                                        PaneRuntimeStore& runtimes, const LayoutDivider divider,
                                        const std::uint16_t coordinate) noexcept -> CommandResult {
  if (tab.zoomed || tab.layout_suspended) {
    return {.status = CommandStatus::unavailable};
  }
  auto proposed_layout = tab.layout;
  const render::PaneRectangle viewport{
      .column = tab.layout_column,
      .row = tab.layout_row,
      .columns = tab.layout_columns,
      .rows = tab.layout_rows,
  };
  const auto edit = proposed_layout.resize_divider(divider, coordinate, viewport);
  switch (edit) {
  case LayoutResizeStatus::no_effect:
    return {.status = CommandStatus::no_effect};
  case LayoutResizeStatus::unavailable:
    return {.status = CommandStatus::unavailable};
  case LayoutResizeStatus::invalid:
    return {.status = CommandStatus::stale_target};
  case LayoutResizeStatus::applied:
    break;
  }
  return commit_layout_resize(session, tab, runtimes, proposed_layout);
}

template <typename Value>
[[nodiscard]] auto command_payload_value(const CommandPayload& payload) noexcept -> const Value& {
  const auto* const value = std::get_if<Value>(&payload);
  LEMMA_ASSERT(value != nullptr);
  return *value;
}

[[nodiscard]] auto swap_panes(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                              const PaneId first, const PaneId second) noexcept -> CommandResult {
  if (tab.zoomed || tab.layout_suspended) {
    return {.status = CommandStatus::unavailable};
  }
  auto proposed = tab.layout;
  if (!proposed.swap(first, second)) {
    return {.status = CommandStatus::stale_target};
  }
  return commit_layout_resize(session, tab, runtimes, proposed);
}

[[nodiscard]] constexpr auto command_status(const bool changed) noexcept -> CommandResult {
  return {.status = changed ? CommandStatus::applied : CommandStatus::no_effect};
}

// Resolving relative input intent into stable IDs is deliberately centralized at the input/Core
// bridge rather than represented as another semantic mutation primitive.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto command_from_input(const SessionRecord& session,
                                      const input::InputCommand input_command,
                                      const CommandOrigin origin) noexcept
    -> std::optional<Command> {
  Command command{.origin = origin};
  switch (input_command) {
  case input::InputCommand::detach:
    command.kind = CommandKind::detach_client;
    break;
  case input::InputCommand::split_left_right:
    command.kind = CommandKind::split_left_right;
    break;
  case input::InputCommand::split_top_bottom:
    command.kind = CommandKind::split_top_bottom;
    break;
  case input::InputCommand::resize_left:
    command.kind = CommandKind::resize_left;
    break;
  case input::InputCommand::resize_right:
    command.kind = CommandKind::resize_right;
    break;
  case input::InputCommand::resize_up:
    command.kind = CommandKind::resize_up;
    break;
  case input::InputCommand::resize_down:
    command.kind = CommandKind::resize_down;
    break;
  case input::InputCommand::focus_left:
    command.kind = CommandKind::focus_left;
    break;
  case input::InputCommand::focus_right:
    command.kind = CommandKind::focus_right;
    break;
  case input::InputCommand::focus_up:
    command.kind = CommandKind::focus_up;
    break;
  case input::InputCommand::focus_down:
    command.kind = CommandKind::focus_down;
    break;
  case input::InputCommand::focus_next:
    command.kind = CommandKind::focus_next;
    break;
  case input::InputCommand::focus_previous:
    command.kind = CommandKind::focus_previous;
    break;
  case input::InputCommand::close_pane:
    command.kind = CommandKind::close_pane;
    break;
  case input::InputCommand::toggle_zoom:
    command.kind = CommandKind::toggle_zoom;
    break;
  case input::InputCommand::enter_copy_mode:
    command.kind = CommandKind::enter_copy_mode;
    break;
  case input::InputCommand::enter_copy_search_forward:
    command.kind = CommandKind::enter_copy_search_forward;
    break;
  case input::InputCommand::enter_copy_search_backward:
    command.kind = CommandKind::enter_copy_search_backward;
    break;
  case input::InputCommand::copy_selection:
    command.kind = CommandKind::copy_selection;
    break;
  case input::InputCommand::create_tab:
    command.kind = CommandKind::create_tab;
    break;
  case input::InputCommand::next_tab:
    command.kind = CommandKind::next_tab;
    break;
  case input::InputCommand::previous_tab:
    command.kind = CommandKind::previous_tab;
    break;
  case input::InputCommand::begin_rename_session:
    command.kind = CommandKind::begin_rename_session;
    command.target = {.session = session.id,
                      .tab = {},
                      .pane = {},
                      .peer_pane = {},
                      .attachment = session.attachment.id};
    break;
  case input::InputCommand::begin_rename_tab:
    command.kind = CommandKind::begin_rename_tab;
    command.target = {.session = session.id,
                      .tab = session.active_tab,
                      .pane = {},
                      .peer_pane = {},
                      .attachment = session.attachment.id};
    break;
  case input::InputCommand::move_tab_left:
  case input::InputCommand::move_tab_right: {
    const auto position = session.tab_order.position_of(session.active_tab);
    if (!position.has_value() || session.tab_order.size() <= 1U) {
      return std::nullopt;
    }
    command.kind = CommandKind::place_tab;
    command.target = {.session = session.id,
                      .tab = session.active_tab,
                      .pane = {},
                      .peer_pane = {},
                      .attachment = {}};
    const auto position_value = position.value_or(0);
    TabId before;
    if (input_command == input::InputCommand::move_tab_left) {
      if (position_value > 0) {
        before = session.tab_order.at(position_value - 1U).value_or(TabId{});
      }
    } else if (position_value + 1U == session.tab_order.size()) {
      before = session.tab_order.at(0).value_or(TabId{});
    } else if (position_value + 2U < session.tab_order.size()) {
      before = session.tab_order.at(position_value + 2U).value_or(TabId{});
    }
    command.payload = TabPlacementCommand{.before = before};
    break;
  }
  case input::InputCommand::swap_pane_left:
  case input::InputCommand::swap_pane_right:
  case input::InputCommand::swap_pane_up:
  case input::InputCommand::swap_pane_down: {
    auto direction = FocusDirection::left;
    if (input_command == input::InputCommand::swap_pane_right) {
      direction = FocusDirection::right;
    } else if (input_command == input::InputCommand::swap_pane_up) {
      direction = FocusDirection::up;
    } else if (input_command == input::InputCommand::swap_pane_down) {
      direction = FocusDirection::down;
    }
    const auto* const tab = active_tab(session);
    const auto other = tab == nullptr
                           ? std::nullopt
                           : pane_in_direction(session, *tab, tab->focused_pane, direction);
    if (tab == nullptr || !other.has_value()) {
      return std::nullopt;
    }
    command.kind = CommandKind::swap_panes;
    command.target = {.session = session.id,
                      .tab = tab->id,
                      .pane = tab->focused_pane,
                      .peer_pane = {},
                      .attachment = {}};
    command.payload = PaneSwapCommand{.other = *other};
    break;
  }
  case input::InputCommand::close_tab:
    command.kind = CommandKind::close_tab;
    break;
  case input::InputCommand::select_tab_0:
  case input::InputCommand::select_tab_1:
  case input::InputCommand::select_tab_2:
  case input::InputCommand::select_tab_3:
  case input::InputCommand::select_tab_4:
  case input::InputCommand::select_tab_5:
  case input::InputCommand::select_tab_6:
  case input::InputCommand::select_tab_7:
  case input::InputCommand::select_tab_8:
  case input::InputCommand::select_tab_9: {
    command.kind = CommandKind::select_tab;
    const auto encoded = static_cast<std::uint8_t>(input_command);
    const auto first = static_cast<std::uint8_t>(input::InputCommand::select_tab_0);
    const auto logical = static_cast<std::uint16_t>(encoded - first);
    command.payload = CommandCoordinate{
        .value = logical == 0U ? std::uint16_t{9} : static_cast<std::uint16_t>(logical - 1U)};
    break;
  }
  case input::InputCommand::begin_command_line:
  case input::InputCommand::show_messages:
  case input::InputCommand::message_view_leave:
  case input::InputCommand::message_view_previous:
  case input::InputCommand::message_view_next:
  case input::InputCommand::message_view_page_previous:
  case input::InputCommand::message_view_page_next:
  case input::InputCommand::message_view_history_start:
  case input::InputCommand::message_view_history_end:
  case input::InputCommand::command_line_cancel:
  case input::InputCommand::command_line_commit:
  case input::InputCommand::command_line_complete:
  case input::InputCommand::command_line_history_previous:
  case input::InputCommand::command_line_history_next:
  case input::InputCommand::command_line_backspace:
  case input::InputCommand::command_line_delete:
  case input::InputCommand::command_line_cursor_left:
  case input::InputCommand::command_line_cursor_right:
  case input::InputCommand::command_line_cursor_home:
  case input::InputCommand::command_line_cursor_end:
  case input::InputCommand::command_line_clear:
  case input::InputCommand::command_line_delete_word:
  case input::InputCommand::copy_cancel_or_leave:
  case input::InputCommand::copy_leave:
  case input::InputCommand::copy_cancel_selection:
  case input::InputCommand::copy_move_left:
  case input::InputCommand::copy_move_down:
  case input::InputCommand::copy_move_up:
  case input::InputCommand::copy_move_right:
  case input::InputCommand::copy_word_left:
  case input::InputCommand::copy_word_right:
  case input::InputCommand::copy_word_end:
  case input::InputCommand::copy_line_start:
  case input::InputCommand::copy_line_first_nonblank:
  case input::InputCommand::copy_line_end:
  case input::InputCommand::copy_history_top:
  case input::InputCommand::copy_history_bottom:
  case input::InputCommand::copy_viewport_top:
  case input::InputCommand::copy_viewport_middle:
  case input::InputCommand::copy_viewport_bottom:
  case input::InputCommand::copy_half_page_up:
  case input::InputCommand::copy_half_page_down:
  case input::InputCommand::copy_page_up:
  case input::InputCommand::copy_page_down:
  case input::InputCommand::copy_visual_character:
  case input::InputCommand::copy_visual_line:
  case input::InputCommand::copy_visual_block:
  case input::InputCommand::copy_swap_endpoint:
  case input::InputCommand::copy_repeat_search:
  case input::InputCommand::copy_reverse_search:
  case input::InputCommand::copy_cancel_search:
  case input::InputCommand::copy_commit_search:
  case input::InputCommand::copy_query_backspace:
  case input::InputCommand::rename_cancel:
  case input::InputCommand::rename_commit:
  case input::InputCommand::rename_backspace:
  case input::InputCommand::rename_delete:
  case input::InputCommand::rename_cursor_left:
  case input::InputCommand::rename_cursor_right:
  case input::InputCommand::rename_cursor_home:
  case input::InputCommand::rename_cursor_end:
  case input::InputCommand::rename_clear:
  case input::InputCommand::rename_delete_word:
  case input::InputCommand::count:
    return std::nullopt;
  }
  return command;
}

[[nodiscard]] constexpr auto pane_input_command(const protocol::PaneCommand command) noexcept
    -> std::optional<input::InputCommand> {
  using enum input::InputCommand;
  switch (command) {
  case protocol::PaneCommand::none:
    return std::nullopt;
  case protocol::PaneCommand::split_left_right:
    return split_left_right;
  case protocol::PaneCommand::split_top_bottom:
    return split_top_bottom;
  case protocol::PaneCommand::resize_left:
    return resize_left;
  case protocol::PaneCommand::resize_right:
    return resize_right;
  case protocol::PaneCommand::resize_up:
    return resize_up;
  case protocol::PaneCommand::resize_down:
    return resize_down;
  case protocol::PaneCommand::focus_left:
    return focus_left;
  case protocol::PaneCommand::focus_right:
    return focus_right;
  case protocol::PaneCommand::focus_up:
    return focus_up;
  case protocol::PaneCommand::focus_down:
    return focus_down;
  case protocol::PaneCommand::focus_next:
    return focus_next;
  case protocol::PaneCommand::focus_previous:
    return focus_previous;
  case protocol::PaneCommand::close:
    return close_pane;
  case protocol::PaneCommand::zoom:
    return toggle_zoom;
  case protocol::PaneCommand::enter_copy_mode:
    return enter_copy_mode;
  case protocol::PaneCommand::enter_copy_search_forward:
    return enter_copy_search_forward;
  case protocol::PaneCommand::enter_copy_search_backward:
    return enter_copy_search_backward;
  case protocol::PaneCommand::create_tab:
    return create_tab;
  case protocol::PaneCommand::next_tab:
    return next_tab;
  case protocol::PaneCommand::previous_tab:
    return previous_tab;
  case protocol::PaneCommand::begin_rename_session:
    return begin_rename_session;
  case protocol::PaneCommand::begin_rename_tab:
    return begin_rename_tab;
  case protocol::PaneCommand::move_tab_left:
    return move_tab_left;
  case protocol::PaneCommand::move_tab_right:
    return move_tab_right;
  case protocol::PaneCommand::swap_pane_left:
    return swap_pane_left;
  case protocol::PaneCommand::swap_pane_right:
    return swap_pane_right;
  case protocol::PaneCommand::swap_pane_up:
    return swap_pane_up;
  case protocol::PaneCommand::swap_pane_down:
    return swap_pane_down;
  case protocol::PaneCommand::kill_tab:
    return close_tab;
  case protocol::PaneCommand::select_tab_0:
    return select_tab_0;
  case protocol::PaneCommand::select_tab_1:
    return select_tab_1;
  case protocol::PaneCommand::select_tab_2:
    return select_tab_2;
  case protocol::PaneCommand::select_tab_3:
    return select_tab_3;
  case protocol::PaneCommand::select_tab_4:
    return select_tab_4;
  case protocol::PaneCommand::select_tab_5:
    return select_tab_5;
  case protocol::PaneCommand::select_tab_6:
    return select_tab_6;
  case protocol::PaneCommand::select_tab_7:
    return select_tab_7;
  case protocol::PaneCommand::select_tab_8:
    return select_tab_8;
  case protocol::PaneCommand::select_tab_9:
    return select_tab_9;
  }
  return std::nullopt;
}

[[nodiscard]] auto command_from_pane_command(const SessionRecord& session,
                                             const protocol::PaneCommand pane_command) noexcept
    -> std::optional<Command> {
  const auto input_command = pane_input_command(pane_command);
  return input_command.has_value()
             ? command_from_input(session, *input_command, CommandOrigin::client)
             : std::nullopt;
}

using SessionNameConflict = bool (*)(void* context, SessionId renamed,
                                     std::string_view candidate) noexcept;

struct SessionCommandContext final {
  SessionRecord* session{nullptr};
  PaneRuntimeStore* runtimes{nullptr};
  SessionNameConflict name_conflict{nullptr};
  void* name_conflict_context{nullptr};
};

// This is the only function that translates validated commands into authoritative mux mutations.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto execute_session_command(void* const context, const Command& command) noexcept
    -> CommandResult {
  auto& command_context = *static_cast<SessionCommandContext*>(context);
  LEMMA_ASSERT(command_context.session != nullptr && command_context.runtimes != nullptr);
  auto& session = *command_context.session;
  auto& runtimes = *command_context.runtimes;
  if (command.target.session.is_valid() && command.target.session != session.id) {
    return {.status = command.target.session.slot() == session.id.slot()
                          ? CommandStatus::stale_target
                          : CommandStatus::wrong_owner};
  }
  if (command.target.attachment.is_valid() && command.target.attachment != session.attachment.id) {
    return {.status = command.target.attachment.slot() == session.id.slot()
                          ? CommandStatus::stale_target
                          : CommandStatus::wrong_owner};
  }
  if (session_lifecycle_command(command.kind)) {
    if (session.attachment.copy_mode.active() && command.kind != CommandKind::rename_session &&
        command.kind != CommandKind::stop_session) {
      leave_copy_mode(session, runtimes);
    }
    if (command.kind == CommandKind::swap_panes) {
      finish_live_divider_resize(session);
    }
    ProductionSessionRuntimeContext runtime_context{.session = &session, .runtimes = &runtimes};
    SessionMachine machine(
        session, production_session_options(runtime_context, command_context.name_conflict,
                                            command_context.name_conflict_context));
    const auto transition = machine.dispatch(command);
    LEMMA_ASSERT(transition.handled);
    apply_session_change(session, runtimes, transition.change);
    return transition.result;
  }
  if (command.kind == CommandKind::detach_client) {
    return session.attachment_runtime.client >= 0
               ? CommandResult{.status = CommandStatus::detach_requested}
               : CommandResult{.status = CommandStatus::unavailable};
  }
  if (command.kind == CommandKind::stop_session) {
    const bool changed = session.active;
    session.active = false;
    return command_status(changed);
  }
  if (command.kind == CommandKind::cancel_attachment_interaction) {
    const bool changed =
        session.attachment.copy_mode.active() || session.attachment.rename_prompt.active() ||
        session.attachment.command_line.active || session.attachment.message_view.active;
    leave_copy_mode(session, runtimes);
    reset_rename_prompt(session);
    reset_command_line(session);
    leave_message_view(session);
    return command_status(changed);
  }
  if (command.kind == CommandKind::begin_rename_session) {
    if (session.attachment.copy_mode.active()) {
      leave_copy_mode(session, runtimes);
    }
    return begin_rename_prompt(session, RenamePromptKind::session, {}, {})
               ? CommandResult{.status = CommandStatus::applied}
               : CommandResult{.status = CommandStatus::unavailable};
  }
  if (command.kind == CommandKind::rename_session) {
    const auto& name = command_payload_value<SessionNameValue>(command.payload);
    if (session.session_name() == name.view()) {
      return {.status = CommandStatus::no_effect};
    }
    if (command_context.name_conflict == nullptr) {
      return {.status = CommandStatus::unavailable};
    }
    if (command_context.name_conflict(command_context.name_conflict_context, session.id,
                                      name.view())) {
      return {.status = CommandStatus::conflict};
    }
    const bool renamed = session.rename(name.view());
    LEMMA_ASSERT(renamed);
    reset_rename_prompt(session, false);
    session.attachment_runtime.status_valid = false;
    schedule_frame(session, FrameUrgency::state_change, false);
    return {.status = CommandStatus::applied};
  }

  auto* const tab =
      command.target.tab.is_valid() ? find_tab(session, command.target.tab) : active_tab(session);
  if (tab == nullptr) {
    return {.status = command.target.tab.is_valid() ? CommandStatus::stale_target
                                                    : CommandStatus::failed};
  }
  if (command.kind == CommandKind::begin_rename_tab) {
    if (session.attachment.copy_mode.active()) {
      leave_copy_mode(session, runtimes);
    }
    return begin_rename_prompt(session, RenamePromptKind::tab, tab->id,
                               tab_title(session, *tab, runtimes))
               ? CommandResult{.status = CommandStatus::applied}
               : CommandResult{.status = CommandStatus::unavailable};
  }
  auto* const targeted_pane = command.target.pane.is_valid()
                                  ? find_pane(session, *tab, command.target.pane)
                                  : find_pane(session, *tab, tab->focused_pane);
  if (targeted_pane == nullptr) {
    return {.status = command.target.pane.is_valid() ? CommandStatus::stale_target
                                                     : CommandStatus::failed};
  }
  const bool divider_resize = command.kind == CommandKind::resize_left_right_divider ||
                              command.kind == CommandKind::resize_top_bottom_divider;
  auto* const peer_pane =
      divider_resize ? find_pane(session, *tab, command.target.peer_pane) : nullptr;
  if (divider_resize && peer_pane == nullptr) {
    return {.status = CommandStatus::stale_target};
  }

  const auto focus_result = [&](const PaneId previous) {
    return session.active ? command_status(tab->focused_pane != previous)
                          : CommandResult{.status = CommandStatus::failed};
  };
  const bool copy_command = command.kind == CommandKind::enter_copy_mode ||
                            command.kind == CommandKind::enter_copy_search_forward ||
                            command.kind == CommandKind::enter_copy_search_backward ||
                            command.kind == CommandKind::copy_selection;
  if (session.attachment.copy_mode.active() && !copy_command) {
    leave_copy_mode(session, runtimes);
  }
  switch (command.kind) {
  case CommandKind::none:
  case CommandKind::detach_client:
  case CommandKind::cancel_attachment_interaction:
  case CommandKind::begin_rename_session:
  case CommandKind::begin_rename_tab:
  case CommandKind::rename_session:
  case CommandKind::stop_session:
    return {.status = CommandStatus::invalid_command};
  case CommandKind::split_left_right:
    if (pane_count(session) >= panes_per_session_max) {
      return {.status = CommandStatus::capacity};
    }
    if (split_pane(session, *tab, runtimes, targeted_pane->id, SplitAxis::left_right) != nullptr) {
      return {.status = CommandStatus::applied};
    }
    return {.status = session.active ? CommandStatus::unavailable : CommandStatus::failed};
  case CommandKind::split_top_bottom:
    if (pane_count(session) >= panes_per_session_max) {
      return {.status = CommandStatus::capacity};
    }
    if (split_pane(session, *tab, runtimes, targeted_pane->id, SplitAxis::top_bottom) != nullptr) {
      return {.status = CommandStatus::applied};
    }
    return {.status = session.active ? CommandStatus::unavailable : CommandStatus::failed};
  case CommandKind::resize_left_right_divider:
    LEMMA_ASSERT(peer_pane != nullptr);
    return resize_split_divider(
        session, *tab, runtimes,
        {.first = targeted_pane->id, .second = peer_pane->id, .axis = SplitAxis::left_right},
        command_payload_value<CommandCoordinate>(command.payload).value);
  case CommandKind::resize_top_bottom_divider:
    LEMMA_ASSERT(peer_pane != nullptr);
    return resize_split_divider(
        session, *tab, runtimes,
        {.first = targeted_pane->id, .second = peer_pane->id, .axis = SplitAxis::top_bottom},
        command_payload_value<CommandCoordinate>(command.payload).value);
  case CommandKind::resize_left:
  case CommandKind::resize_right:
  case CommandKind::resize_up:
  case CommandKind::resize_down: {
    const auto* const requested = std::get_if<CommandCoordinate>(&command.payload);
    const auto amount = requested == nullptr ? std::uint16_t{1} : requested->value;
    if (amount == 0) {
      return {.status = CommandStatus::invalid_command};
    }
    auto direction = ResizeDirection::left;
    if (command.kind == CommandKind::resize_right) {
      direction = ResizeDirection::right;
    } else if (command.kind == CommandKind::resize_up) {
      direction = ResizeDirection::up;
    } else if (command.kind == CommandKind::resize_down) {
      direction = ResizeDirection::down;
    }
    return resize_split(session, *tab, runtimes, targeted_pane->id, direction, amount);
  }
  case CommandKind::focus_left: {
    const auto previous = tab->focused_pane;
    focus_direction(session, *tab, runtimes, targeted_pane->id, FocusDirection::left);
    return focus_result(previous);
  }
  case CommandKind::focus_right: {
    const auto previous = tab->focused_pane;
    focus_direction(session, *tab, runtimes, targeted_pane->id, FocusDirection::right);
    return focus_result(previous);
  }
  case CommandKind::focus_up: {
    const auto previous = tab->focused_pane;
    focus_direction(session, *tab, runtimes, targeted_pane->id, FocusDirection::up);
    return focus_result(previous);
  }
  case CommandKind::focus_down: {
    const auto previous = tab->focused_pane;
    focus_direction(session, *tab, runtimes, targeted_pane->id, FocusDirection::down);
    return focus_result(previous);
  }
  case CommandKind::focus_next: {
    const auto previous = tab->focused_pane;
    focus_next(session, *tab, runtimes, targeted_pane->id);
    return focus_result(previous);
  }
  case CommandKind::focus_previous: {
    const auto previous = tab->focused_pane;
    focus_pane(session, *tab, runtimes, tab->previous_pane);
    return focus_result(previous);
  }
  case CommandKind::focus_pane: {
    const auto previous = tab->focused_pane;
    focus_pane(session, *tab, runtimes, targeted_pane->id);
    return focus_result(previous);
  }
  case CommandKind::close_pane:
    if (close_pane(session, *tab, runtimes, targeted_pane->id)) {
      return {.status = CommandStatus::applied};
    }
    return {.status = session.active ? CommandStatus::unavailable : CommandStatus::failed};
  case CommandKind::toggle_zoom:
  case CommandKind::set_zoom: {
    const auto previous_focused = tab->focused_pane;
    const auto previous_previous = tab->previous_pane;
    const auto previous_zoomed = tab->zoomed;
    const auto* const requested = std::get_if<PaneZoomCommand>(&command.payload);
    const bool desired = requested == nullptr ? !tab->zoomed : requested->enabled;
    if (requested != nullptr && tab->zoomed == desired &&
        (!desired || tab->focused_pane == targeted_pane->id)) {
      return {.status = CommandStatus::no_effect};
    }
    if (desired && targeted_pane->id != tab->focused_pane) {
      tab->previous_pane = tab->focused_pane;
      tab->focused_pane = targeted_pane->id;
    }
    tab->zoomed = desired;
    if (!resolve_session_layout(session, *tab, runtimes)) {
      if (session.active) {
        tab->focused_pane = previous_focused;
        tab->previous_pane = previous_previous;
        tab->zoomed = previous_zoomed;
        return {.status = CommandStatus::unavailable};
      }
      return {.status = CommandStatus::failed};
    }
    schedule_frame(session, FrameUrgency::state_change, true);
    return {.status = CommandStatus::applied};
  }
  case CommandKind::enter_copy_mode:
    if (session.attachment.copy_mode.active()) {
      leave_copy_mode(session, runtimes);
      return {.status = CommandStatus::applied};
    }
    if (auto* const runtime = find_pane_runtime(runtimes, session, *tab, *targeted_pane);
        runtime != nullptr && enter_copy_mode(session, *tab, *targeted_pane, *runtime, runtimes)) {
      return {.status = CommandStatus::applied};
    }
    return {.status = CommandStatus::unavailable};
  case CommandKind::enter_copy_search_forward:
  case CommandKind::enter_copy_search_backward: {
    if (!reactor_status_line()) {
      return {.status = CommandStatus::unavailable};
    }
    auto* runtime = find_pane_runtime(runtimes, session, *tab, *targeted_pane);
    if (runtime == nullptr) {
      return {.status = CommandStatus::unavailable};
    }
    if (!session.attachment.copy_mode.active() &&
        !enter_copy_mode(session, *tab, *targeted_pane, *runtime, runtimes)) {
      return {.status = CommandStatus::unavailable};
    }
    runtime = copy_mode_runtime(session, runtimes);
    const auto direction = command.kind == CommandKind::enter_copy_search_forward
                               ? CopySearchDirection::forward
                               : CopySearchDirection::backward;
    if (runtime == nullptr || !begin_copy_search_prompt(session, *runtime, direction)) {
      leave_copy_mode(session, runtimes);
      return {.status = CommandStatus::unavailable};
    }
    return {.status = CommandStatus::applied};
  }
  case CommandKind::copy_selection: {
    auto* runtime = selection_runtime(session, runtimes);
    if (runtime == nullptr) {
      runtime = find_pane_runtime(runtimes, session, *tab, *targeted_pane);
    }
    if (runtime == nullptr) {
      return {.status = CommandStatus::unavailable};
    }
    const auto active = runtime->terminal.selection_active();
    if (!active.has_value()) {
      return {.status = CommandStatus::failed};
    }
    if (!*active) {
      return {.status = CommandStatus::no_effect};
    }
    return {.status = command_status_for_copy_selection(
                copy_selection_to_outer_clipboard(session, *runtime, runtimes))};
  }
  case CommandKind::rename_tab: {
    const auto& title = command_payload_value<TabTitleValue>(command.payload);
    if (tab->title_override() == title.view()) {
      return {.status = CommandStatus::no_effect};
    }
    const bool renamed = tab->set_title_override(title.view());
    LEMMA_ASSERT(renamed);
    if (session.attachment.rename_prompt.kind == RenamePromptKind::tab &&
        session.attachment.rename_prompt.tab == tab->id) {
      reset_rename_prompt(session, false);
    }
    session.attachment_runtime.status_valid = false;
    schedule_frame(session, FrameUrgency::state_change, false);
    return {.status = CommandStatus::applied};
  }
  case CommandKind::place_tab: {
    const auto before = command_payload_value<TabPlacementCommand>(command.payload).before;
    if (before.is_valid() && find_tab(session, before) == nullptr) {
      return {.status = CommandStatus::stale_target};
    }
    const bool changed = session.tab_order.place_before(
        tab->id, before.is_valid() ? std::optional<TabId>{before} : std::nullopt);
    if (changed) {
      session.attachment_runtime.status_valid = false;
      schedule_frame(session, FrameUrgency::state_change, false);
    }
    return command_status(changed);
  }
  case CommandKind::swap_panes: {
    const auto other_id = command_payload_value<PaneSwapCommand>(command.payload).other;
    auto* const other = find_pane(session, *tab, other_id);
    if (other == nullptr) {
      return {.status = CommandStatus::stale_target};
    }
    finish_live_divider_resize(session);
    return swap_panes(session, *tab, runtimes, targeted_pane->id, other->id);
  }
  case CommandKind::create_tab: {
    if (tab_count(session) >= session.tabs.size() || pane_count(session) >= panes_per_session_max) {
      return {.status = CommandStatus::capacity};
    }
    const auto previous = tab_count(session);
    static_cast<void>(create_tab(session, runtimes));
    if (!session.active) {
      return {.status = CommandStatus::failed};
    }
    return previous == tab_count(session) ? CommandResult{.status = CommandStatus::unavailable}
                                          : CommandResult{.status = CommandStatus::applied};
  }
  case CommandKind::next_tab: {
    const auto previous = session.active_tab;
    cycle_tab(session, runtimes, true);
    return session.active ? command_status(previous != session.active_tab)
                          : CommandResult{.status = CommandStatus::failed};
  }
  case CommandKind::previous_tab: {
    const auto previous = session.active_tab;
    cycle_tab(session, runtimes, false);
    return session.active ? command_status(previous != session.active_tab)
                          : CommandResult{.status = CommandStatus::failed};
  }
  case CommandKind::close_tab:
    remove_tab(session, runtimes, tab->id);
    return {.status = CommandStatus::applied};
  case CommandKind::select_tab: {
    if (!command.target.tab.is_valid()) {
      return {.status = CommandStatus::unavailable};
    }
    const auto previous = session.active_tab;
    if (!select_tab(session, runtimes, command.target.tab)) {
      return {.status = session.active ? CommandStatus::stale_target : CommandStatus::failed};
    }
    return command_status(previous != session.active_tab);
  }
  }
  return {.status = CommandStatus::invalid_command};
}

[[nodiscard]] constexpr auto targets_pane(const CommandKind kind) noexcept -> bool {
  switch (kind) {
  case CommandKind::split_left_right:
  case CommandKind::split_top_bottom:
  case CommandKind::resize_left_right_divider:
  case CommandKind::resize_top_bottom_divider:
  case CommandKind::resize_left:
  case CommandKind::resize_right:
  case CommandKind::resize_up:
  case CommandKind::resize_down:
  case CommandKind::focus_left:
  case CommandKind::focus_right:
  case CommandKind::focus_up:
  case CommandKind::focus_down:
  case CommandKind::focus_next:
  case CommandKind::focus_previous:
  case CommandKind::focus_pane:
  case CommandKind::close_pane:
  case CommandKind::toggle_zoom:
  case CommandKind::set_zoom:
  case CommandKind::enter_copy_mode:
  case CommandKind::enter_copy_search_forward:
  case CommandKind::enter_copy_search_backward:
  case CommandKind::copy_selection:
  case CommandKind::swap_panes:
    return true;
  case CommandKind::none:
  case CommandKind::detach_client:
  case CommandKind::cancel_attachment_interaction:
  case CommandKind::create_tab:
  case CommandKind::next_tab:
  case CommandKind::previous_tab:
  case CommandKind::close_tab:
  case CommandKind::select_tab:
  case CommandKind::begin_rename_session:
  case CommandKind::begin_rename_tab:
  case CommandKind::rename_session:
  case CommandKind::rename_tab:
  case CommandKind::place_tab:
  case CommandKind::stop_session:
    return false;
  }
  return false;
}

// Queues one focus transition for a Pane whose child can still receive input. A full queue keeps
// the previous state so a later reconciliation retries without duplicating a delivered report.
void report_pane_focus(PaneRuntime& runtime, const bool focused) noexcept {
  if (runtime.focus_reported == focused) {
    return;
  }
  if (runtime.accepts_input()) {
    const auto queued_bytes_before = runtime.pending_writes.size();
    const auto queued = queue_focus_input(runtime.pending_writes, runtime.terminal,
                                          focused ? vt::FocusEvent::gained : vt::FocusEvent::lost);
    if (queued == InputQueueResult::full) {
      return;
    }
    if (runtime.pending_writes.size() > queued_bytes_before) {
      runtime.interactive_damage.await_write(queued_bytes_before, runtime.pending_writes.size());
    }
  }
  runtime.focus_reported = focused;
}

// Focus reports derive from committed state rather than from individual transitions: only the
// active Tab's focused Pane of an attached Session with a focused outer terminal has focus. Each
// Pane therefore sees alternating gained/lost reports, queued in order with its other input.
void reconcile_focus_reports(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept {
  if (!session.active) {
    return;
  }
  const auto* const tab =
      session.attachment_runtime.client >= 0 && session.attachment_runtime.outer_focused
          ? active_tab(session)
          : nullptr;
  const auto focused = tab == nullptr ? PaneId{} : tab->focused_pane;
  for (auto& pane_slot : session.panes) {
    auto* const runtime =
        pane_slot.pane == nullptr ? nullptr : find_pane_runtime(runtimes, session, *pane_slot.pane);
    if (runtime != nullptr) {
      report_pane_focus(*runtime, pane_slot.pane->id == focused);
    }
  }
}

// Target completion is the single bounded bridge from implicit client commands to stable IDs.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto dispatch_session_command(SessionRecord& session, PaneRuntimeStore& runtimes,
                                            const Command& command,
                                            const SessionNameConflict name_conflict = nullptr,
                                            void* const name_conflict_context = nullptr) noexcept
    -> CommandResult {
  auto resolved = command;
  if (!resolved.target.session.is_valid()) {
    resolved.target.session = session.id;
  }
  if (resolved.origin == CommandOrigin::client && !resolved.target.attachment.is_valid()) {
    resolved.target.attachment = session.attachment.id;
  }
  if (resolved.kind == CommandKind::select_tab && !resolved.target.tab.is_valid()) {
    const auto* const selected = tab_at_position(
        session,
        static_cast<std::size_t>(command_payload_value<CommandCoordinate>(resolved.payload).value));
    if (selected != nullptr) {
      resolved.target.tab = selected->id;
    }
  } else if (resolved.kind != CommandKind::detach_client &&
             resolved.kind != CommandKind::cancel_attachment_interaction &&
             resolved.kind != CommandKind::begin_rename_session &&
             resolved.kind != CommandKind::rename_session &&
             resolved.kind != CommandKind::stop_session && !resolved.target.tab.is_valid()) {
    resolved.target.tab = session.active_tab;
  }
  if (targets_pane(resolved.kind) && !resolved.target.pane.is_valid()) {
    const auto* const tab = find_tab(session, resolved.target.tab);
    if (tab != nullptr) {
      resolved.target.pane = tab->focused_pane;
    }
  }
  SessionCommandContext context{.session = &session,
                                .runtimes = &runtimes,
                                .name_conflict = name_conflict,
                                .name_conflict_context = name_conflict_context};
  const CommandDispatcher dispatcher(&execute_session_command, &context);
  const auto result = dispatcher.dispatch(resolved);
  if (result.status == CommandStatus::applied && !session_lifecycle_command(resolved.kind)) {
    record_session_mutation(session);
  }
  // Report focus before any later input in the same batch reaches the newly focused Pane.
  reconcile_focus_reports(session, runtimes);
  return result;
}

void invalidate_rename_prompt(SessionRecord& session) noexcept {
  schedule_frame(session, FrameUrgency::state_change, false);
}

[[nodiscard]] auto insert_rename_prompt_text(SessionRecord& session,
                                             const std::span<const std::byte> input) noexcept
    -> bool {
  auto& prompt = session.attachment.rename_prompt;
  if (!prompt.active()) {
    return false;
  }
  const auto capacity = rename_prompt_capacity(prompt.kind);
  if (prompt.size >= capacity) {
    return false;
  }
  // The editor can retain at most one title's worth of bytes. Do not scan a protocol-maximum paste
  // on the reactor thread looking for useful characters after that bounded prefix.
  const auto examined = input.first(std::min(input.size(), limits::tab_title_bytes_max));
  bool changed = false;
  for (const auto encoded : examined) {
    if (prompt.size == capacity) {
      break;
    }
    const auto byte = std::to_integer<std::uint8_t>(encoded);
    if (!rename_prompt_character(prompt.kind, byte)) {
      continue;
    }
    auto text = std::span(prompt.text);
    for (std::size_t index = prompt.size; index > prompt.cursor; --index) {
      text.subspan(index, 1).front() = text.subspan(index - 1U, 1).front();
    }
    text.subspan(prompt.cursor, 1).front() = static_cast<char>(byte);
    ++prompt.cursor;
    ++prompt.size;
    prompt.feedback = RenamePromptFeedback::none;
    changed = true;
  }
  return changed;
}

[[nodiscard]] auto erase_rename_prompt_before_cursor(RenamePromptState& prompt) noexcept -> bool {
  if (prompt.cursor == 0) {
    return false;
  }
  auto text = std::span(prompt.text);
  for (std::size_t index = prompt.cursor; index < prompt.size; ++index) {
    text.subspan(index - 1U, 1).front() = text.subspan(index, 1).front();
  }
  --prompt.cursor;
  --prompt.size;
  std::span(prompt.text).subspan(prompt.size, 1).front() = {};
  prompt.feedback = RenamePromptFeedback::none;
  return true;
}

[[nodiscard]] auto erase_rename_prompt_at_cursor(RenamePromptState& prompt) noexcept -> bool {
  if (prompt.cursor >= prompt.size) {
    return false;
  }
  auto text = std::span(prompt.text);
  for (std::size_t index = prompt.cursor + 1U; index < prompt.size; ++index) {
    text.subspan(index - 1U, 1).front() = text.subspan(index, 1).front();
  }
  --prompt.size;
  std::span(prompt.text).subspan(prompt.size, 1).front() = {};
  prompt.feedback = RenamePromptFeedback::none;
  return true;
}

void clear_rename_prompt(RenamePromptState& prompt) noexcept {
  prompt.text = {};
  prompt.size = 0;
  prompt.cursor = 0;
  prompt.feedback = RenamePromptFeedback::none;
}

void erase_rename_prompt_word(RenamePromptState& prompt) noexcept {
  while (prompt.cursor > 0 &&
         std::span(prompt.text).subspan(prompt.cursor - 1U, 1).front() == ' ') {
    static_cast<void>(erase_rename_prompt_before_cursor(prompt));
  }
  while (prompt.cursor > 0 &&
         std::span(prompt.text).subspan(prompt.cursor - 1U, 1).front() != ' ') {
    static_cast<void>(erase_rename_prompt_before_cursor(prompt));
  }
}

void complete_rename_prompt(SessionRecord& session, PaneRuntimeStore& runtimes,
                            const SessionNameConflict name_conflict,
                            void* const name_conflict_context) noexcept {
  const auto prompt = session.attachment.rename_prompt;
  if (!prompt.active()) {
    return;
  }
  Command command{.origin = CommandOrigin::client};
  command.target = {.session = session.id,
                    .tab = {},
                    .pane = {},
                    .peer_pane = {},
                    .attachment = session.attachment.id};
  if (prompt.kind == RenamePromptKind::session) {
    const auto name = SessionNameValue::create(prompt.view());
    if (!name.has_value()) {
      session.attachment.rename_prompt.feedback = RenamePromptFeedback::invalid;
      invalidate_rename_prompt(session);
      return;
    }
    command.kind = CommandKind::rename_session;
    command.payload = *name;
  } else {
    const auto title = TabTitleValue::create(prompt.view());
    if (!title.has_value()) {
      session.attachment.rename_prompt.feedback = RenamePromptFeedback::invalid;
      invalidate_rename_prompt(session);
      return;
    }
    command.kind = CommandKind::rename_tab;
    command.target.tab = prompt.tab;
    command.payload = *title;
  }
  const auto result =
      dispatch_session_command(session, runtimes, command, name_conflict, name_conflict_context);
  if (result.succeeded()) {
    reset_rename_prompt(session, false);
  } else if (result.status == CommandStatus::conflict) {
    session.attachment.rename_prompt.feedback = RenamePromptFeedback::conflict;
  } else {
    session.attachment.rename_prompt.feedback = RenamePromptFeedback::invalid;
  }
  invalidate_rename_prompt(session);
}

[[nodiscard]] auto apply_rename_input_command(SessionRecord& session, PaneRuntimeStore& runtimes,
                                              const input::InputCommand command,
                                              const SessionNameConflict name_conflict,
                                              void* const name_conflict_context) noexcept -> bool {
  auto& prompt = session.attachment.rename_prompt;
  bool changed = false;
  if (command == input::InputCommand::rename_cancel) {
    reset_rename_prompt(session, false);
    invalidate_rename_prompt(session);
    return false;
  }
  if (command == input::InputCommand::rename_commit) {
    complete_rename_prompt(session, runtimes, name_conflict, name_conflict_context);
    return session.attachment.rename_prompt.active();
  }
  if (command == input::InputCommand::rename_backspace) {
    changed = erase_rename_prompt_before_cursor(prompt);
  } else if (command == input::InputCommand::rename_delete) {
    changed = erase_rename_prompt_at_cursor(prompt);
  } else if (command == input::InputCommand::rename_cursor_left) {
    changed = prompt.cursor > 0U;
    prompt.cursor -= changed ? 1U : 0U;
  } else if (command == input::InputCommand::rename_cursor_right) {
    changed = prompt.cursor < prompt.size;
    prompt.cursor += changed ? 1U : 0U;
  } else if (command == input::InputCommand::rename_cursor_home) {
    changed = prompt.cursor != 0U;
    prompt.cursor = 0U;
  } else if (command == input::InputCommand::rename_cursor_end) {
    changed = prompt.cursor != prompt.size;
    prompt.cursor = prompt.size;
  } else if (command == input::InputCommand::rename_clear) {
    changed = prompt.size != 0U;
    clear_rename_prompt(prompt);
  } else if (command == input::InputCommand::rename_delete_word) {
    const auto before = prompt.size;
    erase_rename_prompt_word(prompt);
    changed = prompt.size != before;
  }
  if (changed) {
    invalidate_rename_prompt(session);
  }
  return prompt.active();
}

auto process_rename_prompt_input(SessionRecord& session, PaneRuntimeStore& runtimes,
                                 const std::span<const std::byte> input,
                                 const SessionNameConflict name_conflict,
                                 void* const name_conflict_context) noexcept -> std::size_t {
  session.interaction_router.select_base(input::ConfiguredInputContext::rename);
  std::size_t offset = 0;
  while (offset < input.size() && session.attachment.rename_prompt.active()) {
    const auto routed =
        session.interaction_router.route_legacy(input.subspan(offset), input.size() - offset);
    if (routed.consumed == 0U || routed.consumed > input.size() - offset) {
      return offset;
    }
    offset += routed.consumed;
    if (const auto* const command = std::get_if<input::RoutedCommand>(&routed.effect);
        command != nullptr) {
      static_cast<void>(apply_rename_input_command(session, runtimes, command->command,
                                                   name_conflict, name_conflict_context));
      continue;
    }
    const auto* const forwarded = std::get_if<input::ForwardLegacyInput>(&routed.effect);
    if (forwarded == nullptr) {
      static_cast<void>(queue_hosted_command(session, routed.effect));
      continue;
    }
    bool changed = false;
    if (forwarded->prefix_size > 0U) {
      changed = insert_rename_prompt_text(
          session, std::span(forwarded->prefix).first(forwarded->prefix_size));
    }
    changed = insert_rename_prompt_text(session, forwarded->current) || changed;
    if (changed) {
      invalidate_rename_prompt(session);
    }
  }
  return offset;
}

// Key policy has already run through the compiled rename context; only forwarded text is edited.
void process_typed_rename_prompt_input(SessionRecord& session,
                                       const std::span<const std::byte> text,
                                       const bool control) noexcept {
  if (!control && !text.empty() && insert_rename_prompt_text(session, text)) {
    invalidate_rename_prompt(session);
  }
}

void invalidate_command_line(SessionRecord& session) noexcept {
  schedule_frame(session, FrameUrgency::state_change, false);
}

// Editor indices are bounded by CommandLineState::size/cursor and fixed history counts.
[[nodiscard]] auto message_view_offset_max(const SessionRecord& session) noexcept -> std::size_t {
  const auto rows = static_cast<std::size_t>(pane_rows(session.attachment.rows));
  const auto size = static_cast<std::size_t>(session.attachment.status_messages.size);
  return size > rows ? size - rows : 0U;
}

void apply_message_view_input_command(SessionRecord& session,
                                      const input::InputCommand command) noexcept {
  auto& view = session.attachment.message_view;
  if (!view.active) {
    return;
  }
  if (command == input::InputCommand::message_view_leave) {
    leave_message_view(session);
    return;
  }
  const auto maximum = message_view_offset_max(session);
  const auto before = static_cast<std::size_t>(view.offset);
  auto offset = before;
  const auto page = std::max<std::size_t>(1U, pane_rows(session.attachment.rows) - 1U);
  if (command == input::InputCommand::message_view_previous) {
    offset = std::min(maximum, offset + 1U);
  } else if (command == input::InputCommand::message_view_next) {
    offset = offset > 0 ? offset - 1U : 0U;
  } else if (command == input::InputCommand::message_view_page_previous) {
    offset = std::min(maximum, offset + page);
  } else if (command == input::InputCommand::message_view_page_next) {
    offset = offset > page ? offset - page : 0U;
  } else if (command == input::InputCommand::message_view_history_start) {
    offset = maximum;
  } else if (command == input::InputCommand::message_view_history_end) {
    offset = 0;
  }
  view.offset = static_cast<std::uint8_t>(offset);
  if (offset != before) {
    schedule_frame(session, FrameUrgency::state_change, true);
  }
}

auto process_message_view_input(SessionRecord& session,
                                const std::span<const std::byte> input) noexcept -> std::size_t {
  session.interaction_router.select_base(input::ConfiguredInputContext::messages);
  std::size_t offset = 0;
  while (offset < input.size() && session.attachment.message_view.active) {
    const auto routed =
        session.interaction_router.route_legacy(input.subspan(offset), input.size() - offset);
    if (routed.consumed == 0 || routed.consumed > input.size() - offset) {
      return offset;
    }
    offset += routed.consumed;
    if (const auto* command = std::get_if<input::RoutedCommand>(&routed.effect);
        command != nullptr) {
      apply_message_view_input_command(session, command->command);
    } else {
      static_cast<void>(queue_hosted_command(session, routed.effect));
    }
  }
  return offset;
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
[[nodiscard]] auto insert_command_line_text(SessionRecord& session,
                                            const std::span<const std::byte> input) noexcept
    -> bool {
  auto& state = session.attachment.command_line;
  if (!state.active || state.submit_requested || state.size == state.text.size()) {
    return false;
  }
  const auto examined = input.first(std::min(input.size(), state.text.size()));
  bool changed = false;
  for (const auto encoded : examined) {
    if (state.size == state.text.size()) {
      break;
    }
    const auto byte = std::to_integer<std::uint8_t>(encoded);
    if (byte < 0x20U || byte > 0x7eU) {
      continue;
    }
    auto text = std::span(state.text);
    for (auto index = state.size; index > state.cursor; --index) {
      text[index] = text[index - 1U];
    }
    text[state.cursor] = static_cast<char>(byte);
    ++state.cursor;
    ++state.size;
    clear_command_line_requests(state);
    changed = true;
  }
  return changed;
}

[[nodiscard]] auto erase_command_line_before_cursor(CommandLineState& state) noexcept -> bool {
  if (state.cursor == 0) {
    return false;
  }
  for (auto index = state.cursor; index < state.size; ++index) {
    state.text[index - 1U] = state.text[index];
  }
  --state.cursor;
  --state.size;
  state.text[state.size] = {};
  clear_command_line_requests(state);
  return true;
}

[[nodiscard]] auto erase_command_line_at_cursor(CommandLineState& state) noexcept -> bool {
  if (state.cursor >= state.size) {
    return false;
  }
  for (auto index = state.cursor + 1U; index < state.size; ++index) {
    state.text[index - 1U] = state.text[index];
  }
  --state.size;
  state.text[state.size] = {};
  clear_command_line_requests(state);
  return true;
}

void clear_command_line(CommandLineState& state) noexcept {
  state.text = {};
  state.size = 0;
  state.cursor = 0;
  clear_command_line_requests(state);
}

void erase_command_line_word(CommandLineState& state) noexcept {
  while (state.cursor > 0 && state.text[state.cursor - 1U] == ' ') {
    static_cast<void>(erase_command_line_before_cursor(state));
  }
  while (state.cursor > 0 && state.text[state.cursor - 1U] != ' ') {
    static_cast<void>(erase_command_line_before_cursor(state));
  }
}

void remember_attachment_command_line(Attachment& attachment) noexcept {
  const auto entered = attachment.command_line.view();
  remember_command_line(attachment.command_history, entered);
  if (active_command_history != nullptr) {
    remember_command_line(*active_command_history, entered);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto apply_command_line_input_command(SessionRecord& session,
                                                    const input::InputCommand command) noexcept
    -> bool {
  auto& state = session.attachment.command_line;
  bool changed = false;
  if (command == input::InputCommand::command_line_cancel) {
    reset_command_line(session, false);
    invalidate_command_line(session);
    return false;
  }
  if (command == input::InputCommand::command_line_commit) {
    if (state.view().empty()) {
      reset_command_line(session);
      return false;
    }
    state.submit_requested = true;
    // A coalesced Tab+Enter packet completes first, then submits the edited line next turn.
    invalidate_command_line(session);
    return true;
  }
  if (command == input::InputCommand::command_line_complete) {
    state.completion_requested = true;
    state.submit_requested = false;
    return true;
  }
  if (command == input::InputCommand::command_line_history_previous ||
      command == input::InputCommand::command_line_history_next) {
    const auto& history = session.attachment.command_history;
    if (history.size == 0) {
      return true;
    }
    auto index = state.history_index;
    if (command == input::InputCommand::command_line_history_previous) {
      index = static_cast<std::uint8_t>(
          std::min<std::size_t>(static_cast<std::size_t>(index) + 1U, history.size));
    } else if (index > 0) {
      --index;
    }
    changed = index != state.history_index;
    state.history_index = index;
    if (index == 0) {
      if (changed) {
        clear_command_line(state);
      }
    } else {
      const auto entry = static_cast<std::size_t>(index) - 1U;
      changed = changed || state.view() != history.entries[entry].view();
      assign_command_line_text(state, history.entries[entry].view());
    }
  } else if (command == input::InputCommand::command_line_backspace) {
    changed = erase_command_line_before_cursor(state);
  } else if (command == input::InputCommand::command_line_delete) {
    changed = erase_command_line_at_cursor(state);
  } else if (command == input::InputCommand::command_line_cursor_left) {
    changed = state.cursor > 0;
    state.cursor -= changed ? 1U : 0U;
  } else if (command == input::InputCommand::command_line_cursor_right) {
    changed = state.cursor < state.size;
    state.cursor += changed ? 1U : 0U;
  } else if (command == input::InputCommand::command_line_cursor_home) {
    changed = state.cursor != 0;
    state.cursor = 0;
  } else if (command == input::InputCommand::command_line_cursor_end) {
    changed = state.cursor != state.size;
    state.cursor = state.size;
  } else if (command == input::InputCommand::command_line_clear) {
    changed = state.size != 0;
    clear_command_line(state);
  } else if (command == input::InputCommand::command_line_delete_word) {
    const auto before = state.size;
    erase_command_line_word(state);
    changed = state.size != before;
  }
  if (changed) {
    invalidate_command_line(session);
  }
  return state.active;
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)

auto process_command_line_input(SessionRecord& session,
                                const std::span<const std::byte> input) noexcept -> std::size_t {
  session.interaction_router.select_base(input::ConfiguredInputContext::command_line);
  std::size_t offset = 0;
  while (offset < input.size() && session.attachment.command_line.active &&
         !session.attachment.command_line.submit_requested) {
    const auto routed =
        session.interaction_router.route_legacy(input.subspan(offset), input.size() - offset);
    if (routed.consumed == 0 || routed.consumed > input.size() - offset) {
      return offset;
    }
    offset += routed.consumed;
    if (const auto* command = std::get_if<input::RoutedCommand>(&routed.effect);
        command != nullptr) {
      static_cast<void>(apply_command_line_input_command(session, command->command));
      continue;
    }
    const auto* forwarded = std::get_if<input::ForwardLegacyInput>(&routed.effect);
    if (forwarded == nullptr) {
      static_cast<void>(queue_hosted_command(session, routed.effect));
      continue;
    }
    bool changed = false;
    if (forwarded->prefix_size > 0) {
      changed = insert_command_line_text(
          session, std::span(forwarded->prefix).first(forwarded->prefix_size));
    }
    changed = insert_command_line_text(session, forwarded->current) || changed;
    if (changed) {
      invalidate_command_line(session);
    }
  }
  return offset;
}

void process_typed_command_line_input(SessionRecord& session, const std::span<const std::byte> text,
                                      const bool control) noexcept {
  if (!control && !text.empty() && insert_command_line_text(session, text)) {
    invalidate_command_line(session);
  }
}

[[nodiscard]] auto resize_session(SessionRecord& session, PaneRuntimeStore& runtimes,
                                  const protocol::Dimensions dimensions,
                                  extension::Runtime* const extensions = nullptr) noexcept -> bool {
  finish_live_divider_resize(session, true);
  const auto columns = std::clamp(dimensions.columns, std::uint16_t{1}, protocol::columns_max);
  const auto rows = std::clamp(dimensions.rows, std::uint16_t{1}, protocol::rows_max);
  // Frame capacity changes only at this lifecycle boundary. Allocation failure preserves the old
  // storage and all terminal geometry, so the caller can reject the resize without partial state.
  const auto retained_frame_bytes = session.attachment_runtime.output.busy()
                                        ? session.attachment_runtime.output.frame_bytes()
                                        : 0;
  if (!session.attachment_runtime.frame.prepare({.columns = columns, .rows = rows},
                                                retained_frame_bytes)) {
    return false;
  }
  ProductionSessionRuntimeContext runtime_context{.session = &session, .runtimes = &runtimes};
  SessionMachine machine(session, production_session_options(runtime_context));
  const auto content =
      extensions != nullptr
          ? extensions->pane_viewport(session.attachment.id, {.columns = columns, .rows = rows})
          : std::optional{PaneRectangle{.columns = columns, .rows = rows}};
  if (!content.has_value()) {
    return false;
  }
  const auto transition = machine.resize_attachment(columns, rows, *content);
  apply_session_change(session, runtimes, transition.change);
  return transition.result.status == CommandStatus::applied ||
         transition.result.status == CommandStatus::no_effect;
}

[[nodiscard]] constexpr auto
selection_gesture_phase(const protocol::MouseInputAction action) noexcept
    -> vt::SelectionGesturePhase {
  switch (action) {
  case protocol::MouseInputAction::press:
    return vt::SelectionGesturePhase::press;
  case protocol::MouseInputAction::release:
    return vt::SelectionGesturePhase::release;
  case protocol::MouseInputAction::motion:
    return vt::SelectionGesturePhase::drag;
  }
  return vt::SelectionGesturePhase::drag;
}

[[nodiscard]] auto process_mouse_selection(SessionRecord& session, PaneRuntimeStore& runtimes,
                                           const Tab& tab, const Pane& pane, PaneRuntime& runtime,
                                           const protocol::MouseInput& mouse,
                                           const std::uint16_t content_row) noexcept -> bool {
  const AttachmentPaneTarget target{.tab = tab.id, .pane = pane.id};
  if (session.attachment.selection_target != std::optional{target}) {
    if (session.attachment.copy_mode.active()) {
      leave_copy_mode(session, runtimes);
    } else {
      clear_mouse_selection(session, runtimes);
    }
    session.attachment.selection_target = target;
  }

  const auto& rectangle = pane.rectangle;
  const auto local_column = static_cast<std::uint16_t>(
      std::clamp(mouse.column, rectangle.column,
                 static_cast<std::uint16_t>(rectangle.column + rectangle.columns - 1U)) -
      rectangle.column);
  const auto local_row = static_cast<std::uint16_t>(
      std::clamp(content_row, rectangle.row,
                 static_cast<std::uint16_t>(rectangle.row + rectangle.rows - 1U)) -
      rectangle.row);
  bool presentation_changed = false;
  if (mouse.action == protocol::MouseInputAction::press) {
    const auto active = runtime.terminal.selection_active();
    if (!active.has_value()) {
      return false;
    }
    presentation_changed = *active;
    runtime.terminal.clear_selection();
  }
  const auto now = reactor_now().time_since_epoch();
  const auto time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const vt::SelectionGestureEvent event{
      .phase = selection_gesture_phase(mouse.action),
      .point = {.space = vt::PointSpace::viewport, .column = local_column, .row = local_row},
      .pointer_x = static_cast<double>(mouse.column) - static_cast<double>(rectangle.column) + 0.5,
      .pointer_y = static_cast<double>(content_row) - static_cast<double>(rectangle.row) + 0.5,
      .cell_width = 1,
      .screen_height = rectangle.rows,
      .time_ns = mouse.action == protocol::MouseInputAction::press
                     ? static_cast<std::uint64_t>(time_ns)
                     : std::uint64_t{0},
      .repeat_interval_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(mouse_repeat_click_interval)
              .count()),
      .repeat_distance = mouse_repeat_click_distance,
      .has_pointer_position = true,
  };
  const auto result = runtime.terminal.selection_gesture(event);
  if (!result.has_value()) {
    return false;
  }
  presentation_changed = presentation_changed || result->selection_changed;
  if (session.attachment.copy_mode.active() && result->selection_changed &&
      !update_copy_viewport_offset(session, runtime)) {
    leave_copy_mode(session, runtimes);
    return false;
  }
  if (presentation_changed) {
    schedule_frame(session, FrameUrgency::interactive, false);
  }
  return true;
}

enum class ParseResult : std::uint8_t {
  keep,
  backpressure,
  yield,
  detach,
  peer_closed,
  error,
};

[[nodiscard]] constexpr auto physical_key(const protocol::KeyInputKey key) noexcept
    -> input::PhysicalKey {
  constexpr std::array mapping{
      input::PhysicalKey::unidentified,
      input::PhysicalKey::a,
      input::PhysicalKey::b,
      input::PhysicalKey::c,
      input::PhysicalKey::d,
      input::PhysicalKey::e,
      input::PhysicalKey::f,
      input::PhysicalKey::g,
      input::PhysicalKey::h,
      input::PhysicalKey::i,
      input::PhysicalKey::j,
      input::PhysicalKey::k,
      input::PhysicalKey::l,
      input::PhysicalKey::m,
      input::PhysicalKey::n,
      input::PhysicalKey::o,
      input::PhysicalKey::p,
      input::PhysicalKey::q,
      input::PhysicalKey::r,
      input::PhysicalKey::s,
      input::PhysicalKey::t,
      input::PhysicalKey::u,
      input::PhysicalKey::v,
      input::PhysicalKey::w,
      input::PhysicalKey::x,
      input::PhysicalKey::y,
      input::PhysicalKey::z,
      input::PhysicalKey::enter,
      input::PhysicalKey::tab,
      input::PhysicalKey::backspace,
      input::PhysicalKey::escape,
      input::PhysicalKey::space,
      input::PhysicalKey::arrow_up,
      input::PhysicalKey::arrow_down,
      input::PhysicalKey::arrow_left,
      input::PhysicalKey::arrow_right,
      input::PhysicalKey::home,
      input::PhysicalKey::end,
      input::PhysicalKey::insert,
      input::PhysicalKey::delete_key,
      input::PhysicalKey::page_up,
      input::PhysicalKey::page_down,
      input::PhysicalKey::f1,
      input::PhysicalKey::f2,
      input::PhysicalKey::f3,
      input::PhysicalKey::f4,
      input::PhysicalKey::f5,
      input::PhysicalKey::f6,
      input::PhysicalKey::f7,
      input::PhysicalKey::f8,
      input::PhysicalKey::f9,
      input::PhysicalKey::f10,
      input::PhysicalKey::f11,
      input::PhysicalKey::f12,
  };
  const auto index = static_cast<std::size_t>(key);
  return index < mapping.size() ? std::span(mapping).subspan(index, 1).front()
                                : input::PhysicalKey::unidentified;
}

static_assert(static_cast<std::uint8_t>(input::PhysicalKey::unidentified) ==
              static_cast<std::uint8_t>(protocol::KeyInputKey::unidentified));
static_assert(static_cast<std::uint8_t>(input::PhysicalKey::a) ==
              static_cast<std::uint8_t>(protocol::KeyInputKey::a));
static_assert(static_cast<std::uint8_t>(input::PhysicalKey::home) ==
              static_cast<std::uint8_t>(protocol::KeyInputKey::home));
static_assert(static_cast<std::uint8_t>(input::PhysicalKey::f12) ==
              static_cast<std::uint8_t>(protocol::KeyInputKey::f12));
static_assert(static_cast<std::size_t>(input::PhysicalKey::count) ==
              static_cast<std::size_t>(protocol::KeyInputKey::f12) + 1U);
static_assert(input::key_modifier_shift == protocol::key_input_modifier_shift);
static_assert(input::key_modifier_control == protocol::key_input_modifier_control);
static_assert(input::key_modifier_alt == protocol::key_input_modifier_alt);
static_assert(input::key_modifier_super == protocol::key_input_modifier_super);
static_assert(input::key_modifier_caps_lock == protocol::key_input_modifier_caps_lock);
static_assert(input::key_modifier_num_lock == protocol::key_input_modifier_num_lock);

[[nodiscard]] constexpr auto protocol_physical_key(const input::PhysicalKey key) noexcept
    -> protocol::KeyInputKey {
  const auto index = static_cast<std::uint8_t>(key);
  return key == input::PhysicalKey::unidentified ||
                 index >= static_cast<std::uint8_t>(input::PhysicalKey::count)
             ? protocol::KeyInputKey::unidentified
             : static_cast<protocol::KeyInputKey>(index);
}

[[nodiscard]] constexpr auto key_action(const protocol::KeyInputAction action) noexcept
    -> input::KeyAction {
  switch (action) {
  case protocol::KeyInputAction::release:
    return input::KeyAction::release;
  case protocol::KeyInputAction::press:
    return input::KeyAction::press;
  case protocol::KeyInputAction::repeat:
    return input::KeyAction::repeat;
  }
  return input::KeyAction::press;
}

[[nodiscard]] constexpr auto key_modifiers(const std::uint16_t modifiers) noexcept
    -> std::uint16_t {
  std::uint16_t result = 0;
  result |= (modifiers & protocol::key_input_modifier_shift) != 0 ? input::key_modifier_shift : 0U;
  result |=
      (modifiers & protocol::key_input_modifier_control) != 0 ? input::key_modifier_control : 0U;
  result |= (modifiers & protocol::key_input_modifier_alt) != 0 ? input::key_modifier_alt : 0U;
  result |= (modifiers & protocol::key_input_modifier_super) != 0 ? input::key_modifier_super : 0U;
  result |= (modifiers & protocol::key_input_modifier_caps_lock) != 0
                ? input::key_modifier_caps_lock
                : 0U;
  result |=
      (modifiers & protocol::key_input_modifier_num_lock) != 0 ? input::key_modifier_num_lock : 0U;
  return result;
}

[[nodiscard]] constexpr auto protocol_key_modifiers(const std::uint16_t modifiers) noexcept
    -> std::uint16_t {
  return modifiers;
}

[[nodiscard]] auto routed_key_event(const protocol::KeyInput& key,
                                    const std::span<const std::byte> text) noexcept
    -> input::KeyEvent {
  return {.action = key_action(key.action),
          .key = physical_key(key.key),
          .modifiers = key_modifiers(key.modifiers),
          .unshifted_codepoint = key.unshifted_codepoint,
          .text = text};
}

[[nodiscard]] auto encoded_application_key(const protocol::KeyInput& original,
                                           const input::EncodeAsKey& encoded) noexcept
    -> protocol::KeyInput {
  auto key = original;
  key.key = protocol_physical_key(encoded.key);
  key.modifiers = protocol_key_modifiers(encoded.modifiers);
  key.consumed_modifiers = 0;
  key.unshifted_codepoint = 0;
  return key;
}

[[nodiscard]] auto focused_input_runtime(SessionRecord& session,
                                         PaneRuntimeStore& runtimes) noexcept -> PaneRuntime* {
  auto* const tab = active_tab(session);
  auto* const pane = tab == nullptr ? nullptr : find_pane(session, *tab, tab->focused_pane);
  return pane == nullptr ? nullptr : find_pane_runtime(runtimes, session, *tab, *pane);
}

[[nodiscard]] auto queue_application_bytes_to(SessionRecord& session, PaneRuntimeStore& runtimes,
                                              PaneRuntime& runtime,
                                              const std::span<const std::byte> bytes) noexcept
    -> InputQueueResult {
  if (!runtime.accepts_input()) {
    return InputQueueResult::encoding_failed;
  }
  const auto queued_bytes_before = runtime.pending_writes.size();
  const auto queued = queue_normalized_input(runtime.pending_writes, runtime.terminal, bytes);
  if (queued != InputQueueResult::queued) {
    return queued;
  }
  std::uint64_t trace_correlation = 0;
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  trace_correlation = session.attachment_runtime.decoded_input_trace_matcher.observe(bytes);
#endif
  diagnostic::record_latency_trace(diagnostic::LatencyTraceStage::daemon_input_message_received,
                                   static_cast<std::uint32_t>(runtime.pty), bytes.size(),
                                   trace_correlation);
  if (runtime.pending_writes.size() > queued_bytes_before) {
    clear_mouse_selection(session, runtimes);
    if (!scroll_viewport_for_application_input(session, runtime)) {
      return InputQueueResult::encoding_failed;
    }
  }
  if (latency_sensitive_input(bytes.size())) {
    runtime.interactive_damage.await_write(queued_bytes_before, runtime.pending_writes.size());
  }
  return InputQueueResult::queued;
}

[[nodiscard]] auto public_input_key(const api::InputEvent& event) noexcept -> protocol::KeyInput {
  static_assert(static_cast<std::uint8_t>(api::InputKey::a) + 1U ==
                static_cast<std::uint8_t>(protocol::KeyInputKey::a));
  static_assert(static_cast<std::uint8_t>(api::InputKey::f12) + 1U ==
                static_cast<std::uint8_t>(protocol::KeyInputKey::f12));
  auto action = protocol::KeyInputAction::press;
  if (event.phase == api::InputKeyPhase::repeat) {
    action = protocol::KeyInputAction::repeat;
  } else if (event.phase == api::InputKeyPhase::release) {
    action = protocol::KeyInputAction::release;
  }
  const auto key = static_cast<protocol::KeyInputKey>(
      static_cast<std::uint8_t>(event.key) + static_cast<std::uint8_t>(protocol::KeyInputKey::a));
  protocol::KeyInput result{.action = action,
                            .key = key,
                            .modifiers = event.modifiers,
                            .consumed_modifiers = 0,
                            .unshifted_codepoint = 0,
                            .composing = false};
  if (event.key >= api::InputKey::a && event.key <= api::InputKey::z) {
    result.unshifted_codepoint = static_cast<std::uint32_t>('a') +
                                 static_cast<std::uint32_t>(event.key) -
                                 static_cast<std::uint32_t>(api::InputKey::a);
  } else if (event.key == api::InputKey::space) {
    result.unshifted_codepoint = static_cast<std::uint32_t>(' ');
  }
  return result;
}

// Event-specific encoders stage into one queue before the batch is admitted atomically.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto queue_public_input_batch(SessionRecord& session, PaneRuntimeStore& runtimes,
                                            PaneRuntime& runtime,
                                            const std::span<const api::InputEvent> events) noexcept
    -> InputQueueResult {
  if (!runtime.accepts_input() || events.empty()) {
    return InputQueueResult::encoding_failed;
  }
  PanePtyWriteQueue staged;
  for (const auto& event : events) {
    InputQueueResult queued = InputQueueResult::encoding_failed;
    if (event.kind == api::InputEventKind::text) {
      queued = queue_normalized_input(
          staged, runtime.terminal, std::as_bytes(std::span(event.text.data(), event.text.size())));
    } else if (event.kind == api::InputEventKind::paste) {
      std::vector<std::byte> paste;
      try {
        const auto bytes = std::as_bytes(std::span(event.text.data(), event.text.size()));
        paste.assign(bytes.begin(), bytes.end());
      } catch (...) {
        return InputQueueResult::encoding_failed;
      }
      queued = queue_paste_input(staged, runtime.terminal, paste);
    } else {
      const auto key = public_input_key(event);
      std::array<std::byte, 1> text{};
      auto key_text = std::span<const std::byte>{};
      if (event.phase != api::InputKeyPhase::release &&
          ((event.key >= api::InputKey::a && event.key <= api::InputKey::z) ||
           event.key == api::InputKey::space)) {
        char character = ' ';
        if (event.key != api::InputKey::space) {
          const auto offset = static_cast<char>(event.key) - static_cast<char>(api::InputKey::a);
          character = static_cast<char>(
              ((event.modifiers & api::input_modifier_shift) != 0 ? 'A' : 'a') + offset);
        }
        text.front() = static_cast<std::byte>(character);
        key_text = text;
      }
      queued = queue_key_input(staged, runtime.terminal, key, key_text);
    }
    if (queued != InputQueueResult::queued) {
      return queued;
    }
  }
  if (staged.empty()) {
    return InputQueueResult::queued;
  }
  if (staged.size() > runtime.pending_writes.input_remaining()) {
    return InputQueueResult::full;
  }
  if (!scroll_viewport_for_application_input(session, runtime)) {
    return InputQueueResult::encoding_failed;
  }
  const auto queued_bytes_before = runtime.pending_writes.size();
  const bool appended = runtime.pending_writes.append(staged.readable_span());
  LEMMA_ASSERT(appended);
  clear_mouse_selection(session, runtimes);
  runtime.interactive_damage.await_write(queued_bytes_before, runtime.pending_writes.size());
  return InputQueueResult::queued;
}

[[nodiscard]] auto queue_application_bytes(SessionRecord& session, PaneRuntimeStore& runtimes,
                                           const std::span<const std::byte> bytes) noexcept
    -> InputQueueResult {
  auto* const runtime = focused_input_runtime(session, runtimes);
  return runtime == nullptr ? InputQueueResult::encoding_failed
                            : queue_application_bytes_to(session, runtimes, *runtime, bytes);
}

[[nodiscard]] auto queue_application_key(SessionRecord& session, PaneRuntimeStore& runtimes,
                                         const protocol::KeyInput& key,
                                         const std::span<const std::byte> text,
                                         const std::span<const std::byte> prefix = {}) noexcept
    -> InputQueueResult {
  auto* const runtime = focused_input_runtime(session, runtimes);
  if (runtime == nullptr || !runtime->accepts_input()) {
    return InputQueueResult::encoding_failed;
  }
  const auto queued_bytes_before = runtime->pending_writes.size();
  const auto queued =
      prefix.empty()
          ? queue_key_input(runtime->pending_writes, runtime->terminal, key, text)
          : queue_prefixed_key_input(runtime->pending_writes, runtime->terminal, prefix, key, text);
  if (queued != InputQueueResult::queued) {
    return queued;
  }
  if (runtime->pending_writes.size() > queued_bytes_before) {
    clear_mouse_selection(session, runtimes);
    if (!scroll_viewport_for_application_input(session, *runtime)) {
      return InputQueueResult::encoding_failed;
    }
    runtime->interactive_damage.await_write(queued_bytes_before, runtime->pending_writes.size());
  }
  return InputQueueResult::queued;
}

void accept_input_route(SessionRecord& session, PaneRuntimeStore& runtimes,
                        const bool presentation_changed,
                        const bool interaction_preemption_requested) noexcept {
  if (interaction_preemption_requested) {
    const Command cancel{
        .kind = CommandKind::cancel_attachment_interaction,
        .origin = CommandOrigin::keymap,
        .target = {.session = session.id,
                   .tab = {},
                   .pane = {},
                   .peer_pane = {},
                   .attachment = session.attachment.id},
    };
    const auto result = dispatch_session_command(session, runtimes, cancel);
    LEMMA_ASSERT(result.succeeded());
  }
  if (presentation_changed) {
    schedule_frame(session, FrameUrgency::state_change, false);
  }
}

[[nodiscard]] constexpr auto command_focuses_pane(const CommandKind command) noexcept -> bool {
  return command == CommandKind::focus_left || command == CommandKind::focus_right ||
         command == CommandKind::focus_up || command == CommandKind::focus_down ||
         command == CommandKind::focus_next || command == CommandKind::focus_previous ||
         command == CommandKind::focus_pane || command == CommandKind::next_tab ||
         command == CommandKind::previous_tab || command == CommandKind::select_tab ||
         command == CommandKind::create_tab || command == CommandKind::split_left_right ||
         command == CommandKind::split_top_bottom;
}

[[nodiscard]] auto dispatch_input_command(SessionRecord& session, PaneRuntimeStore& runtimes,
                                          extension::Runtime& extensions,
                                          const input::InputCommand input_command,
                                          const SessionNameConflict name_conflict,
                                          void* const name_conflict_context) noexcept
    -> ParseResult {
  finish_live_divider_resize(session, true);
  if (input_command == input::InputCommand::begin_command_line) {
    begin_command_line(session, runtimes);
    return ParseResult::keep;
  }
  if (input_command == input::InputCommand::show_messages) {
    begin_message_view(session, runtimes);
    return ParseResult::keep;
  }
  if (session.attachment.message_view.active) {
    leave_message_view(session);
  }
  const bool begins_rename = input_command == input::InputCommand::begin_rename_session ||
                             input_command == input::InputCommand::begin_rename_tab;
  if (session.attachment.rename_prompt.active() && !begins_rename) {
    reset_rename_prompt(session);
  }
  if (session.attachment.command_line.active) {
    reset_command_line(session);
  }
  const auto command = command_from_input(session, input_command, CommandOrigin::keymap);
  if (!command.has_value()) {
    return ParseResult::keep;
  }
  const auto result =
      dispatch_session_command(session, runtimes, *command, name_conflict, name_conflict_context);
  if (result.succeeded() && command_focuses_pane(command->kind)) {
    static_cast<void>(extensions.focus_pane(session.attachment.id));
  }
  return result.status == CommandStatus::detach_requested || !session.active ? ParseResult::detach
                                                                             : ParseResult::keep;
}

[[nodiscard]] auto materialize_legacy_input(
    const input::ForwardLegacyInput& forwarded,
    std::array<std::byte, input::deferred_input_bytes_max + 1U>& storage) noexcept
    -> std::span<const std::byte> {
  if (forwarded.prefix_size == 0U) {
    return forwarded.current;
  }
  LEMMA_ASSERT(forwarded.prefix_size <= forwarded.prefix.size());
  LEMMA_ASSERT(forwarded.current.size() <= storage.size() - forwarded.prefix_size);
  std::ranges::copy(std::span(forwarded.prefix).first(forwarded.prefix_size), storage.begin());
  std::ranges::copy(
      forwarded.current,
      std::span(storage).subspan(forwarded.prefix_size, forwarded.current.size()).begin());
  return std::span(storage).first(forwarded.prefix_size + forwarded.current.size());
}

[[nodiscard]] auto route_surface_key(extension::Runtime& extensions, const SessionRecord& session,
                                     const protocol::KeyInput& key,
                                     std::span<const std::byte> text) noexcept -> bool;

// One decoder-held input message is advanced monotonically. Context transitions and commands are
// never replayed when the pane PTY queue applies backpressure.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
[[nodiscard]] auto process_routed_legacy_input(SessionRecord& session, PaneRuntimeStore& runtimes,
                                               extension::Runtime& extensions,
                                               const std::span<const std::byte> message,
                                               std::size_t& input_budget,
                                               const SessionNameConflict name_conflict,
                                               void* const name_conflict_context) noexcept
    -> ParseResult {
  auto offset = session.attachment_runtime.retained_input_offset.value_or(0);
  if (offset > message.size()) {
    return ParseResult::error;
  }
  if (session.attachment_runtime.pending_routed_input_size > 0U) {
    const auto pending = std::span(session.attachment_runtime.pending_routed_input)
                             .first(session.attachment_runtime.pending_routed_input_size);
    const auto queued = queue_application_bytes(session, runtimes, pending);
    if (queued == InputQueueResult::full) {
      return ParseResult::backpressure;
    }
    if (queued != InputQueueResult::queued) {
      return ParseResult::error;
    }
    session.attachment_runtime.pending_routed_input_size = 0;
  }

  while (offset < message.size()) {
    if (input_budget == 0U) {
      session.attachment_runtime.retained_input_offset = offset;
      return ParseResult::yield;
    }
    --input_budget;
    if (session.attachment.message_view.active) {
      const auto consumed = process_message_view_input(session, message.subspan(offset));
      if (consumed == 0) {
        return ParseResult::error;
      }
      offset += consumed;
      continue;
    }
    if (session.attachment.command_line.active) {
      if (session.attachment.command_line.submit_requested) {
        session.attachment_runtime.retained_input_offset = offset;
        return ParseResult::yield;
      }
      const auto consumed = process_command_line_input(session, message.subspan(offset));
      if (consumed == 0) {
        return ParseResult::error;
      }
      offset += consumed;
      continue;
    }
    if (session.attachment.rename_prompt.active()) {
      const auto consumed = process_rename_prompt_input(session, runtimes, message.subspan(offset),
                                                        name_conflict, name_conflict_context);
      if (consumed == 0) {
        return ParseResult::error;
      }
      offset += consumed;
      continue;
    }
    if (session.attachment.copy_mode.active()) {
      const auto consumed = process_copy_mode_input(session, runtimes, message.subspan(offset, 1));
      if (consumed > 1U || (consumed == 0U && session.attachment.copy_mode.active())) {
        return ParseResult::error;
      }
      offset += consumed;
      continue;
    }
    std::optional<input::InputRouter> router_before;
    if (session.input_router.legacy_route_requires_checkpoint()) {
      router_before.emplace(session.input_router);
    }
    const auto forward_limit =
        session.attachment.copy_mode.active() ? std::size_t{1} : message.size() - offset;
    const auto routed = session.input_router.route_legacy(message.subspan(offset), forward_limit);
    if (routed.consumed == 0U || routed.consumed > message.size() - offset) {
      return ParseResult::error;
    }
    if (const auto* const command = std::get_if<input::RoutedCommand>(&routed.effect);
        command != nullptr) {
      accept_input_route(session, runtimes, routed.presentation_changed,
                         routed.interaction_preemption_requested);
      offset += routed.consumed;
      const auto dispatched = dispatch_input_command(
          session, runtimes, extensions, command->command, name_conflict, name_conflict_context);
      if (dispatched == ParseResult::detach) {
        return dispatched;
      }
      continue;
    }
    if (std::holds_alternative<input::ConsumedInput>(routed.effect)) {
      accept_input_route(session, runtimes, routed.presentation_changed,
                         routed.interaction_preemption_requested);
      offset += routed.consumed;
      continue;
    }

    const auto* const forwarded = std::get_if<input::ForwardLegacyInput>(&routed.effect);
    if (forwarded == nullptr) {
      static_cast<void>(queue_hosted_command(session, routed.effect));
      accept_input_route(session, runtimes, routed.presentation_changed,
                         routed.interaction_preemption_requested);
      offset += routed.consumed;
      continue;
    }
    std::array<std::byte, input::deferred_input_bytes_max + 1U> storage{};
    const auto application_input = materialize_legacy_input(*forwarded, storage);
    if (application_input.empty()) {
      return ParseResult::error;
    }
    if (session.attachment.command_line.active) {
      process_command_line_input(session, application_input);
    } else if (session.attachment.rename_prompt.active()) {
      process_rename_prompt_input(session, runtimes, application_input, name_conflict,
                                  name_conflict_context);
    } else if (session.attachment.copy_mode.active()) {
      const auto consumed = process_copy_mode_input(session, runtimes, application_input);
      if (consumed > application_input.size()) {
        return ParseResult::error;
      }
      const auto remaining = application_input.subspan(consumed);
      if (!remaining.empty()) {
        const auto queued = queue_application_bytes(session, runtimes, remaining);
        if (queued == InputQueueResult::full) {
          LEMMA_ASSERT(remaining.size() <= session.attachment_runtime.pending_routed_input.size());
          std::ranges::copy(remaining, session.attachment_runtime.pending_routed_input.begin());
          session.attachment_runtime.pending_routed_input_size =
              static_cast<std::uint8_t>(remaining.size());
          offset += routed.consumed;
          session.attachment_runtime.retained_input_offset = offset;
          accept_input_route(session, runtimes, routed.presentation_changed,
                             routed.interaction_preemption_requested);
          return ParseResult::backpressure;
        }
        if (queued != InputQueueResult::queued) {
          return ParseResult::error;
        }
      }
    } else if (!route_surface_key(extensions, session,
                                  {.action = protocol::KeyInputAction::press,
                                   .key = protocol::KeyInputKey::unidentified},
                                  application_input)) {
      const auto queued = queue_application_bytes(session, runtimes, application_input);
      if (queued == InputQueueResult::full) {
        if (router_before.has_value()) {
          session.input_router = *router_before;
        }
        session.attachment_runtime.retained_input_offset = offset;
        return ParseResult::backpressure;
      }
      if (queued != InputQueueResult::queued) {
        return ParseResult::error;
      }
    }
    accept_input_route(session, runtimes, routed.presentation_changed,
                       routed.interaction_preemption_requested);
    offset += routed.consumed;
  }
  session.attachment_runtime.retained_input_offset.reset();
  return ParseResult::keep;
}

[[nodiscard]] auto copyable_selection(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept
    -> bool {
  auto* runtime = selection_runtime(session, runtimes);
  if (runtime == nullptr) {
    runtime = focused_input_runtime(session, runtimes);
  }
  if (runtime == nullptr) {
    return false;
  }
  const auto active = runtime->terminal.selection_active();
  return active.has_value() && *active;
}

// NOLINTBEGIN(readability-function-cognitive-complexity)
[[nodiscard]] auto
process_routed_key_input(SessionRecord& session, PaneRuntimeStore& runtimes,
                         extension::Runtime& extensions, const protocol::KeyInput& key,
                         const std::span<const std::byte> text, std::size_t& input_budget,
                         const SessionNameConflict name_conflict,
                         void* const name_conflict_context) noexcept -> ParseResult {
  if (input_budget == 0U) {
    return ParseResult::yield;
  }
  --input_budget;
  if (session.attachment.message_view.active) {
    session.interaction_router.select_base(input::ConfiguredInputContext::messages);
    auto event = routed_key_event(key, text);
    const auto routed = session.interaction_router.route_key(event);
    event.action = input::KeyAction::release;
    static_cast<void>(session.interaction_router.route_key(event));
    if (const auto* command = std::get_if<input::RoutedCommand>(&routed.effect);
        command != nullptr) {
      apply_message_view_input_command(session, command->command);
    } else {
      static_cast<void>(queue_hosted_command(session, routed.effect));
    }
    return ParseResult::keep;
  }
  if (session.attachment.command_line.active) {
    session.interaction_router.select_base(input::ConfiguredInputContext::command_line);
    auto event = routed_key_event(key, text);
    const auto routed = session.interaction_router.route_key(event);
    event.action = input::KeyAction::release;
    static_cast<void>(session.interaction_router.route_key(event));
    if (const auto* command = std::get_if<input::RoutedCommand>(&routed.effect);
        command != nullptr) {
      static_cast<void>(apply_command_line_input_command(session, command->command));
    } else if (std::holds_alternative<input::ForwardCurrentKey>(routed.effect)) {
      process_typed_command_line_input(
          session, text, (key.modifiers & protocol::key_input_modifier_control) != 0U);
    } else {
      static_cast<void>(queue_hosted_command(session, routed.effect));
    }
    return ParseResult::keep;
  }
  if (session.attachment.rename_prompt.active()) {
    session.interaction_router.select_base(input::ConfiguredInputContext::rename);
    auto event = routed_key_event(key, text);
    const auto routed = session.interaction_router.route_key(event);
    event.action = input::KeyAction::release;
    static_cast<void>(session.interaction_router.route_key(event));
    if (const auto* const command = std::get_if<input::RoutedCommand>(&routed.effect);
        command != nullptr) {
      static_cast<void>(apply_rename_input_command(session, runtimes, command->command,
                                                   name_conflict, name_conflict_context));
    } else if (std::holds_alternative<input::ForwardCurrentKey>(routed.effect)) {
      process_typed_rename_prompt_input(
          session, text, (key.modifiers & protocol::key_input_modifier_control) != 0U);
    } else {
      static_cast<void>(queue_hosted_command(session, routed.effect));
    }
    return ParseResult::keep;
  }
  if (session.attachment.copy_mode.active()) {
    process_typed_copy_mode_input(session, runtimes, key, text);
    return ParseResult::keep;
  }
  auto router_before = session.input_router;
  const auto unbound_before = session.input_router.unbound();
  auto routed = session.input_router.route_key(routed_key_event(key, text));
  if (const auto* const command = std::get_if<input::RoutedCommand>(&routed.effect);
      command != nullptr && command->command == input::InputCommand::copy_selection &&
      !copyable_selection(session, runtimes)) {
    if (unbound_before == input::UnboundBehavior::forward) {
      session.input_router = router_before;
      routed.effect = input::ForwardCurrentKey{};
    } else {
      accept_input_route(session, runtimes, routed.presentation_changed,
                         routed.interaction_preemption_requested);
      return ParseResult::keep;
    }
  }
  if (const auto* const command = std::get_if<input::RoutedCommand>(&routed.effect);
      command != nullptr) {
    accept_input_route(session, runtimes, routed.presentation_changed,
                       routed.interaction_preemption_requested);
    return dispatch_input_command(session, runtimes, extensions, command->command, name_conflict,
                                  name_conflict_context);
  }
  if (std::holds_alternative<input::ConsumedInput>(routed.effect)) {
    accept_input_route(session, runtimes, routed.presentation_changed,
                       routed.interaction_preemption_requested);
    return ParseResult::keep;
  }

  const auto* const encoded = std::get_if<input::EncodeAsKey>(&routed.effect);
  if (std::holds_alternative<input::ForwardCurrentKey>(routed.effect) || encoded != nullptr) {
    const auto application = encoded != nullptr ? encoded_application_key(key, *encoded) : key;
    const auto application_text = encoded != nullptr ? std::span<const std::byte>{} : text;
    if (session.attachment.command_line.active) {
      process_typed_command_line_input(
          session, application_text,
          (application.modifiers & protocol::key_input_modifier_control) != 0U);
    } else if (session.attachment.rename_prompt.active()) {
      process_typed_rename_prompt_input(
          session, application_text,
          (application.modifiers & protocol::key_input_modifier_control) != 0U);
    } else if (session.attachment.copy_mode.active()) {
      process_typed_copy_mode_input(session, runtimes, application, application_text);
    } else if (!route_surface_key(extensions, session, application, application_text)) {
      const auto queued = queue_application_key(session, runtimes, application, application_text);
      if (queued == InputQueueResult::full) {
        session.input_router = router_before;
        return ParseResult::backpressure;
      }
      if (queued != InputQueueResult::queued) {
        return ParseResult::error;
      }
    }
  } else {
    std::array<std::byte, input::deferred_input_bytes_max> prefix_storage{};
    std::span<const std::byte> prefix;
    bool forward_current = false;
    if (const auto* const bytes = std::get_if<input::ForwardBytes>(&routed.effect);
        bytes != nullptr) {
      prefix_storage = bytes->bytes;
      prefix = std::span(prefix_storage).first(bytes->size);
    } else if (const auto* const following =
                   std::get_if<input::ForwardBytesThenCurrentKey>(&routed.effect);
               following != nullptr) {
      prefix_storage = following->bytes;
      prefix = std::span(prefix_storage).first(following->size);
      forward_current = true;
    } else if (queue_hosted_command(session, routed.effect)) {
      accept_input_route(session, runtimes, routed.presentation_changed,
                         routed.interaction_preemption_requested);
      return ParseResult::keep;
    } else {
      return ParseResult::error;
    }
    if (prefix.empty()) {
      return ParseResult::error;
    }
    if (session.attachment.message_view.active) {
      process_message_view_input(session, prefix);
    } else if (session.attachment.command_line.active) {
      process_command_line_input(session, prefix);
      if (forward_current && session.attachment.command_line.active) {
        process_typed_command_line_input(
            session, text, (key.modifiers & protocol::key_input_modifier_control) != 0U);
      }
    } else if (session.attachment.rename_prompt.active()) {
      process_rename_prompt_input(session, runtimes, prefix, name_conflict, name_conflict_context);
      if (forward_current && session.attachment.rename_prompt.active()) {
        process_typed_rename_prompt_input(
            session, text, (key.modifiers & protocol::key_input_modifier_control) != 0U);
      }
    } else if (session.attachment.copy_mode.active()) {
      static_cast<void>(process_copy_mode_input(session, runtimes, prefix));
      if (forward_current) {
        if (!session.attachment.copy_mode.active()) {
          return ParseResult::error;
        }
        process_typed_copy_mode_input(session, runtimes, key, text);
      }
    } else {
      const bool surface_routed = route_surface_key(
          extensions, session,
          {.action = protocol::KeyInputAction::press, .key = protocol::KeyInputKey::unidentified},
          prefix);
      if (surface_routed && forward_current) {
        static_cast<void>(route_surface_key(extensions, session, key, text));
      }
      auto queued = InputQueueResult::queued;
      if (!surface_routed) {
        queued = forward_current ? queue_application_key(session, runtimes, key, text, prefix)
                                 : queue_application_bytes(session, runtimes, prefix);
      }
      if (queued == InputQueueResult::full) {
        session.input_router = router_before;
        return ParseResult::backpressure;
      }
      if (queued != InputQueueResult::queued) {
        return ParseResult::error;
      }
    }
  }
  accept_input_route(session, runtimes, routed.presentation_changed,
                     routed.interaction_preemption_requested);
  return ParseResult::keep;
}
// NOLINTEND(readability-function-cognitive-complexity)

[[nodiscard]] auto expensive_client_message(const SessionRecord& session,
                                            const protocol::ClientMessage& message) noexcept
    -> bool {
  if (message.kind == protocol::ClientMessageKind::resize ||
      message.kind == protocol::ClientMessageKind::cell_size ||
      message.kind == protocol::ClientMessageKind::pane_command) {
    return true;
  }
  return message.kind == protocol::ClientMessageKind::mouse &&
         message.mouse.action != protocol::MouseInputAction::press &&
         session.attachment.mouse_capture.has_value() &&
         (session.attachment.mouse_capture->owner == MouseCaptureOwner::divider ||
          session.attachment.mouse_capture->owner == MouseCaptureOwner::status_tab);
}

[[nodiscard]] auto route_surface_key(extension::Runtime& extensions, const SessionRecord& session,
                                     const protocol::KeyInput& key,
                                     const std::span<const std::byte> text) noexcept -> bool {
  if (session.attachment.message_view.active || session.attachment.command_line.active ||
      session.attachment.rename_prompt.active() || session.attachment.copy_mode.active()) {
    return false;
  }
  const auto surface = extensions.focused_surface(session.attachment.id);
  if (!surface.is_valid()) {
    return false;
  }
  const auto owner = extensions.surface_owner(surface);
  try {
    std::string event = R"({"schema":"lemma.event/v1","sequence":)" +
                        std::to_string(extensions.event_sequence(owner)) +
                        R"(,"event":"surface.key","surface":")" + std::to_string(surface.slot()) +
                        ":" + std::to_string(surface.generation()) + R"(","action":)" +
                        std::to_string(static_cast<std::uint8_t>(key.action)) + R"(,"key":)" +
                        std::to_string(static_cast<std::uint8_t>(key.key)) + R"(,"modifiers":)" +
                        std::to_string(key.modifiers);
    static_assert(protocol::legacy_input_message_bytes_max <= limits::extension_input_bytes_max);
    static_assert(protocol::key_input_text_bytes_max <= limits::extension_input_bytes_max);
    if (!extension::append_input_payload(event, text, false)) {
      static_cast<void>(extensions.disconnect(owner));
      return true;
    }
    event += '}';
    if (!extensions.send_event(owner, event)) {
      static_cast<void>(extensions.disconnect(owner));
    }
  } catch (...) {
    static_cast<void>(extensions.disconnect(owner));
  }
  return true;
}

// Keep captured ownership, bounded backpressure, and chunk publication in one rejection boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto route_surface_paste(extension::Runtime& extensions, SessionRecord& session,
                                       const std::span<const std::byte> text) noexcept
    -> std::optional<ParseResult> {
  auto& progress = session.attachment_runtime.surface_paste;
  if (!progress.has_value()) {
    if (session.attachment.message_view.active || session.attachment.command_line.active ||
        session.attachment.rename_prompt.active() || session.attachment.copy_mode.active()) {
      return std::nullopt;
    }
    const auto surface = extensions.focused_surface(session.attachment.id);
    if (!surface.is_valid()) {
      return std::nullopt;
    }
    progress = {.surface = surface, .deadline = {}};
  }
  const auto surface = progress->surface;
  const auto owner = extensions.surface_owner(surface);
  if (!extensions.connected(owner) || extensions.attachment(owner) != session.attachment.id) {
    // A paste already routed to an owner must never leak its suffix into native Pane input.
    progress.reset();
    return ParseResult::keep;
  }
  if (extensions.output_accounting(owner).event_records != 0) {
    // Wait for the previous Event to drain instead of overflowing a healthy, briefly descheduled
    // consumer. Reuse input backpressure so the reactor sleeps on peer write readiness.
    const auto now = reactor_now();
    if (!progress->deadline) {
      progress->deadline = now + std::chrono::seconds(5);
    }
    if (now < *progress->deadline) {
      return ParseResult::backpressure;
    }
    static_cast<void>(extensions.disconnect(owner));
    progress.reset();
    return ParseResult::keep;
  }
  progress->deadline.reset();
  // The accepted key-record bound is not a paste production quantum. Hex encoding doubles the
  // bytes; reserve half the write quantum for framing and other output instead of outrunning it.
  constexpr auto chunk_bytes_max = limits::extension_io_bytes_per_turn_max / 4U;
  static_assert(chunk_bytes_max <= limits::extension_input_bytes_max);
  const auto chunk = text.subspan(progress->offset)
                         .first(std::min(text.size() - progress->offset, chunk_bytes_max));
  try {
    std::string event = R"({"schema":"lemma.event/v1","sequence":)" +
                        std::to_string(extensions.event_sequence(owner)) +
                        R"(,"event":"surface.paste","surface":")" + std::to_string(surface.slot()) +
                        ":" + std::to_string(surface.generation()) + '"';
    if (!extension::append_input_payload(event, chunk, true)) {
      static_cast<void>(extensions.disconnect(owner));
    } else {
      event += '}';
      if (!extensions.send_event(owner, event)) {
        static_cast<void>(extensions.disconnect(owner));
      }
    }
  } catch (...) {
    static_cast<void>(extensions.disconnect(owner));
  }
  progress->offset += chunk.size();
  if (progress->offset < text.size() && extensions.connected(owner)) {
    // At most one bounded encoding per client turn, allowing socket output and native input to
    // progress between chunks even for the maximum-size attachment paste.
    return ParseResult::yield;
  }
  progress.reset();
  return ParseResult::keep;
}

[[nodiscard]] auto route_surface_mouse(extension::Runtime& extensions, SessionRecord& session,
                                       const protocol::MouseInput& mouse,
                                       const std::uint16_t content_row) noexcept -> bool {
  const render::Viewport viewport{.columns = session.attachment.columns,
                                  .rows = pane_rows(session.attachment.rows)};
  auto target = extensions.captured_surface_pointer(session.attachment.id);
  if (!target.is_valid()) {
    target = extensions.surface_at(session.attachment.id, viewport, mouse.column, content_row);
  }
  if (!target.is_valid()) {
    return false;
  }
  const auto owner = extensions.surface_owner(target);
  const auto rectangle = extensions.surface_rectangle(target, viewport);
  if (!owner.is_valid() || !rectangle.has_value()) {
    return false;
  }
  if (mouse.action == protocol::MouseInputAction::press) {
    extensions.capture_surface_pointer(session.attachment.id, target);
    if (extensions.surface_focusable(target)) {
      const auto focused = extensions.focus_surface(owner, target, viewport);
      if (focused.status == extension::SurfaceOperationStatus::applied) {
        schedule_frame(session, FrameUrgency::interactive, true);
      }
    }
  }
  try {
    std::string event = R"({"schema":"lemma.event/v1","sequence":)" +
                        std::to_string(extensions.event_sequence(owner)) +
                        R"(,"event":"surface.mouse","surface":")" + std::to_string(target.slot()) +
                        ":" + std::to_string(target.generation()) + R"(","action":)" +
                        std::to_string(static_cast<std::uint8_t>(mouse.action)) + R"(,"button":)" +
                        std::to_string(static_cast<std::uint8_t>(mouse.button)) +
                        R"(,"modifiers":)" + std::to_string(mouse.modifiers) + R"(,"column":)" +
                        std::to_string(static_cast<std::int32_t>(mouse.column) -
                                       static_cast<std::int32_t>(rectangle->column)) +
                        R"(,"row":)" +
                        std::to_string(static_cast<std::int32_t>(content_row) -
                                       static_cast<std::int32_t>(rectangle->row)) +
                        "}";
    if (!extensions.send_event(owner, event)) {
      static_cast<void>(extensions.disconnect(owner));
    }
  } catch (...) {
    static_cast<void>(extensions.disconnect(owner));
  }
  if (mouse.action == protocol::MouseInputAction::release) {
    extensions.release_surface_pointer(session.attachment.id);
  }
  return true;
}

// Packet dispatch exhaustively maps validated protocol messages to session transitions.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto parse_client_packets(SessionRecord& session, PaneRuntimeStore& runtimes,
                                        extension::Runtime& extensions, std::size_t& message_budget,
                                        std::size_t& geometry_budget, std::size_t& input_budget,
                                        const SessionNameConflict name_conflict,
                                        void* const name_conflict_context) noexcept -> ParseResult {
  while (true) {
    const auto decoded = session.attachment_runtime.decoder.next();
    if (!decoded.has_value()) {
      return ParseResult::error;
    }
    if (!decoded->has_value()) {
      return ParseResult::keep;
    }
    const auto& message = **decoded;
    const bool expensive = expensive_client_message(session, message);
    if (message_budget == 0 || (expensive && geometry_budget == 0)) {
      return ParseResult::yield;
    }
    --message_budget;
    if (expensive) {
      --geometry_budget;
    }
    if (message.kind == protocol::ClientMessageKind::input ||
        message.kind == protocol::ClientMessageKind::key ||
        message.kind == protocol::ClientMessageKind::mouse) {
      clear_status_message(session);
    }
    switch (message.kind) {
    case protocol::ClientMessageKind::hello:
      return ParseResult::error;
    case protocol::ClientMessageKind::detach: {
      const Command command{.kind = CommandKind::detach_client, .origin = CommandOrigin::client};
      const auto result = dispatch_session_command(session, runtimes, command);
      session.attachment_runtime.decoder.consume();
      return result.status == CommandStatus::detach_requested ? ParseResult::detach
                                                              : ParseResult::error;
    }
    case protocol::ClientMessageKind::clipboard_support: {
      auto& attachment = session.attachment_runtime;
      attachment.kitty_clipboard_supported = message.clipboard_supported;
      if (!message.clipboard_supported && attachment.clipboard != nullptr &&
          attachment.clipboard->protocol() == vt::ClipboardProtocol::kitty) {
        attachment.clipboard->fail(vt::ClipboardStatus::unsupported);
        service_clipboard(session, runtimes);
      }
      break;
    }
    case protocol::ClientMessageKind::cell_size:
      if (!apply_cell_size(session, runtimes, message.cell_size)) {
        return ParseResult::error;
      }
      break;
    case protocol::ClientMessageKind::resize:
      if (!resize_session(session, runtimes, message.dimensions, &extensions)) {
        return ParseResult::error;
      }
      break;
    case protocol::ClientMessageKind::input: {
      const auto result =
          process_routed_legacy_input(session, runtimes, extensions, message.input, input_budget,
                                      name_conflict, name_conflict_context);
      if (result != ParseResult::keep) {
        return result;
      }
      break;
    }
    case protocol::ClientMessageKind::key: {
      const auto result =
          process_routed_key_input(session, runtimes, extensions, message.key, message.input,
                                   input_budget, name_conflict, name_conflict_context);
      if (result != ParseResult::keep) {
        return result;
      }
      break;
    }
    case protocol::ClientMessageKind::terminal_reply:
      if (session.attachment_runtime.clipboard != nullptr) {
        // Replies never pass through keymaps, Surface input, or paste; completion precedes the next
        // input record.
        session.attachment_runtime.clipboard->consume(
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            {reinterpret_cast<const char*>(message.input.data()), message.input.size()});
        service_clipboard(session, runtimes);
      }
      break;
    case protocol::ClientMessageKind::paste: {
      if (const auto routed = route_surface_paste(extensions, session, message.input);
          routed.has_value()) {
        if (*routed != ParseResult::keep) {
          return *routed;
        }
        break;
      }
      if (session.attachment.message_view.active) {
        break;
      }
      if (session.attachment.command_line.active) {
        if (insert_command_line_text(session, message.input)) {
          invalidate_command_line(session);
        }
        break;
      }
      if (session.attachment.rename_prompt.active()) {
        if (insert_rename_prompt_text(session, message.input)) {
          invalidate_rename_prompt(session);
        }
        break;
      }
      if (session.attachment.copy_mode.active()) {
        break;
      }
      auto* const tab = active_tab(session);
      auto* const pane = tab == nullptr ? nullptr : find_pane(session, *tab, tab->focused_pane);
      if (pane == nullptr) {
        return ParseResult::error;
      }
      auto* const runtime = find_pane_runtime(runtimes, session, *tab, *pane);
      if (runtime == nullptr) {
        return ParseResult::error;
      }
      const auto queued_bytes_before = runtime->pending_writes.size();
      const auto queued =
          queue_paste_input(runtime->pending_writes, runtime->terminal, message.input);
      if (queued == InputQueueResult::full) {
        return ParseResult::backpressure;
      }
      if (queued == InputQueueResult::encoding_failed) {
        return ParseResult::error;
      }
      if (runtime->pending_writes.size() > queued_bytes_before) {
        if (!scroll_viewport_for_application_input(session, *runtime)) {
          return ParseResult::error;
        }
        runtime->interactive_damage.await_write(queued_bytes_before,
                                                runtime->pending_writes.size());
      }
      break;
    }
    case protocol::ClientMessageKind::focus:
      session.attachment_runtime.outer_focused = message.focus == protocol::FocusInput::gained;
      reconcile_focus_reports(session, runtimes);
      break;
    case protocol::ClientMessageKind::mouse: {
      if (message.mouse.geometry.columns != session.attachment.columns ||
          message.mouse.geometry.rows != session.attachment.rows) {
        return ParseResult::error;
      }
      if (session.attachment.message_view.active) {
        if (message.mouse.action == protocol::MouseInputAction::press &&
            message.mouse.button == protocol::MouseInputButton::four) {
          apply_message_view_input_command(session, input::InputCommand::message_view_previous);
        } else if (message.mouse.action == protocol::MouseInputAction::press &&
                   message.mouse.button == protocol::MouseInputButton::five) {
          apply_message_view_input_command(session, input::InputCommand::message_view_next);
        }
        break;
      }
      if (session.attachment.rename_prompt.active() || session.attachment.command_line.active) {
        break;
      }
      auto* const tab = active_tab(session);
      if (tab == nullptr) {
        return ParseResult::error;
      }
      if (message.mouse.action == protocol::MouseInputAction::press &&
          session.attachment.mouse_capture.has_value() &&
          session.attachment.mouse_capture->owner == MouseCaptureOwner::divider) {
        finish_live_divider_resize(session);
      }
      const bool captured_continuation = session.attachment.mouse_capture.has_value() &&
                                         message.mouse.action != protocol::MouseInputAction::press;
      const auto content_row = message.mouse.row;
      if (!captured_continuation &&
          route_surface_mouse(extensions, session, message.mouse, content_row)) {
        break;
      }
      if (tab->layout_suspended) {
        if (session.attachment.mouse_capture.has_value() &&
            session.attachment.mouse_capture->owner == MouseCaptureOwner::divider) {
          session.active = false;
          return ParseResult::error;
        }
        if (message.mouse.action == protocol::MouseInputAction::release) {
          session.attachment.mouse_capture.reset();
        }
        break;
      }
      const render::PaneRectangle viewport{
          .column = tab->layout_column,
          .row = tab->layout_row,
          .columns = tab->layout_columns,
          .rows = tab->layout_rows,
      };
      if (!captured_continuation && !tab->zoomed &&
          message.mouse.action == protocol::MouseInputAction::press &&
          message.mouse.button == protocol::MouseInputButton::left) {
        const auto divider = tab->layout.divider_at(viewport, message.mouse.column, content_row);
        if (divider.has_value()) {
          session.attachment.mouse_capture = MouseCapture{
              .target = {.tab = tab->id, .pane = divider->first},
              .peer_pane = divider->second,
              .status_tab_before = {},
              .owner = MouseCaptureOwner::divider,
              .divider_axis = divider->axis,
          };
          break;
        }
      }
      if (captured_continuation &&
          session.attachment.mouse_capture->owner == MouseCaptureOwner::discard_until_release) {
        if (message.mouse.action == protocol::MouseInputAction::release) {
          session.attachment.mouse_capture.reset();
        }
        break;
      }
      if (captured_continuation &&
          session.attachment.mouse_capture->owner == MouseCaptureOwner::divider) {
        const auto capture = *session.attachment.mouse_capture;
        auto* const captured_tab = find_tab(session, capture.target.tab);
        if (captured_tab == nullptr || captured_tab->id != session.active_tab ||
            captured_tab->zoomed || captured_tab->layout_suspended) {
          session.active = false;
          return ParseResult::error;
        }
        const auto coordinate =
            capture.divider_axis == SplitAxis::left_right ? message.mouse.column : content_row;
        const Command resize{
            .kind = capture.divider_axis == SplitAxis::left_right
                        ? CommandKind::resize_left_right_divider
                        : CommandKind::resize_top_bottom_divider,
            .origin = CommandOrigin::client,
            .target = {.session = session.id,
                       .tab = capture.target.tab,
                       .pane = capture.target.pane,
                       .peer_pane = capture.peer_pane,
                       .attachment = session.attachment.id},
            .payload = CommandCoordinate{.value = coordinate},
        };
        const auto result = dispatch_session_command(session, runtimes, resize);
        if (result.status == CommandStatus::failed) {
          return ParseResult::error;
        }
        if (result.status != CommandStatus::applied && result.status != CommandStatus::no_effect) {
          if (message.mouse.action == protocol::MouseInputAction::release) {
            session.attachment.mouse_capture.reset();
          } else {
            session.attachment.mouse_capture->owner = MouseCaptureOwner::discard_until_release;
          }
          break;
        }
        if (message.mouse.action == protocol::MouseInputAction::release) {
          finish_live_divider_resize(session);
        }
        break;
      }

      Tab* target_tab = tab;
      Pane* pane = nullptr;
      auto owner = MouseCaptureOwner::application;
      if (captured_continuation) {
        const auto capture = *session.attachment.mouse_capture;
        auto* const captured_tab = find_tab(session, capture.target.tab);
        auto* const captured = captured_tab == nullptr
                                   ? nullptr
                                   : find_pane(session, *captured_tab, capture.target.pane);
        if (captured == nullptr) {
          session.attachment.mouse_capture.reset();
          break;
        }
        target_tab = captured_tab;
        pane = captured;
        owner = capture.owner;
      } else {
        for (auto& slot : session.panes) {
          if (slot.pane == nullptr || slot.pane->tab != tab->id ||
              (tab->zoomed && slot.pane->id != tab->focused_pane)) {
            continue;
          }
          const auto& candidate = slot.pane->rectangle;
          if (message.mouse.column >= candidate.column && content_row >= candidate.row &&
              message.mouse.column < candidate.column + candidate.columns &&
              content_row < candidate.row + candidate.rows) {
            pane = slot.pane.get();
            break;
          }
        }
        if (pane == nullptr) {
          break;
        }
        if (message.mouse.action == protocol::MouseInputAction::press) {
          static_cast<void>(extensions.focus_pane(session.attachment.id));
        }
      }
      auto* const runtime = find_pane_runtime(runtimes, session, *target_tab, *pane);
      if (runtime == nullptr) {
        return ParseResult::error;
      }
      const bool wheel_button = message.mouse.button == protocol::MouseInputButton::four ||
                                message.mouse.button == protocol::MouseInputButton::five;
      if (message.mouse.action == protocol::MouseInputAction::press) {
        if (message.mouse.button == protocol::MouseInputButton::left && target_tab == tab &&
            pane->id != tab->focused_pane) {
          const Command focus{
              .kind = CommandKind::focus_pane,
              .origin = CommandOrigin::client,
              .target = {.session = session.id,
                         .tab = target_tab->id,
                         .pane = pane->id,
                         .peer_pane = {},
                         .attachment = session.attachment.id},
          };
          if (!dispatch_session_command(session, runtimes, focus).succeeded()) {
            return ParseResult::error;
          }
        }
        const auto tracking = runtime->terminal.mouse_tracking();
        if (!tracking.has_value()) {
          return ParseResult::error;
        }
        const AttachmentPaneTarget target{.tab = target_tab->id, .pane = pane->id};
        const bool copy_selection = session.attachment.copy_mode.active() &&
                                    session.attachment.selection_target == std::optional{target};
        if (wheel_button && (copy_selection || !tracking->enabled)) {
          if (!copy_selection) {
            const auto alternate_scroll = runtime->terminal.wheel_uses_alternate_scroll();
            if (!alternate_scroll.has_value()) {
              return ParseResult::error;
            }
            if (*alternate_scroll) {
              clear_mouse_selection(session, runtimes);
              const auto queued_bytes_before = runtime->pending_writes.size();
              const auto queued = queue_alternate_scroll_input(
                  runtime->pending_writes, runtime->terminal,
                  message.mouse.button == protocol::MouseInputButton::four);
              if (queued == InputQueueResult::full) {
                return ParseResult::backpressure;
              }
              if (queued == InputQueueResult::encoding_failed) {
                return ParseResult::error;
              }
              if (runtime->pending_writes.size() > queued_bytes_before) {
                runtime->interactive_damage.await_write(queued_bytes_before,
                                                        runtime->pending_writes.size());
              }
              break;
            }
          }
          if (!scroll_viewport_with_mouse(session, *runtime, message.mouse.button,
                                          copy_selection)) {
            return ParseResult::error;
          }
          break;
        }
        owner = message.mouse.button == protocol::MouseInputButton::left &&
                        (copy_selection || !tracking->enabled)
                    ? MouseCaptureOwner::selection
                    : MouseCaptureOwner::application;
        if (!wheel_button) {
          session.attachment.mouse_capture = MouseCapture{
              .target = target,
              .peer_pane = {},
              .status_tab_before = {},
              .owner = owner,
              .divider_axis = SplitAxis::left_right,
          };
        }
        if (owner == MouseCaptureOwner::application) {
          clear_mouse_selection(session, runtimes);
        }
      }
      if (owner == MouseCaptureOwner::selection) {
        if (!process_mouse_selection(session, runtimes, *target_tab, *pane, *runtime, message.mouse,
                                     content_row)) {
          return ParseResult::error;
        }
        if (message.mouse.action == protocol::MouseInputAction::release) {
          session.attachment.mouse_capture.reset();
        }
        break;
      }

      const auto& rectangle = pane->rectangle;
      const auto action = [value = message.mouse.action]() noexcept {
        switch (value) {
        case protocol::MouseInputAction::press:
          return vt::MouseAction::press;
        case protocol::MouseInputAction::release:
          return vt::MouseAction::release;
        case protocol::MouseInputAction::motion:
          return vt::MouseAction::motion;
        }
        return vt::MouseAction::motion;
      }();
      const vt::MouseEvent event{
          .action = action,
          .button = terminal_mouse_button(message.mouse.button),
          .modifiers = message.mouse.modifiers,
          .x = static_cast<float>(
              std::clamp(message.mouse.column, rectangle.column,
                         static_cast<std::uint16_t>(rectangle.column + rectangle.columns - 1U)) -
              rectangle.column),
          .y = static_cast<float>(
              std::clamp(content_row, rectangle.row,
                         static_cast<std::uint16_t>(rectangle.row + rectangle.rows - 1U)) -
              rectangle.row),
          .geometry = {.screen_width = rectangle.columns, .screen_height = rectangle.rows},
          .any_button_pressed = message.mouse.any_button_pressed,
      };
      const auto queued_bytes_before = runtime->pending_writes.size();
      const auto queued = queue_mouse_input(runtime->pending_writes, runtime->terminal, event);
      if (queued == InputQueueResult::full) {
        return ParseResult::backpressure;
      }
      if (queued == InputQueueResult::encoding_failed) {
        return ParseResult::error;
      }
      if (runtime->pending_writes.size() > queued_bytes_before) {
        runtime->interactive_damage.await_write(queued_bytes_before,
                                                runtime->pending_writes.size());
      }
      if (message.mouse.action == protocol::MouseInputAction::release) {
        session.attachment.mouse_capture.reset();
      }
      break;
    }
    case protocol::ClientMessageKind::host_theme:
      if (message.host_theme == nullptr) {
        return ParseResult::error;
      }
      if (!session.theme_bound) {
        if (!bind_session_theme(session, runtimes, *message.host_theme)) {
          return ParseResult::error;
        }
        schedule_frame(session, FrameUrgency::state_change, true);
      }
      break;
    case protocol::ClientMessageKind::pane_command: {
      finish_live_divider_resize(session, true);
      const bool begins_rename =
          message.pane_command == protocol::PaneCommand::begin_rename_session ||
          message.pane_command == protocol::PaneCommand::begin_rename_tab;
      if (session.attachment.rename_prompt.active() && !begins_rename) {
        reset_rename_prompt(session);
      }
      if (session.attachment.command_line.active) {
        reset_command_line(session);
      }
      const auto command = command_from_pane_command(session, message.pane_command);
      if (!command.has_value() || !dispatch_session_command(session, runtimes, *command,
                                                            name_conflict, name_conflict_context)
                                       .succeeded()) {
        if (!session.active) {
          session.attachment_runtime.decoder.consume();
          return ParseResult::detach;
        }
      }
      break;
    }
    }
    session.attachment_runtime.decoder.consume();
    if (!session.active) {
      return ParseResult::detach;
    }
  }
}

[[nodiscard]] auto receive_client(SessionRecord& session, PaneRuntimeStore& runtimes,
                                  extension::Runtime& extensions, std::size_t& message_budget,
                                  std::size_t& geometry_budget, std::size_t& input_budget,
                                  const SessionNameConflict name_conflict,
                                  void* const name_conflict_context) noexcept -> ParseResult {
  const auto buffered =
      parse_client_packets(session, runtimes, extensions, message_budget, geometry_budget,
                           input_budget, name_conflict, name_conflict_context);
  if (buffered != ParseResult::keep) {
    return buffered;
  }
  const auto available = session.attachment_runtime.decoder.writable_bytes();
  if (available.empty()) {
    return ParseResult::error;
  }
  const auto bytes_read =
      ::recv(session.attachment_runtime.client, available.data(), available.size(), 0);
  if (bytes_read == 0) {
    return ParseResult::peer_closed;
  }
  if (bytes_read < 0) {
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? ParseResult::keep
                                                                     : ParseResult::peer_closed;
  }
  if (!session.attachment_runtime.decoder.commit(static_cast<std::size_t>(bytes_read))
           .has_value()) {
    return ParseResult::error;
  }
  return parse_client_packets(session, runtimes, extensions, message_budget, geometry_budget,
                              input_budget, name_conflict, name_conflict_context);
}

[[nodiscard]] auto advance_copy_search_cursor(PaneRuntime& runtime, vt::TerminalPoint& point,
                                              const CopySearchDirection direction) noexcept
    -> bool {
  const auto viewport = runtime.terminal.viewport_state();
  if (!viewport.has_value() || viewport->total_rows == 0) {
    return false;
  }
  const auto columns = runtime.terminal.size().columns;
  if (direction == CopySearchDirection::forward) {
    if (point.column + 1U < columns) {
      ++point.column;
      return true;
    }
    if (static_cast<std::uint64_t>(point.row) + 1U >= viewport->total_rows) {
      return false;
    }
    point.column = 0;
    ++point.row;
    return true;
  }
  if (point.column > 0) {
    --point.column;
    return true;
  }
  if (point.row == 0) {
    return false;
  }
  point.column = static_cast<std::uint16_t>(columns - 1U);
  --point.row;
  return true;
}

[[nodiscard]] auto active_copy_search_query(const CopyModeState& state) noexcept
    -> std::string_view {
  return state.phase == CopyModePhase::search_prompt ? state.draft_query_view()
                                                     : state.query_view();
}

[[nodiscard]] auto restart_copy_search_after_mutation(SessionRecord& session,
                                                      PaneRuntime& runtime) noexcept -> bool {
  auto& search = session.attachment_runtime.copy_mode;
  if (!search.search_task.has_value()) {
    return false;
  }
  const auto direction = search.search_task->direction;
  const auto endpoint = runtime.terminal.selection_checkpoint_endpoint(vt::PointSpace::screen);
  return endpoint.has_value() && endpoint->has_value() &&
         begin_copy_search(session, runtime, direction, **endpoint);
}

[[nodiscard]] auto finish_copy_search_match(SessionRecord& session, PaneRuntime& runtime,
                                            const vt::SearchMatch match) noexcept -> bool {
  auto& state = session.attachment.copy_mode;
  auto& search = session.attachment_runtime.copy_mode;
  const auto selected = runtime.terminal.select_search_match(match);
  if (!selected.has_value()) {
    return false;
  }
  const auto viewport = runtime.terminal.viewport_state();
  if (!viewport.has_value()) {
    return false;
  }
  const auto target_offset = copy_search_viewport_offset(
      match.end.row, viewport->offset, viewport->visible_rows, viewport->total_rows);
  if (target_offset != viewport->offset) {
    runtime.terminal.scroll_viewport(vt::ViewportScroll::row,
                                     static_cast<std::int64_t>(target_offset));
    record_terminal_observation(runtime);
  }
  if (!update_copy_viewport_offset(session, runtime)) {
    return false;
  }
  search.search_task.reset();
  state.feedback = CopyModeFeedback::none;
  if (state.phase == CopyModePhase::search_prompt) {
    search.preview_match = true;
  } else {
    search.last_search_match = match;
    search.last_search_generation = runtime.mutation_generation;
    search.preview_match = false;
    discard_copy_search_restore(runtime, search);
    state.phase = CopyModePhase::navigation;
  }
  note_compression_activity(runtime);
  invalidate_copy_presentation(session, runtime);
  return true;
}

[[nodiscard]] auto finish_or_wrap_failed_copy_search(SessionRecord& session,
                                                     PaneRuntime& runtime) noexcept -> bool {
  auto& state = session.attachment.copy_mode;
  auto& search = session.attachment_runtime.copy_mode;
  if (!search.search_task.has_value()) {
    return false;
  }
  auto& task = *search.search_task;
  if (!task.wrapped) {
    const auto boundary = copy_search_boundary(runtime, task.direction);
    if (!boundary.has_value()) {
      return false;
    }
    task.wrapped = true;
    task.cursor = vt::SearchCursor{.candidate = *boundary};
    task.deadline = reactor_now();
    return true;
  }
  search.search_task.reset();
  search.preview_match = false;
  state.feedback = CopyModeFeedback::no_match;
  if (!restore_copy_search_selection(session, runtime)) {
    return false;
  }
  if (state.phase == CopyModePhase::searching) {
    discard_copy_search_restore(runtime, search);
    state.phase = CopyModePhase::navigation;
  }
  note_compression_activity(runtime);
  invalidate_copy_presentation(session, runtime);
  return true;
}

[[nodiscard]] auto service_copy_search(SessionRecord& session, PaneRuntimeStore& runtimes,
                                       std::size_t& work_budget) noexcept -> bool {
  auto& state = session.attachment.copy_mode;
  auto& search = session.attachment_runtime.copy_mode;
  auto* const task = search.search_task.has_value() ? &*search.search_task : nullptr;
  if (work_budget == 0 || !state.active() || task == nullptr || reactor_now() < task->deadline) {
    return false;
  }
  auto* const runtime = copy_mode_runtime(session, runtimes);
  if (runtime == nullptr) {
    leave_copy_mode(session, runtimes);
    return true;
  }
  const auto query = active_copy_search_query(state);
  if (query.empty()) {
    leave_copy_mode(session, runtimes);
    return true;
  }
  if (runtime->mutation_generation != task->terminal_generation) {
    if (!restart_copy_search_after_mutation(session, *runtime)) {
      leave_copy_mode(session, runtimes);
    }
    return true;
  }

  const auto work_limit = std::min(work_budget, limits::search_candidates_per_step);
  work_budget -= work_limit;
  const auto stop = task->wrapped ? std::optional{task->stop_before} : std::nullopt;
  const auto searched = runtime->terminal.search_literal_step(
      query, terminal_search_direction(task->direction), task->cursor, work_limit, stop);
  if (!searched.has_value()) {
    leave_copy_mode(session, runtimes);
    return true;
  }
  bool valid = true;
  switch (searched->status) {
  case vt::SearchStepStatus::found:
    valid = finish_copy_search_match(session, *runtime, searched->match);
    break;
  case vt::SearchStepStatus::pending:
    task->cursor = searched->next;
    task->deadline = reactor_now() + copy_search_slice_delay;
    break;
  case vt::SearchStepStatus::not_found:
    valid = finish_or_wrap_failed_copy_search(session, *runtime);
    break;
  }
  if (!valid) {
    leave_copy_mode(session, runtimes);
  }
  return true;
}

[[nodiscard]] auto find_session(Sessions& sessions, const std::string_view name) noexcept
    -> SessionRecord* {
  for (auto& session : sessions) {
    if (session != nullptr && session->active && session->session_name() == name) {
      return session.get();
    }
  }
  return nullptr;
}

[[nodiscard]] auto allocate_numeric_session_name(Sessions& sessions) noexcept
    -> std::optional<SessionName> {
  // At most 64 live sessions exist, so one of the first 65 numeric names must be free even when
  // every live Session has an explicit numeric name.
  for (std::size_t candidate = 0; candidate <= Sessions::capacity(); ++candidate) {
    SessionName name;
    const auto encoded = std::to_chars(name.bytes.begin(), name.bytes.end(), candidate);
    if (encoded.ec != std::errc{}) {
      return std::nullopt;
    }
    name.size = static_cast<std::size_t>(std::distance(name.bytes.begin(), encoded.ptr));
    if (find_session(sessions, name.view()) == nullptr) {
      return name;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto most_recent_detached_session(Sessions& sessions) noexcept -> SessionRecord* {
  SessionRecord* selected = nullptr;
  for (auto& session : sessions) {
    if (session == nullptr || !session->active || session->attachment_runtime.client >= 0 ||
        session->attachment_runtime.pending_attach_slot !=
            std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }
    if (selected == nullptr || session->activity_order > selected->activity_order) {
      selected = session.get();
    }
  }
  return selected;
}

[[nodiscard]] auto session_name_conflict(void* const context, const SessionId renamed,
                                         const std::string_view candidate) noexcept -> bool {
  auto& sessions = *static_cast<Sessions*>(context);
  for (const auto& session : sessions) {
    if (session != nullptr && session->active && session->id != renamed &&
        session->session_name() == candidate) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] auto process_exit_from_wait_status(int status) noexcept -> ProcessExit {
  if (WIFEXITED(status)) {
    return {.kind = ProcessExitKind::exited,
            .value = static_cast<std::uint32_t>(WEXITSTATUS(status))};
  }
  if (WIFSIGNALED(status)) {
    return {.kind = ProcessExitKind::signaled,
            .value = static_cast<std::uint32_t>(WTERMSIG(status))};
  }
  return {};
}

void record_reaped_child(Sessions& sessions, PaneRuntimeStore& runtimes,
                         const ChildExit child_exit) noexcept {
  for (auto& session : sessions) {
    if (session == nullptr || !session->active) {
      continue;
    }
    for (auto& pane_slot : session->panes) {
      if (pane_slot.pane == nullptr) {
        continue;
      }
      auto* const runtime = find_pane_runtime(runtimes, *session, *pane_slot.pane);
      LEMMA_ASSERT(runtime != nullptr);
      if (runtime->child == child_exit.process) {
        runtime->observed_exit = process_exit_from_wait_status(child_exit.status);
        runtime->child = -1;
        return;
      }
    }
  }
  // Already-closed panes are also daemon children and require no Core transition.
}

[[nodiscard]] auto reap_exited_children(Sessions& sessions, PaneRuntimeStore& runtimes,
                                        const ChildReaper child_reaper) noexcept -> bool {
  LEMMA_ASSERT(child_reaper.valid());
  bool reaped = false;
  while (const auto exited = child_reaper.reap(child_reaper.context)) {
    record_reaped_child(sessions, runtimes, *exited);
    reaped = true;
  }
  return reaped;
}

void reclaim_inactive_sessions(Sessions& sessions, PaneRuntimeStore& runtimes,
                               extension::Runtime& extensions) noexcept {
  for (auto& session : sessions) {
    if (session != nullptr && !session->active &&
        session->attachment_runtime.pending_attach_slot ==
            std::numeric_limits<std::uint32_t>::max()) {
      const auto id = session->id;
      extensions.revoke_session(id);
      runtimes.erase_session(id);
      const bool erased = sessions.erase(id);
      LEMMA_ASSERT(erased);
    }
  }
}

[[nodiscard]] auto append_all_listings(ConnectionOutput& output, const Sessions& sessions,
                                       const PaneRuntimeStore& runtimes) noexcept -> bool {
  std::size_t listed = 0;
  for (const auto& session : sessions) {
    if (session != nullptr && session->active) {
      if (!append_listing(output, *session, runtimes)) {
        return false;
      }
      ++listed;
    }
  }
  return listed > 0 || output.append_text("no lemma sessions\n");
}

[[nodiscard]] auto acquire_public_scratch(PublicScratch& owner) noexcept -> std::span<std::byte> {
  if (owner == nullptr) {
    try {
      owner = std::make_unique_for_overwrite<PublicScratchStorage>();
    } catch (...) {
      return {};
    }
  }
  return std::span(*owner).first(public_capture_bytes_max);
}

[[nodiscard]] auto tab_move_anchor(const SessionRecord& session, TabId moving,
                                   std::size_t one_based_position) noexcept
    -> std::optional<std::optional<TabId>>;

enum class AttachmentTransferResult : std::uint8_t {
  transferred,
  deferred,
  conflict,
  capacity,
  unavailable,
  failed,
};

[[nodiscard]] auto transfer_attachment(SessionRecord& source, SessionRecord& target,
                                       Sessions& sessions, PaneRuntimeStore& runtimes,
                                       std::uint64_t& activity_order) noexcept
    -> AttachmentTransferResult;

[[nodiscard]] auto switch_attachment(const api::Command& request, Sessions& sessions,
                                     SessionRecord& target, PaneRuntimeStore& runtimes,
                                     std::uint64_t& activity_order, PublicCommandExecution& result)
    -> CommandStatus {
  SessionRecord* source = nullptr;
  for (const auto& candidate : sessions) {
    if (candidate != nullptr && candidate->active && candidate->attachment_runtime.client >= 0 &&
        candidate->attachment_runtime.connection_id == request.connection) {
      source = candidate.get();
      break;
    }
  }
  if (source == nullptr) {
    result.error_reason = "stale_connection";
    return CommandStatus::stale_target;
  }
  const bool unchanged = source == &target;
  const auto transferred =
      unchanged ? AttachmentTransferResult::transferred
                : transfer_attachment(*source, target, sessions, runtimes, activity_order);
  switch (transferred) {
  case AttachmentTransferResult::transferred:
    result.value_field = "connection";
    result.session_revision = target.mutation_generation;
    if (!append_public_id(result.value_json, target.attachment_runtime.connection_id)) {
      return CommandStatus::failed;
    }
    return unchanged ? CommandStatus::no_effect : CommandStatus::applied;
  case AttachmentTransferResult::deferred:
    result.error_reason = "output_pending";
    result.retryable = true;
    return CommandStatus::conflict;
  case AttachmentTransferResult::conflict:
    result.error_reason = "target_attached";
    return CommandStatus::conflict;
  case AttachmentTransferResult::capacity:
    return CommandStatus::capacity;
  case AttachmentTransferResult::unavailable:
    return CommandStatus::stale_target;
  case AttachmentTransferResult::failed:
    return CommandStatus::failed;
  }
  return CommandStatus::failed;
}

class PublicCommandExecutor final {
public:
  [[nodiscard]] static auto execute(const api::Command& request, Sessions& sessions,
                                    PaneRuntimeStore& runtimes, std::uint64_t& activity_order,
                                    std::span<std::byte> scratch) -> PublicCommandExecution;
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto PublicCommandExecutor::execute(const api::Command& request, Sessions& sessions,
                                    PaneRuntimeStore& runtimes, std::uint64_t& activity_order,
                                    const std::span<std::byte> scratch) -> PublicCommandExecution {
  PublicCommandExecution result;
  if (request.kind == api::CommandKind::daemon_inspect) {
    result.value_json = daemon_inspection(sessions, runtimes);
    result.value_field = "daemon";
    result.status = result.value_json.empty() ? CommandStatus::failed : CommandStatus::applied;
    return result;
  }
  if (request.kind == api::CommandKind::session_list) {
    ConnectionOutput encoded;
    result.status = append_structured_sessions(encoded, sessions) &&
                            copy_connection_json(encoded, result.value_json)
                        ? CommandStatus::applied
                        : CommandStatus::failed;
    result.value_field = "sessions";
    return result;
  }
  if (request.kind == api::CommandKind::session_start) {
    SessionName allocated;
    std::string_view name = request.name;
    if (name.empty()) {
      const auto candidate = allocate_numeric_session_name(sessions);
      if (!candidate.has_value()) {
        result.status = CommandStatus::capacity;
        return result;
      }
      allocated = *candidate;
      name = allocated.view();
    }
    if (find_session(sessions, name) != nullptr) {
      result.status = CommandStatus::conflict;
      return result;
    }
    if (sessions.size() == Sessions::capacity() ||
        activity_order == std::numeric_limits<std::uint64_t>::max()) {
      result.status = CommandStatus::capacity;
      return result;
    }
    std::vector<std::byte> command;
    if (!public_launch_command(request, command)) {
      result.status = CommandStatus::failed;
      return result;
    }
    std::array<char, limits::working_directory_bytes_max + 1U> default_directory{};
    std::string_view working_directory = request.working_directory;
    if (working_directory.empty()) {
      working_directory = reactor_default_cwd();
    }
    if (working_directory.empty()) {
      const auto size = platform::account_home_directory(default_directory);
      if (size == 0) {
        result.status = CommandStatus::failed;
        return result;
      }
      working_directory = std::string_view(default_directory.data(), size);
    }
    std::vector<std::byte> environment;
    if (request.environment_set) {
      if (!public_environment(request, environment)) {
        result.status = CommandStatus::failed;
        return result;
      }
    } else {
      try {
        environment.resize(limits::environment_bytes_max);
      } catch (...) {
        result.status = CommandStatus::failed;
        return result;
      }
      const auto captured = platform::capture_process_environment(environment);
      if (!captured.has_value()) {
        result.status = CommandStatus::failed;
        return result;
      }
      environment.resize(*captured);
    }
    auto created =
        create_session(name, working_directory, environment, LaunchEnvironmentMode::replace);
    if (created == nullptr) {
      result.status = CommandStatus::failed;
      return result;
    }
    const auto id = sessions.insert(std::move(created));
    if (!id.has_value()) {
      result.status = CommandStatus::capacity;
      return result;
    }
    auto* const inserted = sessions.get(*id);
    LEMMA_ASSERT(inserted != nullptr);
    inserted->id = *id;
    inserted->attachment.id = AttachmentId::from_parts(id->slot(), id->generation());
    inserted->attachment.session = *id;
    inserted->activity_order = ++activity_order;
    const auto policy = request.hold ? PaneExitPolicy::hold : PaneExitPolicy::close;
    auto* const tab = create_tab(*inserted, runtimes, command, working_directory, policy);
    if (tab == nullptr) {
      const bool erased = sessions.erase(*id);
      LEMMA_ASSERT(erased);
      result.status = CommandStatus::failed;
      return result;
    }
    inserted->previous_tab = inserted->active_tab;
    result.status = CommandStatus::applied;
    result.session_name = std::string(inserted->session_name());
    result.session = inserted->id;
    result.session_revision = inserted->mutation_generation;
    result.tab = tab->id;
    result.pane = tab->focused_pane;
    return result;
  }

  auto* const session = public_session(sessions, request.session);
  if (session == nullptr || !session->active) {
    result.status = CommandStatus::stale_target;
    return result;
  }
  result.session_name = std::string(session->session_name());
  result.session = session->id;
  result.session_revision = session->mutation_generation;
  if (request.expected_session_revision.has_value() &&
      *request.expected_session_revision != session->mutation_generation) {
    result.status = CommandStatus::conflict;
    result.error_reason = "revision_mismatch";
    result.retryable = true;
    return result;
  }

  if (request.kind == api::CommandKind::attachment_switch) {
    result.status =
        switch_attachment(request, sessions, *session, runtimes, activity_order, result);
    return result;
  }
  if (request.kind == api::CommandKind::session_inspect) {
    result.value_json = session_inspection(*session);
    result.value_field = "session_state";
    result.status = result.value_json.empty() ? CommandStatus::failed : CommandStatus::applied;
    return result;
  }
  if (request.kind == api::CommandKind::tab_inspect) {
    auto* const tab = public_tab(*session, request.tab);
    if (tab == nullptr) {
      result.status = CommandStatus::stale_target;
      return result;
    }
    result.tab = tab->id;
    result.value_json = tab_inspection(*session, *tab, runtimes);
    result.value_field = "tab_state";
    result.status = result.value_json.empty() ? CommandStatus::failed : CommandStatus::applied;
    return result;
  }

  if (request.kind == api::CommandKind::tab_list || request.kind == api::CommandKind::pane_list) {
    ConnectionOutput encoded;
    bool appended = false;
    if (request.kind == api::CommandKind::tab_list) {
      appended = append_structured_tabs(encoded, *session, runtimes);
      result.value_field = "tabs";
    } else {
      appended = append_structured_panes(encoded, *session, runtimes);
      result.value_field = "panes";
    }
    result.status = appended && copy_connection_json(encoded, result.value_json)
                        ? CommandStatus::applied
                        : CommandStatus::failed;
    return result;
  }
  if (request.kind == api::CommandKind::session_rename) {
    const auto name = SessionNameValue::create(request.name);
    if (!name.has_value()) {
      result.status = CommandStatus::invalid_command;
      return result;
    }
    Command command{.kind = CommandKind::rename_session,
                    .origin = CommandOrigin::cli,
                    .target = {.session = session->id,
                               .tab = {},
                               .pane = {},
                               .peer_pane = {},
                               .attachment = {}},
                    .payload = *name};
    result.status =
        dispatch_session_command(*session, runtimes, command, &session_name_conflict, &sessions)
            .status;
    if (result.status == CommandStatus::applied) {
      result.session_name = request.name;
    }
    result.session_revision = session->mutation_generation;
    return result;
  }
  if (request.kind == api::CommandKind::session_kill) {
    const Command command{.kind = CommandKind::stop_session, .origin = CommandOrigin::cli};
    result.status = dispatch_session_command(*session, runtimes, command).status;
    result.session_revision = session->mutation_generation;
    return result;
  }
  if (request.kind == api::CommandKind::tab_new) {
    if (tab_count(*session) >= session->tabs.size() ||
        pane_count(*session) >= panes_per_session_max ||
        runtimes.size() >= limits::panes_hard_max) {
      result.status = CommandStatus::capacity;
      return result;
    }
    std::vector<std::byte> command;
    if (!public_launch_command(request, command)) {
      result.status = CommandStatus::failed;
      return result;
    }
    auto* const created = create_tab(*session, runtimes, command, request.working_directory,
                                     request.hold ? PaneExitPolicy::hold : PaneExitPolicy::close,
                                     request.focus == api::FocusPolicy::created);
    if (created == nullptr) {
      result.status = CommandStatus::failed;
      return result;
    }
    if (!created->set_title_override(request.title)) {
      const Command rollback{.kind = CommandKind::close_tab,
                             .origin = CommandOrigin::internal,
                             .target = {.session = session->id,
                                        .tab = created->id,
                                        .pane = {},
                                        .peer_pane = {},
                                        .attachment = {}}};
      static_cast<void>(dispatch_session_command(*session, runtimes, rollback));
      result.status = CommandStatus::failed;
      return result;
    }
    result.status = CommandStatus::applied;
    result.session_revision = session->mutation_generation;
    result.tab = created->id;
    result.pane = created->focused_pane;
    const auto* const created_pane = find_pane(*session, created->focused_pane);
    const auto* const created_runtime =
        created_pane == nullptr ? nullptr : find_pane_runtime(runtimes, *session, *created_pane);
    result.terminal_generation =
        created_runtime == nullptr ? 0 : created_runtime->observation_generation;
    return result;
  }
  if (request.kind == api::CommandKind::tab_select || request.kind == api::CommandKind::tab_move ||
      request.kind == api::CommandKind::tab_rename || request.kind == api::CommandKind::tab_kill) {
    auto* const tab = public_tab(*session, request.tab);
    if (tab == nullptr) {
      result.status = CommandStatus::stale_target;
      return result;
    }
    result.tab = tab->id;
    Command command{
        .origin = CommandOrigin::cli,
        .target = {
            .session = session->id, .tab = tab->id, .pane = {}, .peer_pane = {}, .attachment = {}}};
    if (request.kind == api::CommandKind::tab_select) {
      command.kind = CommandKind::select_tab;
      command.payload =
          CommandCoordinate{.value = static_cast<std::uint16_t>(
                                request.tab.position == 0 ? 0 : request.tab.position - 1U)};
    } else if (request.kind == api::CommandKind::tab_kill) {
      command.kind = CommandKind::close_tab;
    } else if (request.kind == api::CommandKind::tab_move) {
      const auto anchor = tab_move_anchor(*session, tab->id, request.to_position);
      if (!anchor.has_value()) {
        result.status = CommandStatus::invalid_target;
        return result;
      }
      command.kind = CommandKind::place_tab;
      command.payload = TabPlacementCommand{.before = anchor->value_or(TabId{})};
    } else {
      const auto title = TabTitleValue::create(request.title);
      if (!title.has_value()) {
        result.status = CommandStatus::invalid_command;
        return result;
      }
      command.kind = CommandKind::rename_tab;
      command.payload = *title;
    }
    result.status =
        dispatch_session_command(*session, runtimes, command, &session_name_conflict, &sessions)
            .status;
    result.session_revision = session->mutation_generation;
    return result;
  }

  auto* const pane = find_pane(*session, request.pane.id);
  auto* const tab = pane == nullptr ? nullptr : find_tab(*session, pane->tab);
  auto* const runtime = pane == nullptr ? nullptr : find_pane_runtime(runtimes, *session, *pane);
  if (pane == nullptr || tab == nullptr || runtime == nullptr) {
    result.status = CommandStatus::stale_target;
    return result;
  }
  result.tab = tab->id;
  result.pane = pane->id;
  result.terminal_generation = runtime->observation_generation;
  if (request.kind == api::CommandKind::pane_inspect) {
    result.value_json = pane_inspection(*tab, *pane, *runtime);
    result.value_field = "pane_state";
    result.status = result.value_json.empty() ? CommandStatus::failed : CommandStatus::applied;
    return result;
  }
  if (request.kind == api::CommandKind::pane_split) {
    if (pane_count(*session) >= panes_per_session_max ||
        runtimes.size() >= limits::panes_hard_max) {
      result.status = CommandStatus::capacity;
      return result;
    }
    std::vector<std::byte> command;
    if (!public_launch_command(request, command)) {
      result.status = CommandStatus::failed;
      return result;
    }
    const auto axis =
        request.direction == api::Direction::right ? SplitAxis::left_right : SplitAxis::top_bottom;
    auto* const created =
        split_pane(*session, *tab, runtimes, pane->id, axis, command, request.working_directory,
                   request.hold ? PaneExitPolicy::hold : PaneExitPolicy::close,
                   request.focus == api::FocusPolicy::created);
    result.status = created == nullptr ? CommandStatus::failed : CommandStatus::applied;
    if (created != nullptr) {
      result.pane = created->id;
      const auto* const created_runtime = find_pane_runtime(runtimes, *session, *created);
      result.terminal_generation =
          created_runtime == nullptr ? 0 : created_runtime->observation_generation;
    }
    result.session_revision = session->mutation_generation;
    return result;
  }
  if (request.kind == api::CommandKind::pane_send) {
    const auto queued = queue_application_bytes_to(
        *session, runtimes, *runtime,
        std::as_bytes(std::span(request.text.data(), request.text.size())));
    result.status =
        queued == InputQueueResult::queued ? CommandStatus::applied : CommandStatus::unavailable;
    if (queued != InputQueueResult::queued) {
      result.error_reason =
          queued == InputQueueResult::full ? "input_backpressure" : "input_unavailable";
      result.retryable = queued == InputQueueResult::full;
    }
    return result;
  }
  if (request.kind == api::CommandKind::pane_input) {
    const auto queued =
        queue_public_input_batch(*session, runtimes, *runtime, request.input_events);
    result.status =
        queued == InputQueueResult::queued ? CommandStatus::applied : CommandStatus::unavailable;
    if (queued != InputQueueResult::queued) {
      result.error_reason =
          queued == InputQueueResult::full ? "input_backpressure" : "input_unavailable";
      result.retryable = queued == InputQueueResult::full;
    }
    return result;
  }
  if (request.kind == api::CommandKind::pane_capture) {
    const auto format = request.capture_format == api::CaptureFormat::plain
                            ? vt::ScreenFormat::plain
                            : vt::ScreenFormat::vt;
    const bool unwrap = request.capture_wrap == api::CaptureWrap::logical;
    std::expected<std::size_t, vt::Error> formatted = std::unexpected(vt::Error::invalid_state);
    if (request.capture_source == api::CaptureSource::visible) {
      auto visible = format_public_visible(runtime->terminal, format, request.capture_wrap,
                                           request.lines, scratch);
      formatted = std::move(visible.result);
      result.capture_truncated = visible.truncated;
    } else if (request.capture_source == api::CaptureSource::recent) {
      auto rows = static_cast<std::size_t>(request.lines == 0 ? 80U : request.lines);
      formatted = runtime->terminal.format_recent(format, rows, scratch, unwrap);
      while (!formatted.has_value() && formatted.error() == vt::Error::out_of_space && rows > 1U) {
        rows /= 2U;
        result.capture_truncated = true;
        formatted = runtime->terminal.format_recent(format, rows, scratch, unwrap);
      }
    } else {
      formatted = runtime->terminal.format_last_command(format, scratch, unwrap);
    }
    if (!formatted.has_value()) {
      result.status = request.capture_source == api::CaptureSource::last_command
                          ? CommandStatus::unavailable
                          : CommandStatus::failed;
      result.error_reason = formatted.error() == vt::Error::out_of_space ? "capture_too_large"
                                                                         : "capture_unavailable";
      return result;
    }
    try {
      // Byte payloads and character storage have the same object representation.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      result.text.assign(reinterpret_cast<const char*>(scratch.data()), *formatted);
    } catch (...) {
      result.status = CommandStatus::failed;
      return result;
    }
    result.has_text = true;
    result.has_capture = true;
    result.capture_source = request.capture_source;
    result.capture_format = request.capture_format;
    result.capture_wrap = request.capture_wrap;
    result.terminal_generation = runtime->observation_generation;
    result.status = CommandStatus::applied;
    return result;
  }

  Command command{.origin = CommandOrigin::cli,
                  .target = {.session = session->id,
                             .tab = tab->id,
                             .pane = pane->id,
                             .peer_pane = {},
                             .attachment = {}}};
  if (request.kind == api::CommandKind::pane_focus) {
    command.kind = CommandKind::focus_pane;
  } else if (request.kind == api::CommandKind::pane_kill) {
    command.kind = CommandKind::close_pane;
  } else if (request.kind == api::CommandKind::pane_swap) {
    command.kind = CommandKind::swap_panes;
    command.payload = PaneSwapCommand{.other = request.other.id};
  } else if (request.kind == api::CommandKind::pane_zoom) {
    command.kind = CommandKind::set_zoom;
    command.payload = PaneZoomCommand{.enabled = request.enabled};
  } else {
    switch (request.direction) {
    case api::Direction::left:
      command.kind = CommandKind::resize_left;
      break;
    case api::Direction::right:
      command.kind = CommandKind::resize_right;
      break;
    case api::Direction::up:
      command.kind = CommandKind::resize_up;
      break;
    case api::Direction::down:
      command.kind = CommandKind::resize_down;
      break;
    case api::Direction::none:
      result.status = CommandStatus::invalid_command;
      return result;
    }
  }
  if (request.kind == api::CommandKind::pane_resize) {
    command.payload = CommandCoordinate{.value = request.amount};
  }
  result.status = dispatch_session_command(*session, runtimes, command).status;
  result.session_revision = session->mutation_generation;
  return result;
}

[[nodiscard]] auto public_wait_target(const api::Command& request, Sessions& sessions,
                                      PaneRuntimeStore& runtimes) noexcept -> ObservedPane {
  auto* const session = public_session(sessions, request.session);
  auto* const pane = session == nullptr ? nullptr : find_pane(*session, request.pane.id);
  auto* const runtime = pane == nullptr ? nullptr : find_pane_runtime(runtimes, *session, *pane);
  return {.session = session, .pane = pane, .runtime = runtime};
}

[[nodiscard]] auto public_wait_target(const ProcCommandWait& wait, Sessions& sessions,
                                      PaneRuntimeStore& runtimes) noexcept -> ObservedPane {
  return public_wait_target(wait.request, sessions, runtimes);
}

[[nodiscard]] auto begin_public_wait(api::Command request, Sessions& sessions,
                                     PaneRuntimeStore& runtimes) -> ProcCommandWait {
  const auto timeout = std::chrono::milliseconds(request.wait_timeout_milliseconds);
  // The next completion is relative to Command execution, not to the first later poll.
  std::uint64_t command_baseline = request.after_commands.value_or(0);
  if (request.wait_condition == api::WaitCondition::command && !request.after_commands) {
    const auto target = public_wait_target(request, sessions, runtimes);
    if (target.runtime != nullptr) {
      command_baseline = target.runtime->terminal.signals().commands;
    }
  }
  return {.request = std::move(request),
          .deadline = reactor_now() + timeout,
          .observed_terminal_generation = 0,
          .command_baseline = command_baseline,
          .exit = std::nullopt,
          .observed = false,
          .pane_was_present = false};
}

[[nodiscard]] auto command_wait_ready(const ProcCommandWait& wait,
                                      const PaneRuntime& runtime) noexcept -> bool {
  return wait.request.wait_condition == api::WaitCondition::command &&
         runtime.terminal.signals().commands > wait.command_baseline;
}

[[nodiscard]] constexpr auto is_terminal_wait(const api::Command& request) noexcept -> bool {
  return request.wait_condition == api::WaitCondition::contains ||
         request.wait_condition == api::WaitCondition::prompt;
}

[[nodiscard]] auto public_wait_has_work(const ProcCommandWait& wait, Sessions& sessions,
                                        PaneRuntimeStore& runtimes,
                                        const std::chrono::steady_clock::time_point now) noexcept
    -> bool {
  if (!wait.observed || now >= wait.deadline) {
    return true;
  }
  const auto target = public_wait_target(wait, sessions, runtimes);
  if (target.pane == nullptr || target.runtime == nullptr) {
    return wait.pane_was_present;
  }
  if (target.pane->process_exit.has_value() || command_wait_ready(wait, *target.runtime)) {
    return true;
  }
  return is_terminal_wait(wait.request) &&
         target.runtime->observation_generation > wait.request.after_terminal_generation &&
         target.runtime->observation_generation != wait.observed_terminal_generation;
}

[[nodiscard]] auto wait_process_matches(const api::Command& request,
                                        const ProcessExit process) noexcept -> bool {
  switch (request.wait_condition) {
  case api::WaitCondition::process_exit:
    return true;
  case api::WaitCondition::exit_code:
    return process.kind == ProcessExitKind::exited && process.value == request.wait_value;
  case api::WaitCondition::signal:
    return process.kind == ProcessExitKind::signaled && process.value == request.wait_value;
  case api::WaitCondition::contains:
  case api::WaitCondition::prompt:
  case api::WaitCondition::command:
    return false;
  }
  return false;
}

[[nodiscard]] auto command_failure(const std::string_view reason, const bool retryable = false)
    -> PublicCommandExecution {
  PublicCommandExecution result;
  result.status = CommandStatus::failed;
  result.error_reason = reason;
  result.retryable = retryable;
  return result;
}

[[nodiscard]] auto complete_process_wait(PublicCommandExecution result, const api::Command& request,
                                         const ProcessExit process) -> PublicCommandExecution {
  result.process = process;
  if (wait_process_matches(request, process)) {
    result.status = CommandStatus::applied;
  } else {
    result.status = CommandStatus::failed;
    result.error_reason = "unexpected_exit";
  }
  return result;
}

[[nodiscard]] auto missing_public_wait(const ProcCommandWait& wait) -> PublicCommandExecution {
  PublicCommandExecution result;
  if (!wait.observed || !wait.pane_was_present) {
    result.status = CommandStatus::stale_target;
    return result;
  }
  return complete_process_wait(std::move(result), wait.request, wait.exit.value_or(ProcessExit{}));
}

[[nodiscard]] auto public_wait_result(const ObservedPane target) -> PublicCommandExecution {
  PublicCommandExecution result;
  result.session_name = std::string(target.session->session_name());
  result.session = target.session->id;
  result.session_revision = target.session->mutation_generation;
  result.tab = target.pane->tab;
  result.pane = target.pane->id;
  result.terminal_generation = target.runtime->observation_generation;
  return result;
}

[[nodiscard]] auto complete_terminal_wait(ProcCommandWait& wait, const ObservedPane target,
                                          const std::span<std::byte> scratch,
                                          PublicCommandExecution result)
    -> std::optional<PublicCommandExecution> {
  const auto generation = target.runtime->observation_generation;
  if (generation <= wait.request.after_terminal_generation ||
      generation == wait.observed_terminal_generation) {
    return std::nullopt;
  }
  wait.observed_terminal_generation = generation;
  const auto formatted = format_public_visible(target.runtime->terminal, vt::ScreenFormat::plain,
                                               api::CaptureWrap::rendered, 0, scratch);
  const auto at_prompt = target.runtime->terminal.cursor_at_prompt();
  if (!formatted.result.has_value() || !at_prompt.has_value()) {
    result.status = CommandStatus::unavailable;
    result.error_reason = "capture_unavailable";
    return result;
  }
  // Byte payloads and character storage have the same object representation.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view text(reinterpret_cast<const char*>(scratch.data()), *formatted.result);
  const bool matched = wait.request.wait_condition == api::WaitCondition::prompt
                           ? *at_prompt
                           : text.contains(wait.request.contains);
  if (!matched) {
    return std::nullopt;
  }
  try {
    result.text = text;
  } catch (...) {
    result.status = CommandStatus::failed;
    return result;
  }
  result.has_text = true;
  result.has_capture = true;
  result.capture_source = api::CaptureSource::visible;
  result.capture_format = api::CaptureFormat::plain;
  result.capture_wrap = api::CaptureWrap::rendered;
  result.capture_truncated = formatted.truncated;
  result.status = CommandStatus::applied;
  return result;
}

[[nodiscard]] auto completed_public_wait(ProcCommandWait& wait, Sessions& sessions,
                                         PaneRuntimeStore& runtimes,
                                         const std::span<std::byte> scratch)
    -> std::optional<PublicCommandExecution> {
  const auto target = public_wait_target(wait, sessions, runtimes);
  if (target.session == nullptr || target.pane == nullptr || target.runtime == nullptr) {
    return missing_public_wait(wait);
  }
  wait.observed = true;
  wait.pane_was_present = true;
  wait.request.session.id = target.session->id;
  wait.request.session.name.clear();
  auto result = public_wait_result(target);
  if (command_wait_ready(wait, *target.runtime)) {
    const auto& signals = target.runtime->terminal.signals();
    try {
      result.value_field = "completion";
      result.value_json = R"({"commands":)" + std::to_string(signals.commands) +
                          R"(,"exit_code":)" +
                          (signals.exit_code.has_value() ? std::to_string(*signals.exit_code)
                                                         : std::string{"null"}) +
                          "}";
    } catch (...) {
      result.status = CommandStatus::failed;
      return result;
    }
    result.status = CommandStatus::applied;
    return result;
  }
  if (is_terminal_wait(wait.request)) {
    auto completed = complete_terminal_wait(wait, target, scratch, result);
    if (completed.has_value()) {
      return completed;
    }
  }
  if (target.pane->process_exit.has_value()) {
    return complete_process_wait(std::move(result), wait.request, *target.pane->process_exit);
  }
  if (reactor_now() >= wait.deadline) {
    result.status = CommandStatus::failed;
    result.error_reason = "timeout";
    result.retryable = true;
    return result;
  }
  return std::nullopt;
}

[[nodiscard]] auto poll_public_wait(ProcCommandWait& wait, Sessions& sessions,
                                    PaneRuntimeStore& runtimes, PublicScratch& scratch_owner)
    -> std::optional<PublicCommandExecution> {
  auto scratch = std::span<std::byte>{};
  if (is_terminal_wait(wait.request)) {
    scratch = acquire_public_scratch(scratch_owner);
    if (scratch.empty()) {
      return command_failure("resource_failure");
    }
  }
  return completed_public_wait(wait, sessions, runtimes, scratch);
}

struct ProcBinding final {
  std::string id;
  bool session{false};
  bool tab{false};
  bool pane{false};
};

enum class ProcReferenceField : std::uint8_t {
  session,
  tab,
  pane,
  other,
};

struct ProcReference final {
  std::size_t step{0};
  ProcReferenceField field{ProcReferenceField::session};
};

struct CompiledProcStep final {
  api::Command request;
  std::string id;
  std::array<ProcReference, 4> references{};
  std::size_t reference_count{0};
  std::optional<std::size_t> inferred_session_step;
};

struct ProcOutputIds final {
  SessionId session;
  TabId tab;
  PaneId pane;
};

struct ConnectionProcOwner final {
  std::size_t slot{limits::pending_connections_hard_max};
  std::uint32_t generation{0};
};

struct HostedProcOwner final {
  std::uint64_t invocation{0};
};

struct RuntimeProcOwner final {
  ExtensionGenerationId generation;
  std::uint32_t request_id{0};
};

[[nodiscard]] auto image_paste_owned(const ClipboardPaste& paste, const Sessions& sessions) noexcept
    -> bool {
  const auto* session = sessions.get(paste.session);
  const auto* tab = session != nullptr ? find_tab(*session, session->active_tab) : nullptr;
  return session != nullptr && session->active && session->attachment_runtime.client >= 0 &&
         session->attachment_runtime.connection_id == paste.connection && tab != nullptr &&
         tab->focused_pane == paste.pane;
}

auto begin_image_paste(const api::Command& request, Sessions& sessions,
                       PublicCommandExecution& result) -> std::shared_ptr<ClipboardPaste> {
  auto* session = public_session(sessions, request.session);
  auto* tab = session != nullptr ? find_tab(*session, session->active_tab) : nullptr;
  if (session == nullptr || tab == nullptr || session->attachment_runtime.client < 0 ||
      tab->focused_pane != request.pane.id) {
    result.status = CommandStatus::unavailable;
    result.error_reason = "focused_attachment_required";
    return {};
  }
  if (request.expected_session_revision &&
      *request.expected_session_revision != session->mutation_generation) {
    result.status = CommandStatus::conflict;
    result.error_reason = "revision_mismatch";
    return {};
  }
  auto job = std::make_shared<ClipboardPaste>();
  job->session = session->id;
  job->pane = request.pane.id;
  job->connection = session->attachment_runtime.connection_id;
  const std::array desired{vt::ClipboardContent{.mime = "image/png", .data = {}}};
  const auto started = begin_clipboard_transaction(*session, job->pane,
                                                   {.id = 0, .read = true, .contents = desired});
  if (!started) {
    result.status = CommandStatus::unavailable;
    result.error_reason =
        started.error() == vt::ClipboardStatus::busy ? "clipboard_busy" : "clipboard_unavailable";
    return {};
  }
  session->attachment_runtime.clipboard_paste = job;
  return job;
}

// Revalidate the asynchronous job's owner before saving and before paste publication.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto advance_image_paste(ClipboardPaste& job, const api::Command& request, Sessions& sessions,
                         PaneRuntimeStore& runtimes, std::uint64_t& activity_order)
    -> std::optional<PublicCommandExecution> {
  PublicCommandExecution result;
  result.status = CommandStatus::failed;
  if (!image_paste_owned(job, sessions)) {
    result.error_reason = "clipboard_owner_changed";
    return result;
  }
  if (job.file == nullptr) {
    if (job.reply == nullptr) {
      return std::nullopt;
    }
    if (job.reply->status() != vt::ClipboardStatus::success) {
      result.error_reason = job.reply->status() == vt::ClipboardStatus::denied
                                ? "clipboard_denied"
                                : "clipboard_unavailable";
      return result;
    }
    for (const auto& item : job.reply->contents()) {
      if (item.mime == "image/png") {
        job.file = clipboard::PngFile::start(item.data, reactor_now());
        break;
      }
    }
    job.reply = nullptr;
    if (job.file == nullptr) {
      result.error_reason = "clipboard_image_unavailable";
      return result;
    }
  }
  job.file->advance(reactor_now());
  if (!job.file->done()) {
    return std::nullopt;
  }
  if (job.file->path().empty()) {
    result.error_reason = "invalid_png_or_file_error";
    return result;
  }
  std::string encoded_path;
  if (!api::append_json_string(encoded_path, job.file->path())) {
    result.error_reason = "invalid_file_path";
    return result;
  }
  std::string quoted = "'";
  for (const auto character : job.file->path()) {
    if (character == '\'') {
      quoted += "'\\''";
    } else {
      quoted += character;
    }
  }
  quoted += "' ";
  auto paste = request;
  paste.kind = api::CommandKind::pane_input;
  paste.session = {.id = job.session, .name = {}};
  paste.input_events = {{.kind = api::InputEventKind::paste, .text = std::move(quoted)}};
  // The admitted Proc owner is checked immediately before this step. Native prompts are scoped to
  // their connection. Never retarget a finished clipboard read to a newly focused Pane.
  result = PublicCommandExecutor::execute(paste, sessions, runtimes, activity_order, {});
  result.value_field = "path";
  result.value_json = std::move(encoded_path);
  return result;
}

void service_interactive_paste(SessionRecord& session, Sessions& sessions,
                               PaneRuntimeStore& runtimes, std::uint64_t& activity_order) noexcept {
  auto& job = session.attachment_runtime.interactive_paste;
  if (job == nullptr) {
    return;
  }
  try {
    api::Command request;
    request.kind = api::CommandKind::pane_paste_image;
    request.session = {.id = job->session, .name = {}};
    request.pane = {.id = job->pane};
    const auto result = advance_image_paste(*job, request, sessions, runtimes, activity_order);
    if (!result) {
      return;
    }
    const bool success = result->status == CommandStatus::applied;
    publish_status_message(session,
                           success ? StatusMessageKind::information : StatusMessageKind::error,
                           success ? "Image path pasted" : "Image paste failed");
  } catch (const std::bad_alloc&) {
    // Native command boundary: cancel this job, preserving the attachment and running panes.
    publish_status_message(session, StatusMessageKind::error, "Image paste resource failure");
  }
  job.reset();
}

struct ProcExecutionState final {
  std::vector<CompiledProcStep> steps;
  std::vector<ProcOutputIds> outputs;
  std::vector<std::string> results;
  std::optional<ProcCommandWait> wait;
  std::shared_ptr<ReloadResult> reload;
  std::shared_ptr<ClipboardPaste> image_paste;
  PublicProcId id;
  std::variant<ConnectionProcOwner, HostedProcOwner, RuntimeProcOwner> owner;
  std::size_t retained_result_bytes{0};
  std::size_t next_command{0};
  bool continue_on_error{false};
  bool ok{true};
};

struct PublicProcSlot final {
  std::unique_ptr<ProcExecutionState> execution;
  std::uint32_t generation{0};
};

using PublicProcExecutions = std::array<PublicProcSlot, limits::pending_connections_hard_max>;

// Close-on-exit removes the Pane in the transition that publishes its exit. Pending waits retain
// the outcome first so they report it rather than an unknown exit.
void record_public_wait_exit(PublicProcExecutions& executions, const SessionRecord& session,
                             const PaneId pane, const ProcessExit exit) noexcept {
  for (auto& slot : executions) {
    if (slot.execution == nullptr || !slot.execution->wait.has_value()) {
      continue;
    }
    auto& wait = *slot.execution->wait;
    const auto& selector = wait.request.session;
    const bool same_session = selector.id.is_valid() ? selector.id == session.id
                                                     : selector.name == session.session_name();
    if (same_session && wait.request.pane.id == pane) {
      wait.exit = exit;
      wait.observed = true;
      wait.pane_was_present = true;
    }
  }
}

[[nodiscard]] auto mutable_json_member(api::JsonValue& object, const std::string_view key) noexcept
    -> api::JsonValue* {
  const auto found = std::ranges::find_if(
      object.object, [&](const api::JsonMember& entry) { return entry.key == key; });
  return found == object.object.end() ? nullptr : &found->value;
}

void erase_json_member(api::JsonValue& object, const std::string_view key) {
  std::erase_if(object.object, [&](const api::JsonMember& entry) { return entry.key == key; });
}

[[nodiscard]] auto proc_id_valid(const std::string_view id) noexcept -> bool {
  return !id.empty() && id.size() <= 32U && std::ranges::all_of(id, [](const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
  });
}

[[nodiscard]] auto proc_binding(const std::span<const ProcBinding> bindings,
                                const std::string_view id) noexcept -> const ProcBinding* {
  const auto found =
      std::ranges::find_if(bindings, [&](const ProcBinding& binding) { return binding.id == id; });
  return found == bindings.end() ? nullptr : &*found;
}

[[nodiscard]] auto proc_resource(const std::string_view key) noexcept -> std::string_view {
  if (key == "session") {
    return "session";
  }
  return key == "tab" ? std::string_view{"tab"} : std::string_view{"pane"};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto proc_json_string(std::string value) -> api::JsonValue {
  return {.kind = api::JsonKind::string,
          .boolean = false,
          .number = 0,
          .string = std::move(value),
          .array = {},
          .object = {}};
}

[[nodiscard]] auto proc_json_object() -> api::JsonValue {
  return {.kind = api::JsonKind::object,
          .boolean = false,
          .number = 0,
          .string = {},
          .array = {},
          .object = {}};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto replace_proc_reference(api::JsonValue& request, const std::string_view key,
                                          const std::span<const ProcBinding> bindings) -> bool {
  auto* const selector = mutable_json_member(request, key);
  if (selector == nullptr || selector->kind != api::JsonKind::object) {
    return true;
  }
  const auto reference = api::json_string(*selector, "result");
  if (!reference.has_value()) {
    return true;
  }
  if (selector->object.empty() || selector->object.size() > 2U ||
      std::ranges::any_of(selector->object, [](const api::JsonMember& member) {
        return member.key != "result" && member.key != "field";
      })) {
    return false;
  }
  const auto field = api::json_string(*selector, "field");
  if (api::json_member(*selector, "field") != nullptr && !field.has_value()) {
    return false;
  }
  const auto resource = proc_resource(key);
  if (field.has_value() && *field != resource) {
    return false;
  }
  const auto* const binding = proc_binding(bindings, *reference);
  const bool supported = binding != nullptr && ((resource == "session" && binding->session) ||
                                                (resource == "tab" && binding->tab) ||
                                                (resource == "pane" && binding->pane));
  if (!supported) {
    return false;
  }
  std::optional<api::JsonValue> inferred_parent;
  if (key != "session" && mutable_json_member(request, "session") == nullptr) {
    auto parent = proc_json_object();
    parent.object.push_back({.key = "name", .value = proc_json_string("placeholder")});
    inferred_parent = std::move(parent);
  }
  selector->object.clear();
  if (resource == "session") {
    selector->object.push_back({.key = "name", .value = proc_json_string("placeholder")});
    return true;
  }
  selector->object.push_back({.key = "id", .value = proc_json_string("0:1")});
  if (inferred_parent.has_value()) {
    request.object.push_back({.key = "session", .value = std::move(*inferred_parent)});
  }
  return true;
}

[[nodiscard]] auto compile_proc_command(const api::JsonValue& source,
                                        const std::span<const ProcBinding> bindings)
    -> std::optional<api::Command> {
  if (source.kind != api::JsonKind::object) {
    return std::nullopt;
  }
  auto document = source;
  erase_json_member(document, "id");
  for (const std::string_view key : {"session", "tab", "pane", "other"}) {
    if (!replace_proc_reference(document, key, bindings)) {
      return std::nullopt;
    }
  }
  auto decoded = api::decode_command(document);
  return std::move(decoded.command);
}

[[nodiscard]] auto proc_reference_field(const std::string_view key) noexcept -> ProcReferenceField {
  if (key == "session") {
    return ProcReferenceField::session;
  }
  if (key == "tab") {
    return ProcReferenceField::tab;
  }
  return key == "pane" ? ProcReferenceField::pane : ProcReferenceField::other;
}

void compile_proc_references(const api::JsonValue& source,
                             const std::span<const ProcBinding> shapes,
                             CompiledProcStep& step) noexcept {
  const bool explicit_session = api::json_member(source, "session") != nullptr;
  for (const std::string_view key : {"session", "tab", "pane", "other"}) {
    const auto* const selector = api::json_member(source, key);
    if (selector == nullptr) {
      continue;
    }
    const auto reference = api::json_string(*selector, "result");
    if (!reference.has_value()) {
      continue;
    }
    const auto* const binding = proc_binding(shapes, *reference);
    LEMMA_ASSERT(binding != nullptr && step.reference_count < step.references.size());
    const auto binding_index = static_cast<std::size_t>(binding - shapes.data());
    std::span(step.references).subspan(step.reference_count, 1).front() = {
        .step = binding_index, .field = proc_reference_field(key)};
    ++step.reference_count;
    if (!explicit_session && key != "session" && !step.inferred_session_step.has_value()) {
      step.inferred_session_step = binding_index;
    }
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto concrete_proc_command(const CompiledProcStep& step,
                                         const std::span<const ProcOutputIds> outputs)
    -> std::optional<api::Command> {
  auto request = step.request;
  for (const auto& reference : std::span(step.references).first(step.reference_count)) {
    if (reference.step >= outputs.size()) {
      return std::nullopt;
    }
    const auto& output = outputs.subspan(reference.step, 1).front();
    switch (reference.field) {
    case ProcReferenceField::session:
      if (!output.session.is_valid()) {
        return std::nullopt;
      }
      request.session.id = output.session;
      request.session.name.clear();
      break;
    case ProcReferenceField::tab:
      if (!output.tab.is_valid()) {
        return std::nullopt;
      }
      request.tab.id = output.tab;
      request.tab.position = 0;
      break;
    case ProcReferenceField::pane:
      if (!output.pane.is_valid()) {
        return std::nullopt;
      }
      request.pane.id = output.pane;
      break;
    case ProcReferenceField::other:
      if (!output.pane.is_valid()) {
        return std::nullopt;
      }
      request.other.id = output.pane;
      break;
    }
  }
  if (step.inferred_session_step.has_value()) {
    const auto index = *step.inferred_session_step;
    if (index >= outputs.size() || !outputs.subspan(index, 1).front().session.is_valid()) {
      return std::nullopt;
    }
    request.session.id = outputs.subspan(index, 1).front().session;
    request.session.name.clear();
  }
  return request;
}

[[nodiscard]] auto proc_error(const std::string_view reason,
                              const std::optional<std::size_t> index = std::nullopt)
    -> std::string {
  std::string output = R"({"schema":"lemma.proc-result/v1","ok":false,"error":{"reason":)";
  if (!api::append_json_string(output, reason)) {
    return {};
  }
  if (index.has_value() &&
      (!append_public(output, R"(,"command_index":)") || !append_public_number(output, *index))) {
    return {};
  }
  return append_public(output, "},\"results\":[]}\n") ? output : std::string{};
}

struct ProcPrepareError final {
  std::string_view reason;
  std::optional<std::size_t> index;
};

// A Proc is validated completely with placeholder generational IDs before its first concrete
// Command executes. References remain a composition concern and never enter the Command executor.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto prepare_public_proc(api::JsonValue document)
    -> std::expected<ProcExecutionState, ProcPrepareError> {
  if (document.kind != api::JsonKind::object ||
      api::json_string(document, "schema") != std::optional{api::proc_schema}) {
    return std::unexpected(ProcPrepareError{.reason = "invalid_schema", .index = std::nullopt});
  }
  for (const auto& member : document.object) {
    if (member.key != "schema" && member.key != "on_error" && member.key != "commands") {
      return std::unexpected(ProcPrepareError{.reason = "unknown_field", .index = std::nullopt});
    }
  }
  auto* const commands = mutable_json_member(document, "commands");
  const auto* const on_error_value = api::json_member(document, "on_error");
  const auto on_error = api::json_string(document, "on_error").value_or("stop");
  if (commands == nullptr || commands->kind != api::JsonKind::array || commands->array.empty() ||
      commands->array.size() > api::proc_commands_max ||
      (on_error_value != nullptr && on_error_value->kind != api::JsonKind::string) ||
      (on_error != "stop" && on_error != "continue")) {
    return std::unexpected(ProcPrepareError{.reason = "invalid_document", .index = std::nullopt});
  }

  auto sources = std::move(commands->array);
  ProcExecutionState state;
  std::vector<ProcBinding> shapes;
  state.continue_on_error = on_error == "continue";
  state.steps.reserve(sources.size());
  state.outputs.reserve(sources.size());
  state.results.reserve(sources.size());
  shapes.reserve(sources.size());
  for (std::size_t index = 0; index < sources.size(); ++index) {
    const auto& source = std::span(sources).subspan(index, 1).front();
    auto request = compile_proc_command(source, shapes);
    if (!request.has_value()) {
      return std::unexpected(ProcPrepareError{.reason = "invalid_command", .index = index});
    }
    const auto* const id_value = api::json_member(source, "id");
    const auto id = api::json_string(source, "id");
    const bool creates = request->kind == api::CommandKind::session_start ||
                         request->kind == api::CommandKind::tab_new ||
                         request->kind == api::CommandKind::pane_split;
    if ((id_value != nullptr && !id.has_value()) ||
        (id.has_value() &&
         (!creates || !proc_id_valid(*id) || proc_binding(shapes, *id) != nullptr))) {
      return std::unexpected(ProcPrepareError{.reason = "invalid_or_duplicate_id", .index = index});
    }
    CompiledProcStep step;
    step.request = std::move(*request);
    step.id = id.has_value() ? std::string(*id) : std::string{};
    compile_proc_references(source, shapes, step);
    state.steps.push_back(std::move(step));
    shapes.push_back({.id = id.has_value() ? std::string(*id) : std::string{},
                      .session = creates,
                      .tab = creates,
                      .pane = creates});
  }
  return state;
}

[[nodiscard]] auto
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
encode_proc_result(const ProcExecutionState& state,
                   const std::optional<std::string_view> error = std::nullopt,
                   const std::optional<std::size_t> error_index = std::nullopt,
                   const bool command_completed = false) -> std::string {
  std::string output = state.ok ? R"({"schema":"lemma.proc-result/v1","ok":true)"
                                : R"({"schema":"lemma.proc-result/v1","ok":false)";
  if (error.has_value()) {
    if (!append_public(output, R"(,"partial":true,"error":{"reason":)") ||
        !api::append_json_string(output, *error)) {
      return {};
    }
    if (error_index.has_value() && (!append_public(output, R"(,"command_index":)") ||
                                    !append_public_number(output, *error_index))) {
      return {};
    }
    if (command_completed && !append_public(output, R"(,"command_completed":true)")) {
      return {};
    }
    if (!append_public(output, "}")) {
      return {};
    }
  }
  if (!append_public(output, R"(,"results":[)")) {
    return {};
  }
  for (std::size_t index = 0; index < state.results.size(); ++index) {
    if ((index > 0 && !append_public(output, ",")) ||
        !append_public(output, std::span(state.results).subspan(index, 1).front())) {
      return {};
    }
  }
  return append_public(output, "]}\n") ? output : std::string{};
}

[[nodiscard]] constexpr auto surface_command(const api::CommandKind kind) noexcept -> bool {
  return kind == api::CommandKind::surface_create || kind == api::CommandKind::surface_configure ||
         kind == api::CommandKind::surface_focus || kind == api::CommandKind::surface_close;
}

// Structural Surface mutation, dependent Grid geometry, and Pane runtime resize commit together.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto execute_surface_command(const api::Command& request,
                                           const RuntimeProcOwner& owner, Sessions& sessions,
                                           PaneRuntimeStore& runtimes,
                                           extension::Runtime& extensions) noexcept
    -> PublicCommandExecution {
  PublicCommandExecution execution;
  auto* const session = sessions.get(extensions.session(owner.generation));
  if (session == nullptr || !session->active ||
      extensions.attachment(owner.generation) != session->attachment.id) {
    execution.status = CommandStatus::stale_target;
    return execution;
  }
  const render::Viewport viewport{.columns = session->attachment.columns,
                                  .rows = pane_rows(session->attachment.rows)};
  const bool changes_geometry = request.kind != api::CommandKind::surface_focus;
  const auto previous_content = extensions.pane_viewport(session->attachment.id, viewport);
  if (!previous_content.has_value() || !extensions.begin_surface_transaction(owner.generation)) {
    execution.status = CommandStatus::failed;
    return execution;
  }
  extension::SurfaceOperationResult operation;
  if (request.kind == api::CommandKind::surface_create) {
    operation = extensions.create_surface(owner.generation, request.surface_placement,
                                          request.focusable, request.opaque, viewport);
  } else if (request.kind == api::CommandKind::surface_configure) {
    operation = extensions.configure_surface(owner.generation, request.surface.id,
                                             request.surface_placement, viewport,
                                             request.configured_focusable);
  } else if (request.kind == api::CommandKind::surface_focus) {
    operation = extensions.focus_surface(owner.generation, request.surface.id, viewport);
  } else {
    operation = extensions.close_surface(owner.generation, request.surface.id, viewport);
  }
  switch (operation.status) {
  case extension::SurfaceOperationStatus::applied:
    execution.status = CommandStatus::applied;
    break;
  case extension::SurfaceOperationStatus::no_effect:
    execution.status = CommandStatus::no_effect;
    break;
  case extension::SurfaceOperationStatus::stale:
    execution.status = CommandStatus::stale_target;
    break;
  case extension::SurfaceOperationStatus::wrong_owner:
    execution.status = CommandStatus::wrong_owner;
    break;
  case extension::SurfaceOperationStatus::invalid:
    execution.status = CommandStatus::invalid_command;
    break;
  case extension::SurfaceOperationStatus::capacity:
    execution.status = CommandStatus::capacity;
    break;
  case extension::SurfaceOperationStatus::unavailable:
    execution.status = CommandStatus::unavailable;
    break;
  }
  if (operation.status != extension::SurfaceOperationStatus::applied) {
    if (operation.status == extension::SurfaceOperationStatus::no_effect) {
      extensions.commit_surface_transaction(owner.generation);
    } else {
      static_cast<void>(extensions.rollback_surface_transaction(owner.generation));
    }
  }
  if (operation.status == extension::SurfaceOperationStatus::applied && changes_geometry) {
    const auto content = extensions.pane_viewport(session->attachment.id, viewport);
    if (!content.has_value() || !extensions.resize_surfaces(session->attachment.id, viewport)) {
      static_cast<void>(extensions.rollback_surface_transaction(owner.generation));
      execution.status = CommandStatus::failed;
      return execution;
    }
    ProductionSessionRuntimeContext runtime_context{.session = session, .runtimes = &runtimes};
    SessionMachine machine(*session, production_session_options(runtime_context));
    const auto transition =
        machine.resize_attachment(session->attachment.columns, session->attachment.rows, *content);
    apply_session_change(*session, runtimes, transition.change);
    if (transition.result.status != CommandStatus::applied &&
        transition.result.status != CommandStatus::no_effect) {
      static_cast<void>(extensions.rollback_surface_transaction(owner.generation));
      const auto restored = machine.resize_attachment(session->attachment.columns,
                                                      session->attachment.rows, *previous_content);
      apply_session_change(*session, runtimes, restored.change);
      execution.status = transition.result.status;
      return execution;
    }
    extensions.commit_surface_transaction(owner.generation);
  } else if (operation.status == extension::SurfaceOperationStatus::applied) {
    extensions.commit_surface_transaction(owner.generation);
  }
  if (execution.status == CommandStatus::applied || execution.status == CommandStatus::no_effect) {
    execution.session = session->id;
    execution.session_revision = session->mutation_generation;
    try {
      execution.session_name = std::string(session->session_name());
      if (operation.surface.is_valid()) {
        execution.value_field = "surface";
        execution.value_json = "\"" + std::to_string(operation.surface.slot()) + ":" +
                               std::to_string(operation.surface.generation()) + "\"";
      }
    } catch (...) {
      execution.session_name.clear();
      execution.value_field.clear();
      execution.value_json.clear();
    }
    schedule_frame(*session, FrameUrgency::state_change, true);
  }
  return execution;
}

void handoff_public_focus(const api::Command& request, const PublicCommandExecution& execution,
                          Sessions& sessions, extension::Runtime& extensions) noexcept {
  if ((execution.status == CommandStatus::applied ||
       execution.status == CommandStatus::no_effect) &&
      execution.session.is_valid() &&
      (request.kind == api::CommandKind::pane_focus ||
       request.kind == api::CommandKind::tab_select ||
       ((request.kind == api::CommandKind::pane_split ||
         request.kind == api::CommandKind::tab_new) &&
        request.focus == api::FocusPolicy::created))) {
    if (auto* const focused_session = sessions.get(execution.session); focused_session != nullptr) {
      static_cast<void>(extensions.focus_pane(focused_session->attachment.id));
    }
  }
}

// One call executes at most one already-validated Command from one admitted Proc.
[[nodiscard]] auto
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
execute_public_proc_step(ProcExecutionState& state, Sessions& sessions, PaneRuntimeStore& runtimes,
                         std::uint64_t& activity_order, PublicScratch& scratch_owner,
                         extension::Runtime& extensions) -> std::optional<std::string> {
  const auto command_count = state.steps.size();
  if (state.next_command >= command_count) {
    return encode_proc_result(state);
  }
  const auto index = state.next_command;
  const auto& step = std::span(state.steps).subspan(index, 1).front();
  std::optional<api::Command> request;
  PublicCommandExecution execution;
  if (state.image_paste != nullptr) {
    request = concrete_proc_command(step, state.outputs);
    if (!request) {
      execution.error_reason = "unresolved_reference";
    } else {
      auto completed =
          advance_image_paste(*state.image_paste, *request, sessions, runtimes, activity_order);
      if (!completed) {
        return std::nullopt;
      }
      execution = std::move(*completed);
    }
    state.image_paste.reset();
  } else if (state.reload != nullptr) {
    if (!state.reload->complete) {
      return std::nullopt;
    }
    request = concrete_proc_command(step, state.outputs);
    execution.status = state.reload->success ? CommandStatus::applied : CommandStatus::failed;
    if (!state.reload->success) {
      execution.error_reason = state.reload->error == ConfigurationReloadError::restart_required
                                   ? "restart_required"
                                   : "invalid_configuration";
      execution.text = state.reload->diagnostic;
      execution.has_text = true;
    }
    state.reload.reset();
  } else if (state.wait.has_value()) {
    if (!public_wait_has_work(*state.wait, sessions, runtimes, reactor_now())) {
      return std::nullopt;
    }
    auto completed = poll_public_wait(*state.wait, sessions, runtimes, scratch_owner);
    if (!completed.has_value()) {
      return std::nullopt;
    }
    request = std::move(state.wait->request);
    state.wait.reset();
    execution = std::move(*completed);
  } else {
    request = concrete_proc_command(step, state.outputs);
    if (request.has_value() && request->kind == api::CommandKind::pane_wait) {
      state.wait = begin_public_wait(std::move(*request), sessions, runtimes);
      return std::nullopt;
    }
    if (request.has_value()) {
      if (request->kind == api::CommandKind::pane_paste_image) {
        state.image_paste = begin_image_paste(*request, sessions, execution);
        if (state.image_paste != nullptr) {
          return std::nullopt;
        }
      } else if (request->kind == api::CommandKind::config_reload) {
        state.reload = start_reload();
        if (state.reload != nullptr) {
          return std::nullopt;
        }
        execution.status = CommandStatus::conflict;
        execution.error_reason = "reload_busy_or_unavailable";
      } else {
        const auto* const runtime_owner = std::get_if<RuntimeProcOwner>(&state.owner);
        if (surface_command(request->kind)) {
          if (runtime_owner == nullptr) {
            execution.status = CommandStatus::unavailable;
          } else {
            execution =
                execute_surface_command(*request, *runtime_owner, sessions, runtimes, extensions);
          }
        } else {
          auto scratch = std::span<std::byte>{};
          if (request->kind == api::CommandKind::pane_capture) {
            scratch = acquire_public_scratch(scratch_owner);
          }
          if (scratch.empty() && request->kind == api::CommandKind::pane_capture) {
            execution.status = CommandStatus::failed;
          } else {
            execution = PublicCommandExecutor::execute(*request, sessions, runtimes, activity_order,
                                                       scratch);
          }
        }
      }
    } else {
      execution.error_reason = "unresolved_reference";
    }
  }
  if (request.has_value()) {
    handoff_public_focus(*request, execution, sessions, extensions);
  }
  const auto& result_command = request.has_value() ? *request : step.request;
  auto encoded = encode_command_result(result_command, execution, request.has_value());
  if (encoded.empty()) {
    state.ok = false;
    return encode_proc_result(state, "resource_failure", index, request.has_value());
  }
  if (encoded.back() == '\n') {
    encoded.pop_back();
  }
  std::string wrapped = R"({"index":)" + std::to_string(index);
  if (!step.id.empty()) {
    wrapped += R"(,"id":)";
    if (!api::append_json_string(wrapped, step.id)) {
      state.ok = false;
      return encode_proc_result(state, "resource_failure", index, request.has_value());
    }
  }
  wrapped += R"(,"result":)" + encoded + "}";
  constexpr std::size_t proc_result_envelope_reserve = 512;
  static_assert(proc_result_envelope_reserve < api::json_bytes_max);
  const auto separator = state.results.empty() ? 0U : 1U;
  if (state.retained_result_bytes > api::json_bytes_max - proc_result_envelope_reserve ||
      separator >
          api::json_bytes_max - proc_result_envelope_reserve - state.retained_result_bytes ||
      wrapped.size() > api::json_bytes_max - proc_result_envelope_reserve -
                           state.retained_result_bytes - separator) {
    state.ok = false;
    return encode_proc_result(state, "result_too_large", index, request.has_value());
  }
  state.retained_result_bytes += separator + wrapped.size();
  state.results.push_back(std::move(wrapped));
  const bool succeeded =
      execution.status == CommandStatus::applied || execution.status == CommandStatus::no_effect;
  state.ok = state.ok && succeeded;
  state.outputs.push_back(
      {.session = execution.session, .tab = execution.tab, .pane = execution.pane});
  ++state.next_command;
  if ((!succeeded && !state.continue_on_error) || state.next_command == command_count) {
    return encode_proc_result(state);
  }
  // Steps run one per reactor turn, but input queued by this step is written after this service
  // pass. Capture a following command wait's baseline now, before the application can respond.
  const auto& following = std::span(state.steps).subspan(state.next_command, 1).front();
  if (following.request.kind == api::CommandKind::pane_wait &&
      following.request.wait_condition == api::WaitCondition::command &&
      !following.request.after_commands.has_value()) {
    if (auto wait = concrete_proc_command(following, state.outputs); wait.has_value()) {
      state.wait = begin_public_wait(std::move(*wait), sessions, runtimes);
    }
  }
  return std::nullopt;
}

void finish_public_output(PendingConnection& pending, std::string output,
                          const PendingDisposition disposition) noexcept {
  if (output.empty()) {
    pending.state = PendingState::unused;
    return;
  }
  pending.public_output = std::move(output);
  pending.public_output_offset = 0;
  pending.disposition = disposition;
  pending.state = PendingState::flush_response;
  pending.deadline = reactor_now() + setup_progress_timeout;
}

[[nodiscard]] auto owned_public_proc(PendingConnections& connections,
                                     const ProcExecutionState& execution) noexcept
    -> PendingConnection* {
  const auto* const owner = std::get_if<ConnectionProcOwner>(&execution.owner);
  if (owner == nullptr || owner->slot >= connections.size()) {
    return nullptr;
  }
  auto* const pending = std::span(connections).subspan(owner->slot, 1).front().get();
  if (pending == nullptr || pending->generation != owner->generation ||
      pending->state != PendingState::execute_public_proc || pending->proc != execution.id) {
    return nullptr;
  }
  return pending;
}

[[nodiscard]] auto public_proc_failure(ProcExecutionState& execution) -> std::string {
  execution.ok = false;
  return encode_proc_result(execution, "resource_failure", execution.next_command, false);
}

// The reactor owns admitted Procs independently of transports. The connection slot and generation
// are only a completion owner: closing or reusing that owner cancels the Proc before another step.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
void service_public_procs(PublicProcExecutions& executions, PendingConnections& connections,
                          Sessions& sessions, PaneRuntimeStore& runtimes,
                          std::uint64_t& activity_order, PublicScratch& scratch,
                          std::size_t& cursor, extension::CommandRuntime& hosted_extensions,
                          extension::Runtime& extension_runtime) noexcept {
  for (std::size_t visited = 0; visited < executions.size(); ++visited) {
    const auto slot = (cursor + visited) % executions.size();
    auto& proc_slot = std::span(executions).subspan(slot, 1).front();
    if (proc_slot.execution == nullptr) {
      continue;
    }
    auto* const pending = owned_public_proc(connections, *proc_slot.execution);
    const auto* const hosted = std::get_if<HostedProcOwner>(&proc_slot.execution->owner);
    const auto* const runtime = std::get_if<RuntimeProcOwner>(&proc_slot.execution->owner);
    const auto invocation = hosted == nullptr ? 0U : hosted->invocation;
    const bool hosted_connected =
        hosted != nullptr && hosted_extensions.find(invocation) != nullptr;
    const bool runtime_connected =
        runtime != nullptr && extension_runtime.connected(runtime->generation);
    if (pending == nullptr && !hosted_connected && !runtime_connected) {
      proc_slot.execution.reset();
      continue;
    }
    const auto complete = [&](std::string output) {
      if (pending != nullptr) {
        pending->proc = {};
        finish_public_output(*pending, std::move(output), PendingDisposition::keep_proc);
      } else if (hosted != nullptr) {
        static_cast<void>(hosted_extensions.result(invocation, output));
      } else if (runtime != nullptr) {
        extension_runtime.complete_proc(runtime->generation, runtime->request_id, output);
      }
      proc_slot.execution.reset();
    };
    try {
      auto output = execute_public_proc_step(*proc_slot.execution, sessions, runtimes,
                                             activity_order, scratch, extension_runtime);
      if (output.has_value()) {
        complete(std::move(*output));
      }
    } catch (...) {
      try {
        complete(public_proc_failure(*proc_slot.execution));
      } catch (...) {
        proc_slot.execution.reset();
        if (pending != nullptr) {
          pending->proc = {};
          pending->state = PendingState::unused;
        } else if (hosted != nullptr) {
          hosted_extensions.channel().disconnect();
        } else if (runtime != nullptr) {
          static_cast<void>(extension_runtime.disconnect(runtime->generation));
        }
      }
    }
    cursor = (slot + 1U) % executions.size();
    return;
  }
}

[[nodiscard]] auto admit_owned_proc(PublicProcExecutions& executions, ProcExecutionState execution)
    -> std::optional<PublicProcId> {
  for (std::size_t slot = 0; slot < executions.size(); ++slot) {
    auto& destination = std::span(executions).subspan(slot, 1).front();
    if (destination.execution != nullptr) {
      continue;
    }
    destination.generation = next_generation(destination.generation);
    execution.id = {.slot = static_cast<std::uint32_t>(slot), .generation = destination.generation};
    destination.execution = std::make_unique<ProcExecutionState>(std::move(execution));
    return destination.execution->id;
  }
  return std::nullopt;
}

[[nodiscard]] auto admit_public_proc(PendingConnection& pending, PublicProcExecutions& executions,
                                     ProcExecutionState execution) -> bool {
  execution.owner = ConnectionProcOwner{.slot = pending.slot, .generation = pending.generation};
  const auto admitted = admit_owned_proc(executions, std::move(execution));
  if (!admitted.has_value()) {
    return false;
  }
  pending.proc = *admitted;
  std::string{}.swap(pending.public_input);
  pending.state = PendingState::execute_public_proc;
  return true;
}

[[nodiscard]] auto invocation_attachment(Sessions& sessions,
                                         const extension::Invocation& invocation) noexcept
    -> SessionRecord* {
  auto* const session = sessions.get(invocation.context.session);
  return session != nullptr && session->active && session->attachment_runtime.client >= 0 &&
                 session->attachment_runtime.connection_id == invocation.context.connection
             ? session
             : nullptr;
}

void fail_hosted_commands(extension::CommandRuntime& extensions, Sessions& sessions,
                          const std::string_view message) noexcept {
  for (const auto& invocation : extensions.invocations()) {
    if (invocation.id != 0) {
      if (auto* const session = invocation_attachment(sessions, invocation); session != nullptr) {
        publish_status_message(*session, StatusMessageKind::error, message);
      }
    }
  }
  extensions.fail();
}

// Invocations are attachment-generation owned. Validate that owner after input/detach processing
// and before admitting or executing any extension Proc. A completed effect is never rolled back.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void service_hosted_commands(extension::CommandRuntime& extensions, Sessions& sessions,
                             PublicProcExecutions& executions) noexcept {
  if (extensions.channel().descriptor() < 0) {
    fail_hosted_commands(extensions, sessions, "Error: Extension host unavailable");
    return;
  }
  for (const auto& invocation : extensions.invocations()) {
    if (invocation.id == 0) {
      continue;
    }
    if (reactor_now() >= invocation.deadline) {
      fail_hosted_commands(extensions, sessions, "Error: Extension command timed out");
      return;
    }
    if (invocation.phase != extension::InvocationPhase::cancelling &&
        invocation_attachment(sessions, invocation) == nullptr) {
      extensions.cancel(invocation.id);
    }
  }
  auto message = extensions.channel().receive();
  if (!message.has_value()) {
    if (extensions.channel().descriptor() < 0) {
      fail_hosted_commands(extensions, sessions, "Error: Invalid extension response");
    }
    return;
  }
  auto* const invocation = extensions.find(message->invocation);
  if (invocation == nullptr) {
    if (message->kind == "complete") {
      extensions.finish(message->invocation);
    }
    return; // A cancelled generation cannot submit more work, including already queued requests.
  }
  try {
    if (message->kind == "proc" && invocation->phase == extension::InvocationPhase::running) {
      auto prepared = prepare_public_proc(std::move(message->payload));
      if (!prepared.has_value()) {
        static_cast<void>(extensions.result(
            invocation->id, proc_error(prepared.error().reason, prepared.error().index)));
        return;
      }
      prepared->owner = HostedProcOwner{.invocation = invocation->id};
      if (!admit_owned_proc(executions, std::move(*prepared)).has_value()) {
        static_cast<void>(extensions.result(invocation->id, proc_error("capacity")));
        return;
      }
      invocation->phase = extension::InvocationPhase::waiting_proc;
    } else if (message->kind == "complete" &&
               invocation->phase == extension::InvocationPhase::running) {
      const auto ok = api::json_boolean(message->payload, "ok");
      if (!ok.has_value()) {
        fail_hosted_commands(extensions, sessions, "Error: Invalid extension response");
        return;
      }
      if (!*ok) {
        auto diagnostic = std::string("Error: ");
        const auto error =
            api::json_string(message->payload, "error").value_or("Extension command failed");
        for (const char character : error.substr(0, 180)) {
          diagnostic += character >= 32 && character < 127 ? character : '?';
        }
        if (auto* const session = invocation_attachment(sessions, *invocation);
            session != nullptr) {
          publish_status_message(*session, StatusMessageKind::error, diagnostic);
        }
      }
      extensions.finish(invocation->id);
    } else {
      fail_hosted_commands(extensions, sessions, "Error: Invalid extension response");
    }
  } catch (...) {
    fail_hosted_commands(extensions, sessions, "Error: Extension resource failure");
  }
}

// Dispatch is exhaustive across the bounded extension record kinds and their independent lanes.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto process_extension_record(extension::Runtime& extensions,
                                            PublicProcExecutions& executions, Sessions& sessions,
                                            const std::size_t slot,
                                            std::size_t& work_bytes_remaining) noexcept -> bool {
  const auto owner = extensions.owner_at(slot);
  const auto work_bytes = extensions.buffered_record_bytes(slot);
  if (!owner.is_valid() || !work_bytes.has_value() || *work_bytes > work_bytes_remaining) {
    return false;
  }
  work_bytes_remaining -= *work_bytes;
  const auto record = extensions.receive(slot);
  if (!record.has_value()) {
    return false;
  }
  if (record->kind == extension::RecordKind::proc) {
    if (!extensions.reserve_proc(owner, record->sequence)) {
      static_cast<void>(extensions.send_error(owner, record->sequence, "proc_unavailable"));
      extensions.consume(slot);
      return true;
    }
    try {
      // JSON bytes and character storage have the same object representation.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      const std::string_view json(reinterpret_cast<const char*>(record->payload.data()),
                                  record->payload.size());
      auto parsed = api::parse_json(json);
      auto prepared = parsed.value.has_value()
                          ? prepare_public_proc(std::move(*parsed.value))
                          : std::expected<ProcExecutionState, ProcPrepareError>{std::unexpected(
                                ProcPrepareError{.reason = "invalid_json", .index = std::nullopt})};
      extensions.consume(slot);
      if (!prepared.has_value()) {
        extensions.complete_proc(owner, record->sequence,
                                 proc_error(prepared.error().reason, prepared.error().index));
        return true;
      }
      prepared->owner = RuntimeProcOwner{.generation = owner, .request_id = record->sequence};
      if (!admit_owned_proc(executions, std::move(*prepared)).has_value()) {
        extensions.complete_proc(owner, record->sequence, proc_error("capacity"));
      }
    } catch (...) {
      extensions.consume(slot);
      extensions.complete_proc(
          owner, record->sequence,
          R"({"schema":"lemma.proc-result/v1","ok":false,"error":{"reason":"resource_failure"},"results":[]})");
    }
    return true;
  }
  if (record->kind == extension::RecordKind::surface_update) {
    const auto updated = extensions.apply_surface_update(owner, record->payload);
    extensions.consume(slot);
    if (updated.status == extension::SurfaceOperationStatus::applied) {
      if (auto* const session = sessions.get(extensions.session(owner));
          session != nullptr && session->active) {
        schedule_frame(*session, FrameUrgency::burst, false);
      }
    } else if (updated.status != extension::SurfaceOperationStatus::no_effect) {
      static_cast<void>(extensions.send_error(
          owner, record->sequence, extension::surface_operation_status_name(updated.status)));
    }
    return true;
  }
  static_cast<void>(extensions.disconnect(owner));
  return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void prepare_public_document(PendingConnection& pending,
                             PublicProcExecutions& executions) noexcept {
  try {
    auto parsed = api::parse_json(pending.public_input);
    if (!parsed.value.has_value()) {
      finish_public_output(
          pending,
          encode_public_error({.reason = "invalid_json", .field = {}}, parsed.error_offset),
          PendingDisposition::close);
      return;
    }
    const auto schema = api::json_string(*parsed.value, "schema");
    if (schema == std::optional{api::proc_schema}) {
      auto prepared = prepare_public_proc(std::move(*parsed.value));
      if (!prepared.has_value()) {
        finish_public_output(pending, proc_error(prepared.error().reason, prepared.error().index),
                             PendingDisposition::keep_proc);
        return;
      }
      if (!admit_public_proc(pending, executions, std::move(prepared.value()))) {
        finish_public_output(pending, proc_error("capacity"), PendingDisposition::keep_proc);
      }
      return;
    }
    if (schema == std::optional<std::string_view>{api::events_schema}) {
      const auto decoded = api::decode_event_subscription(*parsed.value);
      if (!decoded.subscription.has_value()) {
        finish_public_output(pending, encode_public_error(decoded.error),
                             PendingDisposition::close);
        return;
      }
      pending.subscription = *decoded.subscription;
      pending.state = PendingState::prepare_public_observer;
      return;
    }
    finish_public_output(pending,
                         encode_public_error({.reason = "invalid_schema", .field = "schema"}),
                         PendingDisposition::close);
  } catch (...) {
    finish_public_output(pending, encode_public_error({.reason = "resource_failure", .field = {}}),
                         PendingDisposition::close);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
class CapacityRejectionOutput final {
public:
  [[nodiscard]] auto append(const std::span<const std::byte> bytes) noexcept -> bool {
    if (bytes.size() > storage_.size() - size_) {
      return false;
    }
    std::ranges::copy(bytes, std::span(storage_).subspan(size_).begin());
    size_ += bytes.size();
    return true;
  }
  [[nodiscard]] auto readable() const noexcept -> std::span<const std::byte> {
    return std::span(storage_).first(size_).subspan(offset_);
  }
  [[nodiscard]] auto busy() const noexcept -> bool { return offset_ < size_; }
  [[nodiscard]] auto consume(const std::size_t bytes) noexcept -> bool {
    if (bytes > size_ - offset_) {
      return false;
    }
    offset_ += bytes;
    return true;
  }
  void reset() noexcept {
    size_ = 0;
    offset_ = 0;
  }

private:
  std::array<std::byte, protocol::attach_header_bytes + 1U + protocol::diagnostic_bytes_max>
      storage_{};
  std::size_t size_{0};
  std::size_t offset_{0};
};

struct CapacityRejectionConnection final {
  [[nodiscard]] auto active() const noexcept -> bool { return descriptor >= 0; }

  int descriptor{-1};
  CapacityRejectionOutput output;
  std::chrono::steady_clock::time_point deadline;
  bool flush_response{false};
};

constexpr std::size_t capacity_rejection_connections_max = 8;
using CapacityRejectionConnections =
    std::array<CapacityRejectionConnection, capacity_rejection_connections_max>;
static_assert(sizeof(CapacityRejectionConnections) <= std::size_t{4} * 1'024U);

void record_pending_progress(PendingConnection& pending) noexcept {
  const auto progress = reactor_now() + setup_progress_timeout;
  pending.deadline =
      pending.public_connection ? progress : std::min(progress, pending.setup_deadline);
}

void begin_pending_field(PendingConnection& pending, const PendingState state,
                         const std::size_t size) noexcept {
  LEMMA_ASSERT(size > 0 && size <= pending.field.size());
  pending.state = state;
  pending.field_size = 0;
  pending.field_target = size;
}

void release_attach_reservation(PendingConnection& pending, const std::size_t slot,
                                Sessions& sessions) noexcept {
  auto* const session = sessions.get(pending.attach_session);
  if (session != nullptr && session->attachment_runtime.pending_attach_slot == slot &&
      session->attachment_runtime.pending_attach_generation == pending.generation) {
    if (session->attachment_runtime.client < 0) {
      session->attachment_runtime.reset_connection();
    } else {
      session->attachment_runtime.pending_attach_slot = std::numeric_limits<std::uint32_t>::max();
      session->attachment_runtime.pending_attach_generation = 0;
    }
  }
  pending.attach_session = {};
}

void close_pending(PendingConnections& connections, const std::size_t slot,
                   Sessions& sessions) noexcept {
  auto& owner = std::span(connections).subspan(slot, 1).front();
  if (owner == nullptr) {
    return;
  }
  release_attach_reservation(*owner, slot, sessions);
  if (owner->extension_peer != nullptr) {
    owner->descriptor = -1;
    owner->extension_peer.reset();
  } else {
    close_descriptor(owner->descriptor);
  }
  owner.reset();
}

void close_capacity_rejection(CapacityRejectionConnections& connections,
                              const std::size_t slot) noexcept {
  auto& connection = std::span(connections).subspan(slot, 1).front();
  close_descriptor(connection.descriptor);
  connection.output.reset();
  connection.flush_response = false;
}

void finish_pending_output(PendingConnection& pending, const PendingDisposition disposition =
                                                           PendingDisposition::close) noexcept {
  pending.disposition = disposition;
  pending.state = PendingState::flush_response;
  pending.field_size = 0;
  pending.field_target = 0;
}

void finish_pending_byte(
    PendingConnection& pending, const std::byte response,
    const PendingDisposition disposition = PendingDisposition::close) noexcept {
  pending.output.reset();
  const bool appended = pending.output.append(std::span(&response, 1));
  LEMMA_ASSERT(appended);
  finish_pending_output(pending, disposition);
}

void finish_pending_create(PendingConnection& pending, const SessionRecord& session) noexcept {
  const auto* const tab = active_tab(session);
  LEMMA_ASSERT(tab != nullptr);
  const auto tab_id = protocol::encode_control_id(tab->id);
  const auto pane_id = protocol::encode_control_id(tab->focused_pane);
  pending.output.reset();
  const auto name = session.session_name();
  const auto name_size = static_cast<std::byte>(name.size());
  const bool appended = pending.output.append(std::span(&response_ready, 1)) &&
                        pending.output.append(std::span(&name_size, 1)) &&
                        pending.output.append(std::as_bytes(std::span(name.data(), name.size()))) &&
                        pending.output.append(tab_id) && pending.output.append(pane_id);
  LEMMA_ASSERT(appended);
  finish_pending_output(pending);
}

void finish_pending_surface(PendingConnection& pending, const TabId tab,
                            const PaneId pane) noexcept {
  const auto tab_id = protocol::encode_control_id(tab);
  const auto pane_id = protocol::encode_control_id(pane);
  pending.output.reset();
  const bool appended = pending.output.append(std::span(&response_ready, 1)) &&
                        pending.output.append(tab_id) && pending.output.append(pane_id);
  LEMMA_ASSERT(appended);
  finish_pending_output(pending);
}

void fail_pending_output(PendingConnection& pending) noexcept {
  finish_pending_byte(pending, response_failed);
}

void finish_pending_disconnect(PendingConnection& pending, const protocol::DisconnectReason reason,
                               const std::string_view diagnostic) noexcept {
  pending.output.reset();
  const auto encoded = protocol::encode_disconnect(reason, diagnostic);
  const bool appended = pending.output.append(encoded.bytes());
  LEMMA_ASSERT(appended);
  finish_pending_output(pending);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void prepare_unnamed_command(PendingConnection& pending, Sessions& sessions,
                             PaneRuntimeStore& runtimes) noexcept {
  bool prepared = true;
  if (pending.command == command_list) {
    prepared = append_all_listings(pending.output, sessions, runtimes);
  } else if (pending.command == command_query_sessions) {
    prepared = append_structured_sessions(pending.output, sessions);
  } else if (pending.command == command_kill_all || pending.command == command_shutdown) {
    const Command stop{.kind = CommandKind::stop_session, .origin = CommandOrigin::cli};
    for (auto& session : sessions) {
      if (session != nullptr) {
        static_cast<void>(dispatch_session_command(*session, runtimes, stop));
      }
    }
    prepared = pending.output.append_text(pending.command == command_shutdown
                                              ? protocol::shutdown_response
                                              : "all lemma sessions stopped\n");
  } else {
    pending.state = PendingState::unused;
    return;
  }
  if (!prepared) {
    fail_pending_output(pending);
  } else {
    finish_pending_output(pending, pending.command == command_shutdown
                                       ? PendingDisposition::shutdown
                                       : PendingDisposition::close);
  }
}

void prepare_rename_command(PendingConnection& pending, Sessions& sessions,
                            PaneRuntimeStore& runtimes) noexcept {
  auto* const session = find_session(sessions, pending.session.view());
  if (session == nullptr) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  Command command{
      .origin = CommandOrigin::cli,
      .target = {.session = session->id, .tab = {}, .pane = {}, .peer_pane = {}, .attachment = {}}};
  if (pending.command == command_rename_session) {
    const auto name =
        SessionNameValue::create({pending.mutation_text.data(), pending.mutation_text_size});
    if (!name.has_value()) {
      finish_pending_byte(pending, response_failed);
      return;
    }
    command.kind = CommandKind::rename_session;
    command.payload = *name;
  } else {
    const auto* const tab = tab_at_position(*session, pending.mutation_position);
    const auto title =
        TabTitleValue::create({pending.mutation_text.data(), pending.mutation_text_size});
    if (tab == nullptr) {
      finish_pending_byte(pending, response_missing);
      return;
    }
    if (!title.has_value()) {
      finish_pending_byte(pending, response_failed);
      return;
    }
    command.kind = CommandKind::rename_tab;
    command.target.tab = tab->id;
    command.payload = *title;
  }
  const auto result =
      dispatch_session_command(*session, runtimes, command, &session_name_conflict, &sessions);
  switch (result.status) {
  case CommandStatus::applied:
  case CommandStatus::no_effect:
    finish_pending_byte(pending, response_ready);
    return;
  case CommandStatus::conflict:
    finish_pending_byte(pending, response_conflict);
    return;
  case CommandStatus::stale_target:
  case CommandStatus::wrong_owner:
    finish_pending_byte(pending, response_missing);
    return;
  case CommandStatus::capacity:
    finish_pending_byte(pending, response_capacity);
    return;
  case CommandStatus::detach_requested:
  case CommandStatus::invalid_command:
  case CommandStatus::invalid_target:
  case CommandStatus::unavailable:
  case CommandStatus::failed:
    finish_pending_byte(pending, response_failed);
    return;
  }
}

[[nodiscard]] constexpr auto decode_control_u16(const std::span<const std::byte, 2> bytes) noexcept
    -> std::uint16_t {
  return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes.front()) << 8U) |
                                    std::to_integer<std::uint16_t>(bytes.back()));
}

template <typename Id>
[[nodiscard]] auto decode_control_target(const std::span<const std::byte> payload,
                                         const std::size_t offset) noexcept -> std::optional<Id> {
  if (offset > payload.size() || protocol::control_id_bytes > payload.size() - offset) {
    return std::nullopt;
  }
  return protocol::decode_control_id<Id>(payload.subspan(offset, protocol::control_id_bytes)
                                             .template first<protocol::control_id_bytes>());
}

void finish_command_result(PendingConnection& pending, const CommandResult result) noexcept {
  switch (result.status) {
  case CommandStatus::applied:
    finish_pending_byte(pending, response_ready);
    return;
  case CommandStatus::no_effect:
    finish_pending_byte(pending, response_no_effect);
    return;
  case CommandStatus::stale_target:
  case CommandStatus::wrong_owner:
    finish_pending_byte(pending, response_missing);
    return;
  case CommandStatus::conflict:
    finish_pending_byte(pending, response_conflict);
    return;
  case CommandStatus::capacity:
    finish_pending_byte(pending, response_capacity);
    return;
  case CommandStatus::unavailable:
    finish_pending_byte(pending, response_unavailable);
    return;
  case CommandStatus::detach_requested:
  case CommandStatus::invalid_command:
  case CommandStatus::invalid_target:
  case CommandStatus::failed:
    finish_pending_byte(pending, response_failed);
    return;
  }
}

[[nodiscard]] auto resolve_control_tab(SessionRecord& session, const TabId requested,
                                       const std::uint16_t position) noexcept -> Tab* {
  if (requested.is_valid()) {
    return find_tab(session, requested);
  }
  return position == 0 ? nullptr : tab_at_position(session, position - 1U);
}

[[nodiscard]] auto tab_move_anchor(const SessionRecord& session, const TabId moving,
                                   const std::size_t one_based_position) noexcept
    -> std::optional<std::optional<TabId>> {
  if (one_based_position == 0 || one_based_position > session.tab_order.size()) {
    return std::nullopt;
  }
  std::array<TabId, tabs_per_session_max> remaining{};
  std::size_t count = 0;
  for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
    const auto id = session.tab_order.at(position);
    LEMMA_ASSERT(id.has_value());
    if (*id != moving) {
      std::span(remaining).subspan(count, 1).front() = *id;
      ++count;
    }
  }
  if (count + 1U != session.tab_order.size()) {
    return std::nullopt;
  }
  const auto destination = one_based_position - 1U;
  return destination >= count
             ? std::optional<TabId>{}
             : std::optional<TabId>{std::span(remaining).subspan(destination, 1).front()};
}

// Control payload parsing is bounded by one retained setup field and dispatches the same typed
// commands used by keyboard and mouse input.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void prepare_semantic_action(PendingConnection& pending, Sessions& sessions,
                             PaneRuntimeStore& runtimes,
                             const std::span<const std::byte> payload) noexcept {
  if (payload.size() != protocol::semantic_action_payload_bytes) {
    fail_pending_output(pending);
    return;
  }
  auto* const session = find_session(sessions, pending.session.view());
  const auto tab_id = decode_control_target<TabId>(payload, 1);
  const auto pane_id = decode_control_target<PaneId>(payload, 1U + protocol::control_id_bytes);
  const auto peer_id =
      decode_control_target<PaneId>(payload, 1U + (2U * protocol::control_id_bytes));
  if (session == nullptr || !tab_id.has_value() || !pane_id.has_value() || !peer_id.has_value()) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  const auto source_position = decode_control_u16(
      payload.subspan(1U + (3U * protocol::control_id_bytes), 2).template first<2>());
  const auto value = decode_control_u16(
      payload.subspan(3U + (3U * protocol::control_id_bytes), 2).template first<2>());
  const auto action =
      static_cast<protocol::ControlAction>(std::to_integer<std::uint8_t>(payload.front()));
  Command command{.origin = CommandOrigin::cli};
  if (action == protocol::ControlAction::session_kill) {
    command.kind = CommandKind::stop_session;
    finish_command_result(pending, dispatch_session_command(*session, runtimes, command));
    return;
  }
  switch (action) {
  case protocol::ControlAction::tab_select:
  case protocol::ControlAction::tab_move:
  case protocol::ControlAction::tab_kill: {
    auto* const tab = resolve_control_tab(*session, *tab_id, source_position);
    if (tab == nullptr) {
      finish_pending_byte(pending, response_missing);
      return;
    }
    command.target.tab = tab->id;
    if (action == protocol::ControlAction::tab_select) {
      command.kind = CommandKind::select_tab;
      command.payload = CommandCoordinate{
          .value = static_cast<std::uint16_t>(source_position == 0 ? 0 : source_position - 1U)};
    } else if (action == protocol::ControlAction::tab_kill) {
      command.kind = CommandKind::close_tab;
    } else {
      const auto anchor = tab_move_anchor(*session, tab->id, value);
      if (!anchor.has_value()) {
        finish_pending_byte(pending, response_failed);
        return;
      }
      command.kind = CommandKind::place_tab;
      command.payload = TabPlacementCommand{.before = anchor->value_or(TabId{})};
    }
    finish_command_result(pending, dispatch_session_command(*session, runtimes, command));
    return;
  }
  case protocol::ControlAction::pane_focus:
  case protocol::ControlAction::pane_swap:
  case protocol::ControlAction::pane_resize_left:
  case protocol::ControlAction::pane_resize_right:
  case protocol::ControlAction::pane_resize_up:
  case protocol::ControlAction::pane_resize_down:
  case protocol::ControlAction::pane_zoom_on:
  case protocol::ControlAction::pane_zoom_off:
  case protocol::ControlAction::pane_kill:
    break;
  case protocol::ControlAction::session_kill:
    LEMMA_ASSERT(false);
    return;
  }
  auto* const pane = find_pane(*session, *pane_id);
  auto* const tab = pane == nullptr ? nullptr : find_tab(*session, pane->tab);
  if (pane == nullptr || tab == nullptr) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  command.target.tab = tab->id;
  command.target.pane = pane->id;
  if (action == protocol::ControlAction::pane_focus) {
    command.kind = CommandKind::focus_pane;
  } else if (action == protocol::ControlAction::pane_swap) {
    command.kind = CommandKind::swap_panes;
    command.payload = PaneSwapCommand{.other = *peer_id};
  } else if (action == protocol::ControlAction::pane_kill) {
    command.kind = CommandKind::close_pane;
  } else if (action == protocol::ControlAction::pane_zoom_on ||
             action == protocol::ControlAction::pane_zoom_off) {
    command.kind = CommandKind::set_zoom;
    command.payload = PaneZoomCommand{.enabled = action == protocol::ControlAction::pane_zoom_on};
  } else {
    if (value == 0 || value > command_resize_amount_max) {
      finish_pending_byte(pending, response_failed);
      return;
    }
    switch (action) {
    case protocol::ControlAction::pane_resize_left:
      command.kind = CommandKind::resize_left;
      break;
    case protocol::ControlAction::pane_resize_right:
      command.kind = CommandKind::resize_right;
      break;
    case protocol::ControlAction::pane_resize_up:
      command.kind = CommandKind::resize_up;
      break;
    case protocol::ControlAction::pane_resize_down:
      command.kind = CommandKind::resize_down;
      break;
    case protocol::ControlAction::tab_select:
    case protocol::ControlAction::tab_move:
    case protocol::ControlAction::tab_kill:
    case protocol::ControlAction::pane_focus:
    case protocol::ControlAction::pane_swap:
    case protocol::ControlAction::pane_zoom_on:
    case protocol::ControlAction::pane_zoom_off:
    case protocol::ControlAction::pane_kill:
    case protocol::ControlAction::session_kill:
      finish_pending_byte(pending, response_failed);
      return;
    }
    command.payload = CommandCoordinate{.value = value};
    finish_command_result(pending, dispatch_session_command(*session, runtimes, command));
    return;
  }
  finish_command_result(pending, dispatch_session_command(*session, runtimes, command));
}

// Surface creation validates one bounded launch transaction before publishing topology.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void prepare_surface_create(PendingConnection& pending, Sessions& sessions,
                            PaneRuntimeStore& runtimes,
                            const std::span<const std::byte> payload) noexcept {
  constexpr std::size_t header_size = 15;
  if (payload.size() < header_size) {
    fail_pending_output(pending);
    return;
  }
  const auto kind =
      static_cast<protocol::SurfaceCreateKind>(std::to_integer<std::uint8_t>(payload.front()));
  const auto target = decode_control_target<PaneId>(payload, 1);
  const auto flags = std::to_integer<std::uint8_t>(payload.subspan(9, 1).front());
  const auto title_size = std::to_integer<std::size_t>(payload.subspan(10, 1).front());
  const auto cwd_size = decode_control_u16(payload.subspan(11, 2).template first<2>());
  const auto command_size = decode_control_u16(payload.subspan(13, 2).template first<2>());
  const auto expected = header_size + title_size + cwd_size + command_size;
  if (!target.has_value() || flags > 1U || expected != payload.size() ||
      title_size > protocol::tab_title_bytes_max ||
      cwd_size > protocol::working_directory_bytes_max ||
      command_size > protocol::command_bytes_max) {
    fail_pending_output(pending);
    return;
  }
  const auto title_bytes = payload.subspan(header_size, title_size);
  const auto cwd_bytes = payload.subspan(header_size + title_size, cwd_size);
  const auto command = payload.subspan(header_size + title_size + cwd_size, command_size);
  // Byte payloads and character storage have the same object representation.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view title(reinterpret_cast<const char*>(title_bytes.data()),
                               title_bytes.size());
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view cwd(reinterpret_cast<const char*>(cwd_bytes.data()), cwd_bytes.size());
  const auto valid_cwd = cwd.empty() || (cwd.front() == '/' && !cwd.contains('\0'));
  const auto title_value = TabTitleValue::create(title);
  if (!valid_cwd || !title_value.has_value() || !valid_launch_command(command)) {
    fail_pending_output(pending);
    return;
  }
  auto* const session = find_session(sessions, pending.session.view());
  if (session == nullptr) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  const auto exit_policy = flags == 0 ? PaneExitPolicy::close : PaneExitPolicy::hold;
  if (kind == protocol::SurfaceCreateKind::tab) {
    if (target->is_valid()) {
      fail_pending_output(pending);
      return;
    }
    if (tab_count(*session) >= session->tabs.size() ||
        pane_count(*session) >= panes_per_session_max ||
        runtimes.size() >= limits::panes_hard_max) {
      finish_pending_byte(pending, response_capacity);
      return;
    }
    auto* const tab = create_tab(*session, runtimes, command, cwd, exit_policy);
    if (tab == nullptr) {
      finish_pending_byte(pending, response_failed);
      return;
    }
    const bool titled = tab->set_title_override(title);
    LEMMA_ASSERT(titled);
    finish_pending_surface(pending, tab->id, tab->focused_pane);
    return;
  }
  if (kind != protocol::SurfaceCreateKind::split_right &&
      kind != protocol::SurfaceCreateKind::split_down) {
    fail_pending_output(pending);
    return;
  }
  auto* const source = find_pane(*session, *target);
  auto* const tab = source == nullptr ? nullptr : find_tab(*session, source->tab);
  if (source == nullptr || tab == nullptr || !title.empty()) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  const auto axis = kind == protocol::SurfaceCreateKind::split_right ? SplitAxis::left_right
                                                                     : SplitAxis::top_bottom;
  if (pane_count(*session) >= panes_per_session_max || runtimes.size() >= limits::panes_hard_max) {
    finish_pending_byte(pending, response_capacity);
    return;
  }
  auto* const created =
      split_pane(*session, *tab, runtimes, source->id, axis, command, cwd, exit_policy);
  if (created == nullptr) {
    finish_pending_byte(pending, response_failed);
    return;
  }
  finish_pending_surface(pending, tab->id, created->id);
}

void prepare_send_pane(PendingConnection& pending, Sessions& sessions, PaneRuntimeStore& runtimes,
                       const std::span<const std::byte> payload) noexcept {
  if (payload.size() <= protocol::control_id_bytes) {
    fail_pending_output(pending);
    return;
  }
  const auto id = decode_control_target<PaneId>(payload, 0);
  auto* const session = find_session(sessions, pending.session.view());
  auto* const pane = session == nullptr || !id.has_value() ? nullptr : find_pane(*session, *id);
  auto* const runtime = pane == nullptr ? nullptr : find_pane_runtime(runtimes, *session, *pane);
  if (runtime == nullptr) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  const auto result = queue_application_bytes_to(*session, runtimes, *runtime,
                                                 payload.subspan(protocol::control_id_bytes));
  finish_command_result(pending, {.status = result == InputQueueResult::queued
                                                ? CommandStatus::applied
                                                : CommandStatus::unavailable});
}

void prepare_capture_pane(PendingConnection& pending, Sessions& sessions,
                          PaneRuntimeStore& runtimes,
                          const std::span<const std::byte> payload) noexcept {
  if (payload.size() != protocol::control_id_bytes) {
    fail_pending_output(pending);
    return;
  }
  const auto id = decode_control_target<PaneId>(payload, 0);
  auto* const session = find_session(sessions, pending.session.view());
  auto* const pane = session == nullptr || !id.has_value() ? nullptr : find_pane(*session, *id);
  auto* const runtime = pane == nullptr ? nullptr : find_pane_runtime(runtimes, *session, *pane);
  if (runtime == nullptr || !runtime->live()) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  constexpr std::size_t header_size = 3;
  const auto formatted = runtime->terminal.format_screen(
      vt::ScreenFormat::plain,
      std::span(pending.field).first(limits::pending_connection_output_bytes_max - header_size));
  if (!formatted.has_value() || *formatted > std::numeric_limits<std::uint16_t>::max()) {
    fail_pending_output(pending);
    return;
  }
  const auto size = protocol::encode_bounded_size(*formatted);
  pending.output.reset();
  const bool appended = pending.output.append(std::span(&response_ready, 1)) &&
                        pending.output.append(size) &&
                        pending.output.append(std::span(pending.field).first(*formatted));
  if (!appended) {
    fail_pending_output(pending);
  } else {
    finish_pending_output(pending);
  }
}

void prepare_pane_status(PendingConnection& pending, Sessions& sessions, PaneRuntimeStore& runtimes,
                         const std::span<const std::byte> payload) noexcept {
  if (payload.size() != protocol::control_id_bytes) {
    fail_pending_output(pending);
    return;
  }
  const auto id = decode_control_target<PaneId>(payload, 0);
  auto* const session = find_session(sessions, pending.session.view());
  auto* const pane = session == nullptr || !id.has_value() ? nullptr : find_pane(*session, *id);
  auto* const runtime = pane == nullptr ? nullptr : find_pane_runtime(runtimes, *session, *pane);
  if (runtime == nullptr) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  std::array<std::byte, 7> response{};
  response.front() = response_ready;
  std::span(response).subspan(1, 1).front() =
      static_cast<std::byte>(pane->process_exit.has_value() ? 1 : 0);
  const auto exit = pane->process_exit.value_or(ProcessExit{});
  std::span(response).subspan(2, 1).front() = static_cast<std::byte>(exit.kind);
  std::span(response).subspan(3, 1).front() = static_cast<std::byte>((exit.value >> 24U) & 0xffU);
  std::span(response).subspan(4, 1).front() = static_cast<std::byte>((exit.value >> 16U) & 0xffU);
  std::span(response).subspan(5, 1).front() = static_cast<std::byte>((exit.value >> 8U) & 0xffU);
  std::span(response).subspan(6, 1).front() = static_cast<std::byte>(exit.value & 0xffU);
  pending.output.reset();
  const bool appended = pending.output.append(response);
  LEMMA_ASSERT(appended);
  finish_pending_output(pending);
}

void prepare_control_payload(PendingConnection& pending, Sessions& sessions,
                             PaneRuntimeStore& runtimes) noexcept {
  const auto payload = std::span<const std::byte>(pending.field).first(pending.field_target);
  pending.output.reset();
  if (pending.command == command_semantic_action) {
    prepare_semantic_action(pending, sessions, runtimes, payload);
  } else if (pending.command == command_surface_create) {
    prepare_surface_create(pending, sessions, runtimes, payload);
  } else if (pending.command == command_send_pane) {
    prepare_send_pane(pending, sessions, runtimes, payload);
  } else if (pending.command == command_capture_pane) {
    prepare_capture_pane(pending, sessions, runtimes, payload);
  } else if (pending.command == command_pane_status) {
    prepare_pane_status(pending, sessions, runtimes, payload);
  } else {
    fail_pending_output(pending);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void prepare_named_command(PendingConnection& pending, Sessions& sessions,
                           PaneRuntimeStore& runtimes, std::uint64_t& activity_order) noexcept {
  const bool create_command = pending.command == command_create ||
                              pending.command == command_create_with_context ||
                              pending.command == command_create_auto_with_context;
  if (pending.command == command_create_auto_with_context) {
    const auto name = allocate_numeric_session_name(sessions);
    if (!name.has_value()) {
      finish_pending_byte(pending, response_capacity);
      return;
    }
    pending.session = *name;
  }
  SessionRecord* session = find_session(sessions, pending.session.view());
  if (create_command) {
    if (session != nullptr) {
      finish_pending_byte(pending, response_conflict);
      return;
    }
    if (sessions.size() == Sessions::capacity()) {
      finish_pending_byte(pending, response_capacity);
      return;
    }
    const auto environment_mode = pending.command == command_create_with_context ||
                                          pending.command == command_create_auto_with_context
                                      ? LaunchEnvironmentMode::replace
                                      : LaunchEnvironmentMode::inherit;
    const std::span<const std::byte> environment(pending.environment);
    auto created = create_session(pending.session.view(), pending.working_directory.view(),
                                  environment, environment_mode);
    if (created == nullptr) {
      finish_pending_byte(pending, response_failed);
      return;
    }
    const auto id = sessions.insert(std::move(created));
    if (!id.has_value()) {
      finish_pending_byte(pending, response_capacity);
      return;
    }
    auto* const inserted = sessions.get(*id);
    LEMMA_ASSERT(inserted != nullptr);
    inserted->id = *id;
    inserted->attachment.id = AttachmentId::from_parts(id->slot(), id->generation());
    inserted->attachment.session = *id;
    if (activity_order == std::numeric_limits<std::uint64_t>::max()) {
      const bool erased = sessions.erase(*id);
      LEMMA_ASSERT(erased);
      finish_pending_byte(pending, response_capacity);
      return;
    }
    inserted->activity_order = ++activity_order;
    const std::span<const std::byte> launch_command(pending.launch_command);
    if (create_tab(*inserted, runtimes, launch_command, {}, pending.exit_policy) == nullptr) {
      const bool erased = sessions.erase(*id);
      LEMMA_ASSERT(erased);
      finish_pending_byte(pending, response_failed);
      return;
    }
    inserted->previous_tab = inserted->active_tab;
    finish_pending_create(pending, *inserted);
    return;
  }
  if (session == nullptr) {
    finish_pending_byte(pending, response_missing);
    return;
  }
  if (pending.command == command_list_session) {
    if (!append_listing(pending.output, *session, runtimes)) {
      fail_pending_output(pending);
    } else {
      finish_pending_output(pending);
    }
    return;
  }
  if (pending.command == command_query_session || pending.command == command_query_tabs ||
      pending.command == command_query_panes) {
    bool appended = false;
    if (pending.command == command_query_session) {
      appended = pending.output.append_text("[") &&
                 append_structured_session(pending.output, *session) &&
                 pending.output.append_text("]");
    } else if (pending.command == command_query_tabs) {
      appended = append_structured_tabs(pending.output, *session, runtimes);
    } else {
      appended = append_structured_panes(pending.output, *session, runtimes);
    }
    if (!appended) {
      fail_pending_output(pending);
    } else {
      finish_pending_output(pending);
    }
    return;
  }
  if (pending.command == command_list_tabs || pending.command == command_list_panes) {
    const bool appended = pending.command == command_list_tabs
                              ? append_tab_listings(pending.output, *session, runtimes)
                              : append_pane_listings(pending.output, *session, runtimes);
    if (!appended) {
      fail_pending_output(pending);
    } else {
      finish_pending_output(pending);
    }
    return;
  }

  if (!pending.output.append_text("lemma session \"") ||
      !pending.output.append_title(session->session_name()) ||
      !pending.output.append_text("\" stopped\n")) {
    fail_pending_output(pending);
    return;
  }
  const Command stop{.kind = CommandKind::stop_session, .origin = CommandOrigin::cli};
  static_cast<void>(dispatch_session_command(*session, runtimes, stop));
  finish_pending_output(pending);
}

void prepare_attach(PendingConnection& pending, Sessions& sessions, PaneRuntimeStore& runtimes,
                    const std::size_t slot) noexcept {
  SessionRecord* const session = pending.session.size == 0
                                     ? most_recent_detached_session(sessions)
                                     : find_session(sessions, pending.session.view());
  if (session == nullptr) {
    finish_pending_disconnect(pending, protocol::DisconnectReason::session_missing,
                              pending.session.size == 0 ? "no detached lemma session"
                                                        : "no lemma session");
    return;
  }
  if (session->attachment_runtime.client >= 0 || session->attachment_runtime.pending_attach_slot !=
                                                     std::numeric_limits<std::uint32_t>::max()) {
    finish_pending_disconnect(pending, protocol::DisconnectReason::session_busy,
                              "lemma session is already attached");
    return;
  }
  if (!sessions.connection_available(session->id)) {
    finish_pending_disconnect(pending, protocol::DisconnectReason::capacity,
                              "attachment identity capacity exhausted");
    return;
  }
  const protocol::Dimensions previous_dimensions{.columns = session->attachment.columns,
                                                 .rows = session->attachment.rows};
  if (!resize_session(*session, runtimes, pending.attach_dimensions)) {
    session->attachment_runtime.frame.release();
    finish_pending_disconnect(pending, protocol::DisconnectReason::setup_failed,
                              "failed to prepare attached viewport");
    return;
  }
  if (!session->theme_bound && pending.attach_host_theme.has_value() &&
      !bind_session_theme(*session, runtimes, *pending.attach_host_theme)) {
    // Viewport and theme are one attach transaction. A recoverable theme failure restores the
    // previous viewport; inability to restore either terminal state fails the Session closed.
    if (session->active && !resize_session(*session, runtimes, previous_dimensions)) {
      session->active = false;
    }
    session->attachment_runtime.frame.release();
    finish_pending_disconnect(pending, protocol::DisconnectReason::setup_failed,
                              "failed to apply host terminal theme");
    return;
  }
  // Theme and viewport are committed together here. Failure while flushing the accepted hello is
  // then ordinary AttachmentRuntime loss: it releases connection resources but does not rewrite
  // stable Session defaults or the last committed viewport.
  session->attachment_runtime.pending_attach_slot = static_cast<std::uint32_t>(slot);
  session->attachment_runtime.pending_attach_generation = pending.generation;
  pending.attach_session = session->id;
  pending.output.reset();
  const auto hello = protocol::encode_daemon_hello(pending.attach_dimensions);
  const bool appended = pending.output.append(hello.bytes());
  LEMMA_ASSERT(appended);
  finish_pending_output(pending, PendingDisposition::attach);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-exception-escape)
void complete_pending_field(PendingConnection& pending, Sessions& sessions,
                            PaneRuntimeStore& runtimes, std::uint64_t& activity_order) noexcept {
  switch (pending.state) {
  case PendingState::read_command:
    pending.command = pending.field.front();
    if (pending.command == std::byte{'{'}) {
      pending.public_connection = true;
      pending.public_input = "{";
      pending.state = PendingState::read_public_json;
      pending.field_size = 0;
      pending.field_target = 0;
    } else if (pending.command == extension::protocol_magic.front()) {
      try {
        pending.extension_peer = std::make_unique<extension::FramedPeer>(pending.descriptor);
      } catch (...) {
        pending.state = PendingState::unused;
        break;
      }
      if (!pending.extension_peer->prime(pending.command)) {
        pending.state = PendingState::unused;
        break;
      }
      pending.state = PendingState::read_extension;
    } else if (pending.command == protocol::attach_magic.front()) {
      if (!pending.attach_decoder.prepare().has_value()) {
        pending.state = PendingState::unused;
        break;
      }
      pending.attach_decoder.reset();
      pending.attach_decoder.writable_bytes().front() = pending.command;
      const auto committed = pending.attach_decoder.commit(1);
      LEMMA_ASSERT(committed.has_value());
      pending.state = PendingState::read_attach;
    } else if (pending.command == command_list || pending.command == command_query_sessions ||
               pending.command == command_kill_all || pending.command == command_shutdown) {
      pending.output.reset();
      prepare_unnamed_command(pending, sessions, runtimes);
    } else if (pending.command == command_create_auto_with_context) {
      begin_pending_field(pending, PendingState::read_create_flags, 1);
    } else if (pending.command == command_create ||
               pending.command == command_create_with_context ||
               pending.command == command_list_session || pending.command == command_list_tabs ||
               pending.command == command_list_panes || pending.command == command_query_session ||
               pending.command == command_query_tabs || pending.command == command_query_panes ||
               pending.command == command_rename_session || pending.command == command_rename_tab ||
               pending.command == command_kill || pending.command == command_surface_create ||
               pending.command == command_semantic_action || pending.command == command_send_pane ||
               pending.command == command_capture_pane || pending.command == command_pane_status) {
      begin_pending_field(pending, PendingState::read_name_size, 1);
    } else {
      pending.state = PendingState::unused;
    }
    break;
  case PendingState::read_attach:
    LEMMA_ASSERT(false);
    break;
  case PendingState::read_name_size: {
    const auto size = protocol::decode_session_name_size(pending.field.front());
    if (size == 0 || size > pending.session.bytes.size()) {
      pending.state = PendingState::unused;
    } else {
      begin_pending_field(pending, PendingState::read_name, size);
    }
    break;
  }
  case PendingState::read_name:
    pending.session.size = pending.field_target;
    std::ranges::copy(std::span(pending.field).first(pending.session.size),
                      std::as_writable_bytes(std::span(pending.session.bytes)).begin());
    if (!valid_session_name(pending.session.view())) {
      pending.state = PendingState::unused;
    } else if (pending.command == command_create_with_context) {
      begin_pending_field(pending, PendingState::read_create_flags, 1);
    } else if (pending.command == command_rename_session) {
      begin_pending_field(pending, PendingState::read_mutation_size, 1);
    } else if (pending.command == command_rename_tab) {
      begin_pending_field(pending, PendingState::read_mutation_position, 1);
    } else if (pending.command == command_surface_create ||
               pending.command == command_semantic_action || pending.command == command_send_pane ||
               pending.command == command_capture_pane || pending.command == command_pane_status) {
      begin_pending_field(pending, PendingState::read_control_payload_size, 2);
    } else {
      pending.output.reset();
      prepare_named_command(pending, sessions, runtimes, activity_order);
    }
    break;
  case PendingState::read_mutation_position:
    pending.mutation_position = std::to_integer<std::uint8_t>(pending.field.front());
    if (pending.mutation_position >= tabs_per_session_max) {
      pending.state = PendingState::unused;
    } else {
      begin_pending_field(pending, PendingState::read_mutation_size, 1);
    }
    break;
  case PendingState::read_mutation_size: {
    const auto size =
        static_cast<std::size_t>(std::to_integer<std::uint8_t>(pending.field.front()));
    const auto maximum = pending.command == command_rename_session
                             ? protocol::session_name_bytes_max
                             : protocol::tab_title_bytes_max;
    if (size > maximum || (pending.command == command_rename_session && size == 0)) {
      pending.state = PendingState::unused;
    } else if (size == 0) {
      pending.mutation_text = {};
      pending.mutation_text_size = 0;
      pending.output.reset();
      prepare_rename_command(pending, sessions, runtimes);
    } else {
      begin_pending_field(pending, PendingState::read_mutation, size);
    }
    break;
  }
  case PendingState::read_mutation:
    pending.mutation_text = {};
    pending.mutation_text_size = pending.field_target;
    std::ranges::copy(std::span(pending.field).first(pending.mutation_text_size),
                      std::as_writable_bytes(std::span(pending.mutation_text)).begin());
    pending.output.reset();
    prepare_rename_command(pending, sessions, runtimes);
    break;
  case PendingState::read_create_flags: {
    const auto flags = std::to_integer<std::uint8_t>(pending.field.front());
    if (flags > 1U) {
      pending.state = PendingState::unused;
    } else {
      pending.exit_policy = flags == 0 ? PaneExitPolicy::close : PaneExitPolicy::hold;
      begin_pending_field(pending, PendingState::read_working_directory_size, 2);
    }
    break;
  }
  case PendingState::read_working_directory_size: {
    const auto size =
        protocol::decode_bounded_size(std::span<const std::byte>(pending.field).first<2>());
    if (size == protocol::unavailable_working_directory_size ||
        size > protocol::working_directory_bytes_max) {
      pending.state = PendingState::unused;
    } else {
      begin_pending_field(pending, PendingState::read_working_directory, size);
    }
    break;
  }
  case PendingState::read_working_directory:
    pending.working_directory = {};
    pending.working_directory.size = pending.field_target;
    std::ranges::copy(std::span(pending.field).first(pending.working_directory.size),
                      std::as_writable_bytes(std::span(pending.working_directory.bytes)).begin());
    if (pending.working_directory.view().front() != '/' ||
        pending.working_directory.view().contains('\0')) {
      pending.state = PendingState::unused;
    } else {
      begin_pending_field(pending, PendingState::read_environment_size, 2);
    }
    break;
  case PendingState::read_environment_size: {
    const auto size =
        protocol::decode_bounded_size(std::span<const std::byte>(pending.field).first<2>());
    if (size > protocol::environment_bytes_max) {
      pending.state = PendingState::unused;
    } else if (size == 0) {
      pending.environment.clear();
      pending.environment_size = 0;
      begin_pending_field(pending, PendingState::read_launch_command_size, 2);
    } else {
      pending.environment_size = size;
      begin_pending_field(pending, PendingState::read_environment, size);
    }
    break;
  }
  case PendingState::read_environment:
    if (!valid_environment(
            std::span<const std::byte>(pending.field).first(pending.environment_size))) {
      pending.state = PendingState::unused;
    } else {
      try {
        pending.environment.resize(pending.environment_size);
      } catch (...) {
        fail_pending_output(pending);
        break;
      }
      std::ranges::copy(std::span(pending.field).first(pending.environment_size),
                        pending.environment.begin());
      begin_pending_field(pending, PendingState::read_launch_command_size, 2);
    }
    break;
  case PendingState::read_launch_command_size: {
    const auto size =
        protocol::decode_bounded_size(std::span<const std::byte>(pending.field).first<2>());
    if (size > protocol::command_bytes_max) {
      pending.state = PendingState::unused;
    } else if (size == 0) {
      pending.launch_command.clear();
      pending.launch_command_size = 0;
      pending.output.reset();
      prepare_named_command(pending, sessions, runtimes, activity_order);
    } else {
      try {
        pending.launch_command.resize(size);
      } catch (...) {
        fail_pending_output(pending);
        break;
      }
      pending.launch_command_size = size;
      pending.state = PendingState::read_launch_command;
      pending.field_size = 0;
      pending.field_target = size;
    }
    break;
  }
  case PendingState::read_launch_command: {
    const std::span<const std::byte> command(pending.launch_command);
    if (!valid_launch_command(command)) {
      pending.state = PendingState::unused;
    } else {
      pending.output.reset();
      prepare_named_command(pending, sessions, runtimes, activity_order);
    }
    break;
  }
  case PendingState::read_control_payload_size: {
    const auto size =
        protocol::decode_bounded_size(std::span<const std::byte>(pending.field).first<2>());
    if (size == 0 || size > protocol::control_payload_bytes_max || size > pending.field.size()) {
      pending.state = PendingState::unused;
    } else {
      begin_pending_field(pending, PendingState::read_control_payload, size);
    }
    break;
  }
  case PendingState::read_control_payload:
    prepare_control_payload(pending, sessions, runtimes);
    break;
  case PendingState::read_public_json:
  case PendingState::read_extension:
  case PendingState::execute_public_proc:
  case PendingState::prepare_public_observer:
  case PendingState::observe:
  case PendingState::unused:
  case PendingState::flush_response:
    LEMMA_ASSERT(false);
    break;
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-exception-escape)
void process_public_read(PendingConnection& pending, PublicProcExecutions& executions) noexcept {
  if (pending.state == PendingState::observe ||
      pending.state == PendingState::execute_public_proc) {
    std::array<std::byte, 256> unexpected{};
    const auto received = ::recv(pending.descriptor, unexpected.data(), unexpected.size(), 0);
    if (received == 0 || received > 0 ||
        (received < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
      pending.state = PendingState::unused;
    }
    return;
  }
  std::array<char, std::size_t{16} * 1'024U> input{};
  const auto received = ::recv(pending.descriptor, input.data(), input.size(), 0);
  if (received > 0) {
    record_pending_progress(pending);
    const auto size = static_cast<std::size_t>(received);
    if (size > api::json_bytes_max - pending.public_input.size()) {
      finish_public_output(pending,
                           encode_public_error({.reason = "payload_too_large", .field = {}}),
                           PendingDisposition::close);
      return;
    }
    try {
      pending.public_input.append(input.data(), size);
    } catch (...) {
      pending.state = PendingState::unused;
      return;
    }
    const auto newline = pending.public_input.find('\n');
    if (newline == std::string::npos) {
      return;
    }
    const auto trailing = std::string_view(pending.public_input).substr(newline + 1U);
    if (std::ranges::any_of(trailing, [](const char character) {
          return character != ' ' && character != '\t' && character != '\r' && character != '\n';
        })) {
      finish_public_output(pending,
                           encode_public_error({.reason = "pipelining_not_supported", .field = {}}),
                           PendingDisposition::close);
      return;
    }
    pending.public_input.resize(newline);
    if (!pending.public_input.empty() && pending.public_input.back() == '\r') {
      pending.public_input.pop_back();
    }
    prepare_public_document(pending, executions);
    return;
  }
  if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
    return;
  }
  pending.state = PendingState::unused;
}

// Admission validates and binds the complete first record before transferring descriptor ownership.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void process_extension_read(PendingConnections& connections, Sessions& sessions,
                            PaneRuntimeStore& runtimes, extension::Runtime& extensions,
                            ExtensionObservations& observations, PublicScratch& scratch_owner,
                            const std::size_t slot) noexcept {
  auto* const pending = std::span(connections).subspan(slot, 1).front().get();
  LEMMA_ASSERT(pending != nullptr && pending->extension_peer != nullptr);
  static_cast<void>(pending->extension_peer->read_ready());
  const auto record = pending->extension_peer->receive();
  if (!record.has_value()) {
    if (!pending->extension_peer->connected()) {
      close_pending(connections, slot, sessions);
    }
    return;
  }
  if (record->kind != extension::RecordKind::hello) {
    close_pending(connections, slot, sessions);
    return;
  }
  try {
    // JSON bytes and character storage have the same object representation.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view json(reinterpret_cast<const char*>(record->payload.data()),
                                record->payload.size());
    auto parsed = api::parse_json(json);
    auto hello = parsed.value.has_value() ? extension::decode_hello(*parsed.value) : std::nullopt;
    if (!hello.has_value()) {
      close_pending(connections, slot, sessions);
      return;
    }
    auto* const target = hello->subscription.session.has_value()
                             ? public_session(sessions, *hello->subscription.session)
                             : nullptr;
    if (hello->subscription.session.has_value() &&
        (target == nullptr || !target->active || !target->attachment.id.is_valid())) {
      close_pending(connections, slot, sessions);
      return;
    }
    pending->subscription = hello->subscription;
    const bool observe = (hello->capabilities & extension::capability_observe) != 0;
    const auto scratch = observe && hello->subscription.screen
                             ? acquire_public_scratch(scratch_owner)
                             : std::span<std::byte>{};
    if (observe && hello->subscription.screen && scratch.empty()) {
      close_pending(connections, slot, sessions);
      return;
    }
    auto snapshot = (hello->capabilities & extension::capability_observe) != 0
                        ? encode_initial_snapshot(*pending, sessions, runtimes, scratch, 1)
                        : std::string{};
    if ((hello->capabilities & extension::capability_observe) != 0 && snapshot.empty()) {
      close_pending(connections, slot, sessions);
      return;
    }
    pending->extension_peer->consume();
    auto peer = std::move(*pending->extension_peer);
    pending->extension_peer.reset();
    pending->descriptor = -1;
    const auto admitted = extensions.admit(
        std::move(peer), std::move(*hello), target != nullptr ? target->id : SessionId{},
        target != nullptr ? target->attachment.id : AttachmentId{}, snapshot);
    if (admitted.has_value()) {
      observations.at(admitted->slot()) = {.owner = *admitted,
                                           .panes = pending->observed_panes,
                                           .semantic_hash = pending->observed_semantic_hash,
                                           .presentation_hash = pending->observed_presentation_hash,
                                           .signal_stamp = pending->observed_signal_stamp};
    }
    close_pending(connections, slot, sessions);
  } catch (...) {
    close_pending(connections, slot, sessions);
  }
}

// The branches are the bounded outcomes of one nonblocking setup read.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void process_pending_fields(PendingConnections& connections, Sessions& sessions,
                            PaneRuntimeStore& runtimes, std::uint64_t& activity_order,
                            PublicProcExecutions& executions, extension::Runtime& extensions,
                            ExtensionObservations& observations, PublicScratch& scratch_owner,
                            const std::size_t slot) noexcept {
  auto* const pending = std::span(connections).subspan(slot, 1).front().get();
  LEMMA_ASSERT(pending != nullptr);
  constexpr std::size_t operations_per_turn_max = 8;
  for (std::size_t operation = 0; operation < operations_per_turn_max && pending->active() &&
                                  pending->state != PendingState::flush_response;
       ++operation) {
    if (pending->state == PendingState::execute_public_proc ||
        pending->state == PendingState::prepare_public_observer) {
      return;
    }
    if (pending->state == PendingState::read_extension) {
      process_extension_read(connections, sessions, runtimes, extensions, observations,
                             scratch_owner, slot);
      return;
    }
    if (pending->state == PendingState::read_public_json ||
        pending->state == PendingState::observe) {
      process_public_read(*pending, executions);
      if (pending->state == PendingState::unused) {
        close_pending(connections, slot, sessions);
      }
      return;
    }
    if (pending->state == PendingState::read_attach) {
      const auto decoded = pending->attach_decoder.next();
      if (!decoded.has_value()) {
        const auto reason = decoded.error() == protocol::DecodeError::version_mismatch
                                ? protocol::DisconnectReason::version_mismatch
                                : protocol::DisconnectReason::protocol_error;
        finish_pending_disconnect(*pending, reason,
                                  protocol::decode_error_diagnostic(decoded.error()));
        continue;
      }
      if (decoded->has_value()) {
        const auto& message = **decoded;
        LEMMA_ASSERT(message.kind == protocol::ClientMessageKind::hello);
        pending->session = {};
        pending->session.size = message.session.size();
        std::ranges::copy(message.session, pending->session.bytes.begin());
        pending->attach_dimensions = message.dimensions;
        pending->attach_host_theme =
            message.host_theme == nullptr ? std::nullopt : std::optional{*message.host_theme};
        pending->attach_decoder.consume();
        prepare_attach(*pending, sessions, runtimes, slot);
        continue;
      }
      auto available = pending->attach_decoder.writable_bytes();
      if (available.empty()) {
        finish_pending_disconnect(*pending, protocol::DisconnectReason::protocol_error,
                                  "attach decoder buffer exhausted");
        continue;
      }
      const auto received = ::recv(pending->descriptor, available.data(), available.size(), 0);
      if (received > 0) {
        if (!pending->attach_decoder.commit(static_cast<std::size_t>(received)).has_value()) {
          finish_pending_disconnect(*pending, protocol::DisconnectReason::protocol_error,
                                    "attach decoder buffer exhausted");
        } else {
          record_pending_progress(*pending);
        }
        continue;
      }
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      close_pending(connections, slot, sessions);
      return;
    }

    auto available =
        pending->state == PendingState::read_launch_command
            ? std::span(pending->launch_command)
                  .subspan(pending->field_size, pending->field_target - pending->field_size)
            : std::span(pending->field)
                  .subspan(pending->field_size, pending->field_target - pending->field_size);
    const auto received = ::recv(pending->descriptor, available.data(), available.size(), 0);
    if (received > 0) {
      pending->field_size += static_cast<std::size_t>(received);
      record_pending_progress(*pending);
      if (pending->field_size == pending->field_target) {
        complete_pending_field(*pending, sessions, runtimes, activity_order);
      }
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    close_pending(connections, slot, sessions);
    return;
  }
  if (!pending->active()) {
    close_pending(connections, slot, sessions);
  }
}

void process_pending_read(PendingConnections& connections, Sessions& sessions,
                          PaneRuntimeStore& runtimes, std::uint64_t& activity_order,
                          PublicProcExecutions& executions, extension::Runtime& extensions,
                          ExtensionObservations& observations, PublicScratch& scratch_owner,
                          const std::size_t slot) noexcept {
  auto* const pending = std::span(connections).subspan(slot, 1).front().get();
  LEMMA_ASSERT(pending != nullptr);
  if (!pending->public_connection && reactor_now() >= pending->setup_deadline) {
    close_pending(connections, slot, sessions);
    return;
  }
  process_pending_fields(connections, sessions, runtimes, activity_order, executions, extensions,
                         observations, scratch_owner, slot);
}

void handle_client_parse_result(SessionRecord& session, PaneRuntimeStore& runtimes,
                                ParseResult result) noexcept;

void handoff_attached_connection(PendingConnections& connections, const std::size_t slot,
                                 Sessions& sessions, PaneRuntimeStore& runtimes,
                                 extension::Runtime& extensions,
                                 std::uint64_t& activity_order) noexcept {
  auto& owner = std::span(connections).subspan(slot, 1).front();
  LEMMA_ASSERT(owner != nullptr);
  auto& pending = *owner;
  SessionRecord* const session = sessions.get(pending.attach_session);
  if (session == nullptr || !session->active ||
      session->attachment_runtime.pending_attach_slot != slot ||
      session->attachment_runtime.pending_attach_generation != pending.generation) {
    close_pending(connections, slot, sessions);
    return;
  }

  const int connection = pending.descriptor;
  pending.descriptor = -1;
  session->attachment_runtime.client = connection;
  session->attachment_runtime.decoder = std::move(pending.attach_decoder);
  release_attach_reservation(pending, slot, sessions);
  owner.reset();

  if (activity_order < std::numeric_limits<std::uint64_t>::max()) {
    session->activity_order = ++activity_order;
  }
  session->attachment_runtime.connection_id = sessions.allocate_connection(session->id);
  session->attachment_runtime.output.reset();
  session->attachment_runtime.server_sequence = 2;
  session->attachment_runtime.full_redraw_generation = 0;
  session->attachment_runtime.input_backpressured = false;
  session->attachment_runtime.client_work_pending = false;
  session->attachment_runtime.retained_input_offset.reset();
  session->attachment_runtime.pending_routed_input_size = 0;
  session->attachment_runtime.client_close_state = ConnectionCloseState::none;
  session->attachment_runtime.client_close_reason = protocol::DisconnectReason::protocol_error;
  session->attachment_runtime.frame_scheduler.cancel();
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  session->attachment_runtime.frame_trace_correlation = 0;
#endif
  if (!compose_session_frame(*session, runtimes, extensions, true, reactor_now())) {
    detach_attachment(*session, runtimes);
    return;
  }
  reconcile_focus_reports(*session, runtimes);
  auto message_budget = client_messages_per_turn_max;
  auto geometry_budget = client_geometry_messages_per_turn_max;
  auto input_budget = client_input_steps_per_turn_max;
  handle_client_parse_result(*session, runtimes,
                             parse_client_packets(*session, runtimes, extensions, message_budget,
                                                  geometry_budget, input_budget,
                                                  &session_name_conflict, &sessions));
}

[[nodiscard]] auto write_pending_output(void* const context,
                                        const std::span<const std::byte> bytes) noexcept
    -> ConnectionWriteAttempt {
  auto& pending = *static_cast<PendingConnection*>(context);
  const auto sent = reactor_send(pending.descriptor, bytes, MSG_NOSIGNAL);
  if (sent.bytes > 0) {
    record_pending_progress(pending);
  }
  return {.bytes = sent.bytes, .error = sent.error};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-exception-escape)
[[nodiscard]] auto flush_pending_output(PendingConnections& connections, const std::size_t slot,
                                        std::size_t& global_budget, Sessions& sessions,
                                        PaneRuntimeStore& runtimes, extension::Runtime& extensions,
                                        std::uint64_t& activity_order) noexcept -> bool {
  auto* const pending = std::span(connections).subspan(slot, 1).front().get();
  LEMMA_ASSERT(pending != nullptr);
  const auto disposition = pending->disposition;
  if (pending->public_connection) {
    while (pending->public_output_offset < pending->public_output.size() && global_budget > 0) {
      const auto remaining =
          std::string_view(pending->public_output)
              .substr(pending->public_output_offset,
                      std::min(pending->public_output.size() - pending->public_output_offset,
                               global_budget));
      const auto sent =
          reactor_send(pending->descriptor, std::as_bytes(std::span(remaining)), MSG_NOSIGNAL);
      if (sent.bytes > 0) {
        const auto size = static_cast<std::size_t>(sent.bytes);
        pending->public_output_offset += size;
        global_budget -= size;
        record_pending_progress(*pending);
        continue;
      }
      if (sent.bytes < 0 && sent.error == EINTR) {
        continue;
      }
      if (sent.bytes < 0 && (sent.error == EAGAIN || sent.error == EWOULDBLOCK)) {
        return false;
      }
      close_pending(connections, slot, sessions);
      return false;
    }
    if (pending->public_output_offset < pending->public_output.size()) {
      return false;
    }
    pending->public_output.clear();
    pending->public_output_offset = 0;
    if (disposition == PendingDisposition::keep_proc) {
      pending->public_input.clear();
      pending->state = PendingState::read_public_json;
      pending->disposition = PendingDisposition::close;
      return false;
    }
    if (disposition == PendingDisposition::keep_observe) {
      pending->public_input.clear();
      pending->state = PendingState::observe;
      pending->disposition = PendingDisposition::close;
      return false;
    }
    close_pending(connections, slot, sessions);
    return false;
  }
  const auto status =
      flush_connection_output(pending->output, global_budget, &write_pending_output, pending);
  if (status == ConnectionFlushStatus::hard_error) {
    close_pending(connections, slot, sessions);
    return disposition == PendingDisposition::shutdown;
  }
  if (!pending->active() || status != ConnectionFlushStatus::drained) {
    return false;
  }
  if (disposition == PendingDisposition::attach) {
    handoff_attached_connection(connections, slot, sessions, runtimes, extensions, activity_order);
  } else {
    close_pending(connections, slot, sessions);
  }
  return disposition == PendingDisposition::shutdown;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void process_capacity_rejection_read(CapacityRejectionConnections& connections,
                                     const std::size_t slot) noexcept {
  auto& connection = std::span(connections).subspan(slot, 1).front();
  LEMMA_ASSERT(connection.active() && !connection.flush_response);
  std::byte discriminator{};
  const auto received = ::recv(connection.descriptor, &discriminator, 1, 0);
  if (received > 0) {
    connection.output.reset();
    if (discriminator == protocol::attach_magic.front()) {
      const auto rejection = protocol::encode_disconnect(
          protocol::DisconnectReason::capacity, "daemon pending connection capacity exhausted");
      const bool appended = connection.output.append(rejection.bytes());
      LEMMA_ASSERT(appended);
    } else if (discriminator == std::byte{'{'}) {
      constexpr std::string_view rejection =
          R"({"schema":"lemma.proc-result/v1","ok":false,"error":{"reason":"capacity"},"results":[]}
)";
      const bool appended = connection.output.append(std::as_bytes(std::span(rejection)));
      LEMMA_ASSERT(appended);
    } else {
      const bool appended = connection.output.append(std::span(&response_capacity, 1));
      LEMMA_ASSERT(appended);
    }
    connection.flush_response = true;
    connection.deadline = reactor_now() + setup_progress_timeout;
    return;
  }
  if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
    return;
  }
  close_capacity_rejection(connections, slot);
}

[[nodiscard]] auto write_capacity_rejection_output(void* const context,
                                                   const std::span<const std::byte> bytes) noexcept
    -> ConnectionWriteAttempt {
  auto& connection = *static_cast<CapacityRejectionConnection*>(context);
  const auto sent = reactor_send(connection.descriptor, bytes, MSG_NOSIGNAL);
  if (sent.bytes > 0) {
    connection.deadline = reactor_now() + setup_progress_timeout;
  }
  return {.bytes = sent.bytes, .error = sent.error};
}

void flush_capacity_rejection_output(CapacityRejectionConnections& connections,
                                     const std::size_t slot, std::size_t& global_budget) noexcept {
  auto& connection = std::span(connections).subspan(slot, 1).front();
  LEMMA_ASSERT(connection.active() && connection.flush_response);
  const auto status = flush_connection_output(connection.output, global_budget,
                                              &write_capacity_rejection_output, &connection);
  if (status == ConnectionFlushStatus::drained || status == ConnectionFlushStatus::hard_error) {
    close_capacity_rejection(connections, slot);
  }
}

// Deadline folding is bounded across the fixed session/tab/pane hierarchy.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto frame_poll_timeout(const Sessions& sessions, const PaneRuntimeStore& runtimes,
                                      const FrameScheduler::TimePoint now, int timeout) noexcept
    -> int {
  const auto tighten = [now, &timeout](const std::optional<FrameScheduler::TimePoint> deadline) {
    if (!deadline.has_value()) {
      return false;
    }
    if (now >= *deadline) {
      timeout = 0;
      return true;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
    const auto candidate = static_cast<int>(std::max(remaining.count(), std::int64_t{1}));
    timeout = timeout < 0 ? candidate : std::min(timeout, candidate);
    return false;
  };
  for (const auto& session : sessions) {
    if (session == nullptr || !session->active) {
      continue;
    }
    if (session->attachment_runtime.client_work_pending ||
        session->attachment.command_line.completion_requested ||
        (session->attachment.command_line.submit_requested &&
         !session->attachment_runtime.output.busy())) {
      return 0;
    }
    if (const auto& paste = session->attachment_runtime.surface_paste;
        paste && tighten(paste->deadline)) {
      return 0;
    }
    if (session->attachment_runtime.clipboard != nullptr &&
        (session->attachment_runtime.clipboard->done() ||
         tighten(session->attachment_runtime.clipboard->deadline()))) {
      return 0;
    }
    if (const auto& paste = session->attachment_runtime.interactive_paste; paste != nullptr) {
      if (!image_paste_owned(*paste, sessions) || paste->reply != nullptr ||
          (paste->file != nullptr && tighten(paste->file->deadline()))) {
        return 0;
      }
    }
    if ((session->attachment_runtime.copy_mode.search_task.has_value() &&
         tighten(session->attachment_runtime.copy_mode.search_task->deadline)) ||
        (session->attachment_runtime.copy_mode.pending_escape_size > 0 &&
         tighten(session->attachment_runtime.copy_mode.pending_escape_deadline)) ||
        (session->attachment.status_message_visible &&
         tighten(session->attachment_runtime.status_message_deadline)) ||
        tighten(session->attachment_runtime.frame_scheduler.deadline(frame_sink_state(*session))) ||
        tighten(session->attachment_runtime.output.deadline()) ||
        (session->attachment_runtime.client >= 0 && !session->attachment_runtime.output.busy() &&
         tighten(session->attachment_runtime.graphics.deadline()))) {
      return 0;
    }
    for (const auto& pane_slot : session->panes) {
      if (pane_slot.pane == nullptr) {
        continue;
      }
      const auto* const runtime = find_pane_runtime(runtimes, *session, *pane_slot.pane);
      LEMMA_ASSERT(runtime != nullptr);
      if (tighten(runtime->presentation_gate.deadline()) ||
          (runtime->compression_scheduled && tighten(runtime->compression_deadline))) {
        return 0;
      }
    }
  }
  return timeout;
}

[[nodiscard]] auto public_deadline_applies(const PendingConnection& pending) noexcept -> bool {
  return pending.state == PendingState::flush_response ||
         (pending.state == PendingState::read_public_json && !pending.public_input.empty());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto poll_timeout(Sessions& sessions, PaneRuntimeStore& runtimes,
                                const PendingConnections& pending,
                                const PublicProcExecutions& executions,
                                const CapacityRejectionConnections& capacity_rejections,
                                const bool immediate_public_work) noexcept -> int {
  const auto now = reactor_now();
  int timeout = -1;
  if (immediate_public_work) {
    timeout = 0;
  }
  const auto tighten = [now, &timeout](const auto& connection) {
    if (now >= connection.deadline) {
      return true;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(connection.deadline - now);
    const auto candidate = static_cast<int>(std::max(remaining.count(), std::int64_t{1}));
    timeout = timeout < 0 ? candidate : std::min(timeout, candidate);
    return false;
  };
  for (const auto& slot : executions) {
    if (slot.execution == nullptr) {
      continue;
    }
    if (const auto& paste = slot.execution->image_paste; paste != nullptr) {
      if (!image_paste_owned(*paste, sessions) || paste->reply != nullptr) {
        return 0;
      }
      if (paste->file != nullptr) {
        struct FileDeadline {
          std::chrono::steady_clock::time_point deadline;
        };
        if (tighten(FileDeadline{.deadline = paste->file->deadline()})) {
          return 0;
        }
      }
      continue;
    }
    if (slot.execution->reload != nullptr && !slot.execution->reload->complete) {
      continue;
    }
    if (!slot.execution->wait.has_value()) {
      return 0;
    }
    if (public_wait_has_work(*slot.execution->wait, sessions, runtimes, now) ||
        tighten(*slot.execution->wait)) {
      return 0;
    }
  }
  for (const auto& connection : pending) {
    if (connection == nullptr || !connection->active()) {
      continue;
    }
    if (connection->state == PendingState::execute_public_proc) {
      continue;
    }
    if ((!connection->public_connection || public_deadline_applies(*connection)) &&
        tighten(*connection)) {
      return 0;
    }
  }
  for (const auto& connection : capacity_rejections) {
    if (connection.active() && tighten(connection)) {
      return 0;
    }
  }
  return frame_poll_timeout(sessions, runtimes, now, timeout);
}

struct PaneDamageAssessment final {
  bool interactive{false};
  bool status_changed{false};
};

[[nodiscard]] auto assess_pane_damage(SessionRecord& session, const PaneRuntimeStore& runtimes,
                                      PaneRuntime& runtime, const PtyDrainResult& drained,
                                      const bool track_interactive_damage,
                                      const std::uint64_t interactive_status_before) noexcept
    -> PaneDamageAssessment {
  const auto status_after = current_status_signature(session, runtimes);
  const bool interactive_status_damage =
      track_interactive_damage && status_after != interactive_status_before;
  const bool status_changed = !session.attachment_runtime.status_valid ||
                              status_after != session.attachment_runtime.status_signature;
  const bool visible_damage =
      drained.render_damage || interactive_status_damage || drained.damage_capture_failed;
  const bool interactive_damage = runtime.interactive_damage.pending() && visible_damage;
  if (interactive_damage) {
    // Damage in an inactive tab is already covered by its next full redraw. Do not let the input
    // latch promote an unrelated update after the tab becomes active again.
    static_cast<void>(runtime.interactive_damage.consume());
  }
  return {.interactive = interactive_damage, .status_changed = status_changed};
}

[[nodiscard]] auto frame_urgency(const PtyDrainResult& drained, const bool process_changed,
                                 const PaneDamageAssessment damage) noexcept -> FrameUrgency {
  if (drained.damage_capture_failed || drained.presentation_released) {
    return FrameUrgency::state_change;
  }
  if (damage.interactive) {
    return FrameUrgency::interactive;
  }
  if (drained.failure.has_value() || process_changed || damage.status_changed) {
    return FrameUrgency::state_change;
  }
  return FrameUrgency::burst;
}

[[nodiscard]] auto pane_event_changed(const SessionRecord& session, const PtyDrainResult& drained,
                                      const bool process_changed) noexcept -> bool {
  if (session.attachment_runtime.client < 0) {
    return false;
  }
  return drained.changed || process_changed;
}

constexpr std::size_t blocked_sink_pty_read_bytes_per_turn_max = std::size_t{4} * 1'024U;

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void process_pane_events(SessionRecord& session, Tab& tab, Pane& pane, PaneRuntime& runtime,
                         PaneRuntimeStore& runtimes, const pollfd& events,
                         std::size_t& global_budget, std::size_t& blocked_session_budget) noexcept {
  if ((events.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
    return;
  }
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  auto* const trace_matcher = &runtime.output_trace_matcher;
#else
  diagnostic::LatencyTraceMarkerMatcher* const trace_matcher = nullptr;
#endif
  const bool track_interactive_damage =
      session.attachment_runtime.client >= 0 && runtime.interactive_damage.pending();
  const auto interactive_status_before =
      track_interactive_damage ? current_status_signature(session, runtimes) : 0;
  // A client-blocked session keeps consuming canonical PTY state, but all of its ready panes share
  // one isolation slice so a many-pane session cannot spend the daemon-wide allowance by taking a
  // fresh slice for every pane.
  const bool blocked_sink = session.attachment_runtime.output.busy();
  std::size_t pane_budget =
      blocked_sink ? std::min(global_budget, blocked_session_budget) : global_budget;
  if (pane_budget == 0) {
    return;
  }
  const bool clipboard_eligible = session.attachment_runtime.client >= 0 &&
                                  tab.id == session.active_tab && pane.id == tab.focused_pane;
  runtime.terminal.set_clipboard_access(
      clipboard_eligible && active_reactor_environment->clipboard_read,
      clipboard_eligible && active_reactor_environment->clipboard_write);
  const auto pane_budget_before = pane_budget;
  const auto drained =
      drain_pty(runtime.pty, runtime.terminal, runtime.presentation_gate, runtime.pending_writes,
                pane_budget, track_interactive_damage, trace_matcher);
  if (runtime.terminal.clipboard_request().has_value()) {
    start_clipboard_request(session, pane, runtime);
  }
  const auto bytes_drained = pane_budget_before - pane_budget;
  global_budget -= bytes_drained;
  if (blocked_sink) {
    blocked_session_budget -= bytes_drained;
  }
  if (drained.failure.has_value()) {
    runtime.fail(*drained.failure);
  }
  session.attachment_runtime.bell_pending = session.attachment_runtime.bell_pending || drained.bell;
  if (drained.signal) {
    runtime.signal_stamp = runtimes.issue_signal_stamp();
    session.signal_stamp = runtime.signal_stamp;
  }
  if (drained.title_changed) {
    session.attachment_runtime.status_valid = false;
  }
  if (drained.changed && runtime.live()) {
    record_terminal_mutation(runtime);
    preserve_copy_viewport_after_mutation(session, pane, runtime, runtimes);
    note_compression_activity(runtime);
  }
  const bool process_changed = refresh_process_name_if_due(runtime, reactor_now());
  if (!pane_event_changed(session, drained, process_changed)) {
    return;
  }
  const auto damage = assess_pane_damage(session, runtimes, runtime, drained,
                                         track_interactive_damage, interactive_status_before);
#ifdef LEMMA_ENABLE_LATENCY_TRACE
  if (drained.correlation != 0 && tab.id == session.active_tab && pane.id == tab.focused_pane) {
    session.attachment_runtime.frame_trace_correlation = drained.correlation;
  }
#endif
  if (session.attachment.message_view.active && !drained.bell && !process_changed &&
      !damage.status_changed) {
    return;
  }
  if (tab.id == session.active_tab && (!drained.presentation_deferred || drained.bell ||
                                       process_changed || damage.status_changed)) {
    schedule_frame(session, frame_urgency(drained, process_changed, damage),
                   drained.damage_capture_failed || drained.force_full, pane.id);
  } else if (damage.status_changed) {
    schedule_frame(session, FrameUrgency::state_change, false);
  }
}

void queue_client_disconnect_if_ready(SessionRecord& session, PaneRuntimeStore& runtimes) noexcept {
  if (session.attachment_runtime.client_close_state != ConnectionCloseState::queue_disconnect ||
      session.attachment_runtime.output.busy()) {
    return;
  }
  const auto reason = session.attachment_runtime.client_close_reason;
  const auto diagnostic = reason == protocol::DisconnectReason::normal
                              ? std::string_view{}
                              : std::string_view{"invalid client protocol message"};
  if (session.attachment_runtime.server_sequence == 0 ||
      session.attachment_runtime.server_sequence == std::numeric_limits<std::uint32_t>::max() ||
      !session.attachment_runtime.output.queue_disconnect(
          reason, diagnostic, session.attachment_runtime.server_sequence, reactor_now())) {
    detach_attachment(session, runtimes);
    return;
  }
  ++session.attachment_runtime.server_sequence;
  session.attachment_runtime.client_close_state = ConnectionCloseState::disconnect_queued;
}

void handle_client_parse_result(SessionRecord& session, PaneRuntimeStore& runtimes,
                                const ParseResult result) noexcept {
  session.attachment_runtime.input_backpressured = result == ParseResult::backpressure;
  session.attachment_runtime.client_work_pending = result == ParseResult::yield;
  if (result == ParseResult::detach) {
    session.attachment_runtime.client_close_reason = protocol::DisconnectReason::normal;
    session.attachment_runtime.client_close_state = ConnectionCloseState::queue_disconnect;
    session.attachment_runtime.input_backpressured = false;
    session.attachment_runtime.client_work_pending = false;
    queue_client_disconnect_if_ready(session, runtimes);
    return;
  }
  if (result == ParseResult::peer_closed) {
    detach_attachment(session, runtimes);
    return;
  }
  if (result == ParseResult::error) {
    session.attachment_runtime.client_close_reason = protocol::DisconnectReason::protocol_error;
    session.attachment_runtime.client_close_state = ConnectionCloseState::queue_disconnect;
    session.attachment_runtime.input_backpressured = false;
    session.attachment_runtime.client_work_pending = false;
    queue_client_disconnect_if_ready(session, runtimes);
  }
}

void process_client_events(SessionRecord& session, PaneRuntimeStore& runtimes,
                           extension::Runtime& extensions, const pollfd& events,
                           std::size_t& message_budget, std::size_t& geometry_budget,
                           std::size_t& input_budget, const SessionNameConflict name_conflict,
                           void* const name_conflict_context) noexcept {
  // Consume resizes before flushing queued output so resize_session can discard bytes composed
  // for the previous physical viewport. Decoder-held work is retried without socket readiness on a
  // later bounded turn or after destination capacity becomes available. A closed peer cannot
  // retain it.
  if (session.attachment_runtime.client >= 0 &&
      (session.attachment_runtime.input_backpressured ||
       session.attachment_runtime.client_work_pending) &&
      (events.revents & (POLLHUP | POLLERR)) != 0) {
    detach_attachment(session, runtimes);
    return;
  }
  if (session.attachment_runtime.client >= 0 &&
      session.attachment_runtime.client_close_state == ConnectionCloseState::none &&
      (session.attachment_runtime.input_backpressured ||
       session.attachment_runtime.client_work_pending ||
       (events.revents & (POLLIN | POLLHUP | POLLERR)) != 0)) {
    handle_client_parse_result(session, runtimes,
                               receive_client(session, runtimes, extensions, message_budget,
                                              geometry_budget, input_budget, name_conflict,
                                              name_conflict_context));
  }
}

// Decimal conversion iterators are raw pointers in this standard library implementation.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
template <typename Id, std::size_t Size>
[[nodiscard]] auto command_line_id(const Id id, std::array<char, Size>& output) noexcept
    -> std::string_view {
  const auto slot = std::to_chars(output.begin(), output.end(), id.slot());
  if (slot.ec != std::errc{} || slot.ptr == output.end()) {
    return {};
  }
  *slot.ptr = ':';
  const auto generation = std::to_chars(std::next(slot.ptr), output.end(), id.generation());
  return generation.ec == std::errc{}
             ? std::string_view(output.data(),
                                static_cast<std::size_t>(generation.ptr - output.data()))
             : std::string_view{};
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

// Completion edits use parser-validated ranges into one bounded line and candidate arrays.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void apply_command_line_completion(CommandLineState& state, const CommandLineCompletionQuery& query,
                                   const CommandLineCompletion& completion) noexcept {
  state.completion_requested = false;
  if (completion.replacement.empty()) {
    return;
  }
  const bool add_space =
      completion.append_space &&
      (query.replace_end == state.size ||
       (state.text[query.replace_end] != ' ' && state.text[query.replace_end] != '\t'));
  const auto inserted = completion.replacement.size() + (add_space ? 1U : 0U);
  const auto removed = query.replace_end - query.replace_begin;
  if (state.size - removed + inserted > state.text.size()) {
    return;
  }
  const auto tail = state.size - query.replace_end;
  if (inserted > removed) {
    const auto difference = inserted - removed;
    for (auto index = tail; index > 0; --index) {
      state.text[query.replace_end + difference + index - 1U] =
          state.text[query.replace_end + index - 1U];
    }
  } else {
    const auto difference = removed - inserted;
    for (std::size_t index = 0; index < tail; ++index) {
      state.text[query.replace_end - difference + index] = state.text[query.replace_end + index];
    }
  }
  const auto replacement_target = std::span(state.text).subspan(query.replace_begin);
  std::ranges::copy(completion.replacement, replacement_target.begin());
  if (add_space) {
    state.text[query.replace_begin + completion.replacement.size()] = ' ';
  }
  state.size = state.size - removed + inserted;
  state.cursor = query.replace_begin + inserted;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void complete_attachment_command_line(SessionRecord& session, Sessions& sessions,
                                      const extension::CommandRuntime& extensions) noexcept {
  auto& state = session.attachment.command_line;
  const auto query = command_line_completion_query(state.view(), state.cursor);
  constexpr auto candidate_capacity =
      std::max(panes_per_session_max, static_cast<std::size_t>(limits::sessions_hard_max)) +
      extension::commands_max;
  std::array<std::string_view, candidate_capacity> candidates{};
  std::array<std::array<char, 32>, panes_per_session_max> encoded{};
  std::size_t count = 0;
  if (query.kind == CommandLineCompletionKind::session) {
    for (const auto& candidate : sessions) {
      if (candidate != nullptr && candidate->active) {
        candidates[count++] = candidate->session_name();
      }
    }
  } else if (query.kind == CommandLineCompletionKind::tab) {
    for (std::size_t position = 0; position < session.tab_order.size(); ++position) {
      const auto encoded_position =
          std::to_chars(encoded[position].begin(), encoded[position].end(), position + 1U);
      if (encoded_position.ec == std::errc{}) {
        candidates[count++] = std::string_view(
            encoded[position].data(),
            static_cast<std::size_t>(encoded_position.ptr - encoded[position].begin()));
      }
    }
  } else if (query.kind == CommandLineCompletionKind::pane) {
    for (const auto& pane_slot : session.panes) {
      if (pane_slot.pane == nullptr) {
        continue;
      }
      auto& bytes = encoded[count];
      const auto value = command_line_id(pane_slot.pane->id, bytes);
      if (!value.empty()) {
        candidates[count++] = value;
      }
    }
  } else {
    const auto static_candidates = command_line_static_completions(query.kind);
    std::ranges::copy(static_candidates, candidates.begin());
    count = static_candidates.size();
    if (query.kind == CommandLineCompletionKind::root) {
      for (const auto& command : extensions.commands()) {
        candidates[count++] = command.name;
      }
    }
  }
  const auto completion = complete_command_line(query.prefix, std::span(candidates).first(count));
  apply_command_line_completion(state, query, completion);
  invalidate_command_line(session);
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)

[[nodiscard]] constexpr auto command_line_message(const CommandLineError error) noexcept
    -> std::string_view {
  switch (error) {
  case CommandLineError::unknown_command:
    return "Error: Unknown command";
  case CommandLineError::capacity:
    return "Error: Capacity reached";
  case CommandLineError::invalid_syntax:
    return "Error: Invalid syntax";
  }
  return "Error: Command failed";
}

[[nodiscard]] constexpr auto command_line_message(const AttachmentTransferResult result) noexcept
    -> std::string_view {
  switch (result) {
  case AttachmentTransferResult::conflict:
    return "Error: Target is busy";
  case AttachmentTransferResult::capacity:
    return "Error: Capacity reached";
  case AttachmentTransferResult::unavailable:
    return "Error: Target not found";
  case AttachmentTransferResult::failed:
    return "Error: Command failed";
  case AttachmentTransferResult::transferred:
  case AttachmentTransferResult::deferred:
    return {};
  }
  return "Error: Command failed";
}

[[nodiscard]] constexpr auto command_line_message(const CommandStatus status) noexcept
    -> std::string_view {
  switch (status) {
  case CommandStatus::conflict:
    return "Error: Target is busy";
  case CommandStatus::capacity:
    return "Error: Capacity reached";
  case CommandStatus::invalid_target:
  case CommandStatus::stale_target:
  case CommandStatus::wrong_owner:
  case CommandStatus::unavailable:
    return "Error: Target not found";
  case CommandStatus::invalid_command:
    return "Error: Invalid syntax";
  case CommandStatus::failed:
    return "Error: Command failed";
  case CommandStatus::applied:
  case CommandStatus::no_effect:
  case CommandStatus::detach_requested:
    return {};
  }
  return "Error: Command failed";
}

void finish_command_line_error(SessionRecord& session, const std::string_view message) noexcept {
  reset_command_line(session, false);
  publish_status_message(session, StatusMessageKind::error, message);
}

// Geometry, theme, transport, and graphics ownership transfer as one bounded transition.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto transfer_attachment(SessionRecord& source, SessionRecord& target,
                                       Sessions& sessions, PaneRuntimeStore& runtimes,
                                       std::uint64_t& activity_order) noexcept
    -> AttachmentTransferResult {
  if (&source == &target) {
    reset_command_line(source);
    return AttachmentTransferResult::transferred;
  }
  auto& source_runtime = source.attachment_runtime;
  auto& target_runtime = target.attachment_runtime;
  if (!target.active) {
    return AttachmentTransferResult::unavailable;
  }
  if (target_runtime.client >= 0 ||
      target_runtime.pending_attach_slot != std::numeric_limits<std::uint32_t>::max()) {
    return AttachmentTransferResult::conflict;
  }
  if (source_runtime.output.busy() || source_runtime.clipboard_write.bytes != nullptr ||
      source_runtime.clipboard != nullptr || !source_runtime.clipboard_paste.expired()) {
    return AttachmentTransferResult::deferred;
  }
  if (!sessions.connection_available(target.id)) {
    return AttachmentTransferResult::capacity;
  }
  target_runtime.reset_connection();
  const protocol::Dimensions previous_dimensions{.columns = target.attachment.columns,
                                                 .rows = target.attachment.rows};
  const protocol::Dimensions dimensions{.columns = source.attachment.columns,
                                        .rows = source.attachment.rows};
  const auto previous_cells = target_runtime.cell_size;
  if (!resize_session(target, runtimes, dimensions) ||
      !apply_cell_size(target, runtimes, source_runtime.cell_size)) {
    if (!resize_session(target, runtimes, previous_dimensions) ||
        !apply_cell_size(target, runtimes, previous_cells)) {
      target.active = false;
    }
    return AttachmentTransferResult::failed;
  }
  if (!target.theme_bound && source.theme_bound &&
      !apply_session_theme(target, runtimes, source.theme)) {
    // Viewport and theme preparation are one transaction, as on an ordinary attach. If either
    // rollback cannot restore a usable Session, keep it out of subsequent target resolution.
    if (target.active && (!resize_session(target, runtimes, previous_dimensions) ||
                          !apply_cell_size(target, runtimes, previous_cells))) {
      target.active = false;
    }
    return AttachmentTransferResult::failed;
  }

  const auto server_sequence = source_runtime.server_sequence;
  const auto full_redraw_generation = source_runtime.full_redraw_generation;
  const auto previous_outer_modes = source_runtime.outer_modes;
  // A retained Surface record must revalidate its old owner after the connection moves.
  const bool client_work_pending =
      source_runtime.client_work_pending || source_runtime.surface_paste.has_value();
  const auto surface_paste = source_runtime.surface_paste;
  const auto kitty_clipboard_supported = source_runtime.kitty_clipboard_supported;
  const bool osc52_read_retired = source_runtime.osc52_read_retired;
  const bool outer_focused = source_runtime.outer_focused;
  const int client = std::exchange(source_runtime.client, -1);
  auto decoder = std::move(source_runtime.decoder);
  auto graphics = std::move(source_runtime.graphics);
  source_runtime.decoder = {};
  auto history = source.attachment.command_history;
  auto messages = source.attachment.status_messages;
  source.attachment.command_history = {};
  source.attachment.status_messages = {};
  detach_attachment(source, runtimes);

  target_runtime.client = client;
  target_runtime.decoder = std::move(decoder);
  target_runtime.graphics = std::move(graphics);
  target_runtime.server_sequence = server_sequence;
  target_runtime.full_redraw_generation = full_redraw_generation;
  target_runtime.outer_modes = previous_outer_modes;
  target_runtime.client_work_pending = client_work_pending;
  target_runtime.surface_paste = surface_paste;
  target_runtime.kitty_clipboard_supported = kitty_clipboard_supported;
  target_runtime.osc52_read_retired = osc52_read_retired;
  target_runtime.outer_focused = outer_focused;
  target_runtime.connection_id = sessions.allocate_connection(target.id);
  target_runtime.status_valid = false;
  target_runtime.client_close_state = ConnectionCloseState::none;
  target.attachment.command_history = history;
  target.attachment.status_messages = messages;
  target.attachment.command_line = {};
  target.attachment.status_message_visible = false;
  target.attachment.message_view = {};
  target.input_router.reset();
  target.interaction_router.reset();
  if (activity_order < std::numeric_limits<std::uint64_t>::max()) {
    target.activity_order = ++activity_order;
  }
  schedule_frame(target, FrameUrgency::state_change, true);
  reconcile_focus_reports(source, runtimes);
  reconcile_focus_reports(target, runtimes);
  return AttachmentTransferResult::transferred;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void service_attachment_command_lines(Sessions& sessions, PaneRuntimeStore& runtimes,
                                      std::uint64_t& activity_order,
                                      extension::CommandRuntime& extensions,
                                      extension::Runtime& surface_runtime) noexcept {
  for (auto& owner : sessions) {
    if (owner == nullptr || !owner->active || owner->attachment_runtime.client < 0) {
      continue;
    }
    auto& session = *owner;
    if (const auto queued = std::exchange(session.attachment_runtime.hosted_command, std::nullopt);
        queued.has_value()) {
      const auto commands = extensions.commands();
      if (queued->index >= commands.size() ||
          !extensions.start(commands.subspan(queued->index, 1).front().name, {}, queued->context,
                            reactor_now())) {
        publish_status_message(session, StatusMessageKind::error,
                               "Error: Extension unavailable or busy");
      }
    }
    auto& state = session.attachment.command_line;
    if (!state.active) {
      continue;
    }
    if (state.completion_requested) {
      try {
        complete_attachment_command_line(session, sessions, extensions);
      } catch (...) {
        state.completion_requested = false;
        invalidate_command_line(session);
      }
      continue;
    }
    if (!state.submit_requested) {
      continue;
    }
    remember_attachment_command_line(session.attachment);
    auto* const tab = active_tab(session);
    auto* const pane = tab == nullptr ? nullptr : find_pane(session, *tab, tab->focused_pane);
    if (tab == nullptr || pane == nullptr) {
      finish_command_line_error(session, "Error: Target not found");
      continue;
    }
    try {
      const auto parsed = parse_command_line(
          state.view(), {.session = session.id, .tab = tab->id, .pane = pane->id},
          extensions.commands());
      if (!parsed.has_value()) {
        finish_command_line_error(session, command_line_message(parsed.error()));
        continue;
      }
      const auto& action = *parsed;
      if (action.kind == CommandLineActionKind::command &&
          action.command.kind == api::CommandKind::pane_paste_image) {
        PublicCommandExecution result;
        auto job = begin_image_paste(action.command, sessions, result);
        if (job == nullptr) {
          finish_command_line_error(session, "Error: Image paste busy or unavailable");
        } else {
          session.attachment_runtime.interactive_paste = std::move(job);
          reset_command_line(session);
          publish_status_message(session, StatusMessageKind::information,
                                 "Reading clipboard image");
        }
        continue;
      }
      if (action.kind == CommandLineActionKind::command &&
          action.command.kind == api::CommandKind::config_reload) {
        auto result = start_reload();
        if (result == nullptr) {
          finish_command_line_error(session, "Error: Reload busy or unavailable");
        } else {
          active_reload->interactive = std::move(result);
          active_reload->connection = session.attachment_runtime.connection_id;
          reset_command_line(session);
        }
        continue;
      }
      if (action.kind == CommandLineActionKind::hosted) {
        if (extensions.start(action.hosted_command, action.arguments,
                             {.session = session.id,
                              .tab = tab->id,
                              .pane = pane->id,
                              .connection = session.attachment_runtime.connection_id},
                             reactor_now())) {
          reset_command_line(session);
        } else {
          finish_command_line_error(session, "Error: Extension capacity reached");
        }
        continue;
      }
      if (action.kind == CommandLineActionKind::detach) {
        reset_command_line(session, false);
        handle_client_parse_result(session, runtimes, ParseResult::detach);
        continue;
      }
      if (action.kind == CommandLineActionKind::switch_session) {
        auto* const target = public_session(sessions, action.switch_session);
        const auto transferred = target == nullptr ? AttachmentTransferResult::unavailable
                                                   : transfer_attachment(session, *target, sessions,
                                                                         runtimes, activity_order);
        if (transferred == AttachmentTransferResult::transferred ||
            transferred == AttachmentTransferResult::deferred) {
          continue;
        }
        finish_command_line_error(session, command_line_message(transferred));
        continue;
      }
      const auto execution = PublicCommandExecutor::execute(action.command, sessions, runtimes,
                                                            activity_order, std::span<std::byte>{});
      handoff_public_focus(action.command, execution, sessions, surface_runtime);
      if (execution.status == CommandStatus::applied ||
          execution.status == CommandStatus::no_effect) {
        reset_command_line(session);
      } else {
        finish_command_line_error(session, command_line_message(execution.status));
      }
    } catch (...) {
      finish_command_line_error(session, "Error: Command failed");
    }
  }
}

[[nodiscard]] auto write_pane_pty(void* const context,
                                  const std::span<const std::byte> bytes) noexcept
    -> PtyWriteAttempt {
  auto& runtime = *static_cast<PaneRuntime*>(context);
  const auto written = ::write(runtime.pty, bytes.data(), bytes.size());
  if (written > 0) {
    const auto size = static_cast<std::size_t>(written);
    runtime.interactive_damage.record_write(size);
    std::uint64_t trace_correlation = 0;
#ifdef LEMMA_ENABLE_LATENCY_TRACE
    trace_correlation = runtime.input_trace_matcher.observe(bytes.first(size));
#endif
    diagnostic::record_latency_trace(diagnostic::LatencyTraceStage::daemon_pty_write_progress,
                                     static_cast<std::uint32_t>(runtime.pty),
                                     static_cast<std::uint64_t>(written), trace_correlation);
  }
  return {.bytes = written, .error = written < 0 ? errno : 0};
}

[[nodiscard]] auto flush_pane_writes(PaneRuntime& runtime, std::size_t& global_budget) noexcept
    -> bool {
  return flush_pty_write_queue(runtime.pending_writes, global_budget, &write_pane_pty, &runtime) !=
         PtyFlushStatus::hard_error;
}

struct PaneRuntimeOutcome final {
  PaneAddress pane;
  std::optional<ProcessExit> process_exit;
  PaneRuntimeFailure failure{PaneRuntimeFailure::terminal_integrity_error};
};

void apply_pane_runtime_outcome(SessionRecord& session, Tab& tab, PaneRuntimeStore& runtimes,
                                const PaneRuntimeOutcome& outcome) noexcept {
  LEMMA_ASSERT(outcome.pane.session == session.id);
  if (session.attachment.mouse_capture.has_value() &&
      session.attachment.mouse_capture->owner == MouseCaptureOwner::divider &&
      session.attachment.mouse_capture->target.tab == tab.id) {
    // Every committed divider position is already converged; consume the eventual stale release.
    finish_live_divider_resize(session, true);
  }
  auto* const pane = find_pane(session, tab, outcome.pane.pane);
  auto* const runtime = runtimes.get(outcome.pane);
  LEMMA_ASSERT(pane != nullptr && runtime != nullptr);
  if (session.attachment.selection_target ==
      std::optional{AttachmentPaneTarget{.tab = tab.id, .pane = pane->id}}) {
    leave_copy_mode(session, runtimes);
  }
  ProductionSessionRuntimeContext runtime_context{.session = &session, .runtimes = &runtimes};
  SessionMachine machine(session, production_session_options(runtime_context));
  const auto transition =
      machine.runtime_failed(pane->id, outcome.process_exit.value_or(ProcessExit{}),
                             outcome.failure == PaneRuntimeFailure::child_exit);
  apply_session_change(session, runtimes, transition.change);
}

// Removal may rewrite tab and pane ownership while traversing fixed Session pane slots.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void reclaim_dead_panes(SessionRecord& session, PaneRuntimeStore& runtimes,
                        PublicProcExecutions& executions) noexcept {
  for (std::size_t index = 0; index < session.panes.size() && session.active; ++index) {
    // index is bounded by the fixed Session pane capacity.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    auto& pane_owner = session.panes[index].pane;
    if (pane_owner == nullptr) {
      continue;
    }
    auto* const tab = find_tab(session, pane_owner->tab);
    LEMMA_ASSERT(tab != nullptr);
    auto* const runtime = find_pane_runtime(runtimes, session, *pane_owner);
    LEMMA_ASSERT(runtime != nullptr);
    if (!runtime->live()) {
      LEMMA_ASSERT(runtime->failure.has_value());
      const auto failure = *runtime->failure;
      if (failure == PaneRuntimeFailure::child_exit && !runtime->observed_exit.has_value() &&
          runtime->child > 0) {
        int status = 0;
        const auto reaped = ::waitpid(runtime->child, &status, WNOHANG);
        if (reaped == runtime->child) {
          runtime->observed_exit = process_exit_from_wait_status(status);
          runtime->child = -1;
        } else if (reaped == 0 || (reaped < 0 && errno == EINTR)) {
          // PTY EOF and wait status can become observable in adjacent reactor turns. Keep the
          // failed runtime unpublished until Core has the real outcome rather than committing a
          // transient unknown status.
          continue;
        }
      }
      const auto address = pane_address(session, *pane_owner);
      if (failure == PaneRuntimeFailure::child_exit && runtime->observed_exit.has_value()) {
        record_public_wait_exit(executions, session, pane_owner->id, *runtime->observed_exit);
      }
      apply_pane_runtime_outcome(
          session, *tab, runtimes,
          {.pane = address, .process_exit = runtime->observed_exit, .failure = failure});
    }
  }
}

void expire_status_messages(Sessions& sessions, const ReactorClock::time_point now) noexcept {
  for (auto& session : sessions) {
    if (session == nullptr || !session->active || !session->attachment.status_message_visible ||
        !session->attachment_runtime.status_message_deadline.has_value() ||
        now < *session->attachment_runtime.status_message_deadline) {
      continue;
    }
    clear_status_message(*session);
  }
}

// Collecting due panes traverses the fixed session hierarchy before the bounded fair work pass.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void run_due_scrollback_compression(Sessions& sessions, PaneRuntimeStore& runtimes,
                                    std::size_t& cursor) noexcept {
  constexpr std::size_t steps_per_turn_max = 8;
  const auto now = reactor_now();
  // Populate before incrementing count; only that prefix is visited, never unused slots.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  std::array<PaneRuntime*, static_cast<std::size_t>(limits::panes_hard_max)> due;
  std::size_t count = 0;
  for (auto& session : sessions) {
    if (session == nullptr || !session->active) {
      continue;
    }
    for (auto& pane_slot : session->panes) {
      if (pane_slot.pane == nullptr) {
        continue;
      }
      auto* const runtime = find_pane_runtime(runtimes, *session, *pane_slot.pane);
      LEMMA_ASSERT(runtime != nullptr);
      if (runtime->live() && runtime->compression_scheduled &&
          now >= runtime->compression_deadline) {
        std::span(due).subspan(count, 1).front() = runtime;
        ++count;
      }
    }
  }
  if (count == 0) {
    cursor = 0;
    return;
  }
  cursor %= count;
  const auto steps = std::min(count, steps_per_turn_max);
  for (std::size_t visited = 0; visited < steps; ++visited) {
    auto& runtime = *std::span(due).subspan((cursor + visited) % count, 1).front();
    const auto compressed = runtime.terminal.compress_scrollback();
    if (!compressed.has_value()) {
      runtime.fail(PaneRuntimeFailure::scrollback_compression_error);
      runtime.compression_scheduled = false;
      continue;
    }
    runtime.compression_scheduled = *compressed == vt::CompressionResult::pending;
    if (runtime.compression_scheduled) {
      runtime.compression_deadline = now + scrollback_compression_slice_delay;
    }
  }
  cursor = (cursor + steps) % count;
}

// Gate expiry visits the same fixed hierarchy and schedules only visible active-tab repairs.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void release_expired_presentation_gates(Sessions& sessions, PaneRuntimeStore& runtimes) noexcept {
  const auto now = reactor_now();
  for (auto& session : sessions) {
    if (session == nullptr || !session->active) {
      continue;
    }
    for (auto& pane_slot : session->panes) {
      if (pane_slot.pane == nullptr) {
        continue;
      }
      auto* const runtime = find_pane_runtime(runtimes, *session, *pane_slot.pane);
      LEMMA_ASSERT(runtime != nullptr);
      const auto released = runtime->presentation_gate.release_if_expired(now);
      if (released.urgent_render && pane_slot.pane->tab == session->active_tab) {
        schedule_frame(*session, FrameUrgency::state_change, released.force_full);
      }
    }
  }
}

struct ExtensionGeometryObservation final {
  AttachmentId attachment;
  TabId active_tab;
  std::uint64_t generation{0};
  std::uint16_t columns{0};
  std::uint16_t rows{0};
};

// Geometry reconciliation bridges retained Surface constraints and fallible Pane runtime effects.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void reconcile_extension_geometry(
    Sessions& sessions, PaneRuntimeStore& runtimes, extension::Runtime& extensions,
    std::array<ExtensionGeometryObservation, limits::sessions_hard_max>& observations) noexcept {
  for (auto& session : sessions) {
    if (session == nullptr || !session->active) {
      continue;
    }
    LEMMA_ASSERT(session->id.slot() < observations.size());
    auto& observed = std::span(observations).subspan(session->id.slot(), 1).front();
    const auto generation = extensions.geometry_generation(session->attachment.id);
    if (observed.attachment == session->attachment.id &&
        observed.active_tab == session->active_tab && observed.generation == generation &&
        observed.columns == session->attachment.columns &&
        observed.rows == session->attachment.rows) {
      continue;
    }
    const render::Viewport viewport{.columns = session->attachment.columns,
                                    .rows = pane_rows(session->attachment.rows)};
    if (!extensions.resize_surfaces(session->attachment.id, viewport)) {
      continue;
    }
    const auto content = extensions.pane_viewport(session->attachment.id, viewport);
    if (!content.has_value()) {
      continue;
    }
    auto* const tab = active_tab(*session);
    if (tab == nullptr) {
      continue;
    }
    if (tab->layout_column != content->column || tab->layout_row != content->row ||
        tab->layout_columns != content->columns || tab->layout_rows != content->rows) {
      ProductionSessionRuntimeContext runtime_context{.session = session.get(),
                                                      .runtimes = &runtimes};
      SessionMachine machine(*session, production_session_options(runtime_context));
      const auto transition = machine.resize_attachment(session->attachment.columns,
                                                        session->attachment.rows, *content);
      apply_session_change(*session, runtimes, transition.change);
      if (transition.result.status != CommandStatus::applied &&
          transition.result.status != CommandStatus::no_effect) {
        continue;
      }
    }
    schedule_frame(*session, FrameUrgency::state_change, true);
    observed = {.attachment = session->attachment.id,
                .active_tab = session->active_tab,
                .generation = generation,
                .columns = session->attachment.columns,
                .rows = session->attachment.rows};
  }
}

void queue_due_frames(Sessions& sessions, PaneRuntimeStore& runtimes,
                      extension::Runtime& extensions) noexcept {
  const auto now = reactor_now();
  for (auto& session : sessions) {
    if (session != nullptr && session->active && session->attachment_runtime.client >= 0 &&
        !session->attachment_runtime.output.busy() &&
        session->attachment_runtime.graphics.wake(now)) {
      schedule_frame(*session, FrameUrgency::burst, false);
    }
    if (session == nullptr || !session->active ||
        session->attachment_runtime.client_close_state != ConnectionCloseState::none ||
        !session->attachment_runtime.frame_scheduler.due(now, frame_sink_state(*session))) {
      continue;
    }
    if (!compose_session_frame(*session, runtimes, extensions,
                               session->attachment_runtime.frame_scheduler.force_full(), now)) {
      detach_attachment(*session, runtimes);
    }
    session->attachment_runtime.frame_scheduler.complete();
  }
}

[[nodiscard]] auto write_attached_client(void* const context,
                                         const std::span<const std::byte> bytes) noexcept
    -> ClientFrameWriteAttempt {
  auto& session = *static_cast<SessionRecord*>(context);
  const auto sent = reactor_send(session.attachment_runtime.client, bytes, MSG_NOSIGNAL);
  return {.bytes = sent.bytes, .error = sent.error};
}

void expire_attached_client_frames(Sessions& sessions, PaneRuntimeStore& runtimes,
                                   const ClientFrameOutput::TimePoint now) noexcept {
  for (auto& session : sessions) {
    if (session != nullptr && session->active && session->attachment_runtime.client >= 0 &&
        session->attachment_runtime.output.expired(now)) {
      detach_attachment(*session, runtimes);
    }
  }
}

// Fair bounded drains schedule independently staged clipboard/graphics continuations.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void flush_attached_client_frames(Sessions& sessions, PaneRuntimeStore& runtimes,
                                  const std::span<ClientFrameFlushTarget> storage,
                                  std::size_t& cursor,
                                  const ClientFrameOutput::TimePoint now) noexcept {
  std::size_t count = 0;
  for (auto& session : sessions) {
    if (session == nullptr || !session->active || session->attachment_runtime.client < 0) {
      continue;
    }
    LEMMA_ASSERT(count < storage.size());
    storage.subspan(count, 1).front() = {
        .descriptor = session->attachment_runtime.client,
        .frame = &session->attachment_runtime.frame,
        .output = &session->attachment_runtime.output,
        .write = &write_attached_client,
        .context = session.get(),
    };
    ++count;
  }

  std::size_t global_budget = attached_client_write_bytes_per_turn_max;
  auto active_targets = storage.first(count);
  flush_ready_client_frames(active_targets, cursor, global_budget, now);
  for (std::size_t index = 0; index < active_targets.size(); ++index) {
    auto& target = active_targets.subspan(index, 1).front();
    auto& session = *static_cast<SessionRecord*>(target.context);
    const auto status = target.status;
    if (status == ClientFrameFlushStatus::hard_error ||
        status == ClientFrameFlushStatus::deadline_exceeded ||
        (status == ClientFrameFlushStatus::drained &&
         session.attachment_runtime.client_close_state ==
             ConnectionCloseState::disconnect_queued)) {
      detach_attachment(session, runtimes);
    } else if (status == ClientFrameFlushStatus::drained) {
      if (session.attachment_runtime.clipboard_write.redraw_after_write) {
        session.attachment_runtime.clipboard_write.redraw_after_write = false;
        schedule_frame(session, FrameUrgency::state_change, true);
      } else if (session.attachment_runtime.graphics.pending() ||
                 session.attachment_runtime.clipboard_write.bytes != nullptr) {
        schedule_frame(session, FrameUrgency::burst, false);
      }
      queue_client_disconnect_if_ready(session, runtimes);
    }
  }
}

void expire_pending_connections(PendingConnections& pending_connections,
                                Sessions& sessions) noexcept {
  const auto now = reactor_now();
  for (std::size_t slot = 0; slot < pending_connections.size(); ++slot) {
    const auto& pending = std::span(pending_connections).subspan(slot, 1).front();
    if (pending != nullptr && pending->active() &&
        (!pending->public_connection || public_deadline_applies(*pending)) &&
        now >= pending->deadline) {
      close_pending(pending_connections, slot, sessions);
    }
  }
}

void expire_capacity_rejections(CapacityRejectionConnections& connections) noexcept {
  const auto now = reactor_now();
  for (std::size_t slot = 0; slot < connections.size(); ++slot) {
    const auto& connection = std::span(connections).subspan(slot, 1).front();
    if (connection.active() && now >= connection.deadline) {
      close_capacity_rejection(connections, slot);
    }
  }
}

[[nodiscard]] auto empty_pending_slot(PendingConnections& pending_connections,
                                      const PendingConnectionGenerations& generations) noexcept
    -> std::optional<std::size_t> {
  for (std::size_t slot = 0; slot < pending_connections.size(); ++slot) {
    if (std::span(pending_connections).subspan(slot, 1).front() == nullptr &&
        std::span(generations).subspan(slot, 1).front() <
            std::numeric_limits<std::uint32_t>::max()) {
      return slot;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto empty_capacity_rejection_slot(CapacityRejectionConnections& connections) noexcept
    -> std::optional<std::size_t> {
  for (std::size_t slot = 0; slot < connections.size(); ++slot) {
    if (!std::span(connections).subspan(slot, 1).front().active()) {
      return slot;
    }
  }
  return std::nullopt;
}

// Acceptance exhaustively handles primary slots, bounded rejection slots, and allocation failure.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void accept_pending_connections(const int listener, PendingConnections& pending_connections,
                                PendingConnectionGenerations& generations,
                                CapacityRejectionConnections& capacity_rejections) noexcept {
  constexpr std::size_t accepts_per_turn_max = 8;
  for (std::size_t accepted = 0; accepted < accepts_per_turn_max; ++accepted) {
    const auto available = empty_pending_slot(pending_connections, generations);
    std::optional<std::size_t> rejection_slot;
    if (!available.has_value()) {
      rejection_slot = empty_capacity_rejection_slot(capacity_rejections);
    }

    int connection = ::accept(listener, nullptr, nullptr);
    if (connection < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if (!set_nonblocking(connection)) {
      close_descriptor(connection);
      continue;
    }
    if (!available.has_value()) {
      if (!rejection_slot.has_value()) {
        // The bounded responder pool is reserved for peers whose protocol can still be identified.
        // Shed additional peers so silent saturated connections cannot gate listener service.
        close_descriptor(connection);
        continue;
      }
      auto& rejection = std::span(capacity_rejections).subspan(*rejection_slot, 1).front();
      rejection.descriptor = connection;
      rejection.output.reset();
      rejection.flush_response = false;
      rejection.deadline = reactor_now() + setup_progress_timeout;
      continue;
    }
    auto& owner = std::span(pending_connections).subspan(*available, 1).front();
    try {
      owner = std::make_unique<PendingConnection>();
    } catch (const std::bad_alloc&) {
      close_descriptor(connection);
      continue;
    }
    owner->descriptor = connection;
    auto& generation = std::span(generations).subspan(*available, 1).front();
    generation = next_generation(generation);
    owner->generation = generation;
    owner->slot = static_cast<std::uint32_t>(*available);
    const auto now = reactor_now();
    owner->deadline = now + setup_progress_timeout;
    owner->setup_deadline = now + setup_total_timeout;
    begin_pending_field(*owner, PendingState::read_command, 1);
  }
}

enum class DescriptorKind : std::uint8_t {
  child_reaper,
  pane,
  client,
  pending,
  capacity_rejection,
  extension_host,
  extension_peer,
  configuration_loader,
};

struct DescriptorOwner final {
  SessionId session;
  TabId tab;
  PaneId pane;
  ConnectionId connection;
  std::size_t auxiliary_slot{0};
  DescriptorKind kind{DescriptorKind::client};
  ExtensionGenerationId extension_owner;
};

template <typename Tag>
[[nodiscard]] auto readiness_id(const GenerationalId<Tag> id) noexcept -> std::uint64_t {
  return (static_cast<std::uint64_t>(id.generation()) << 32U) | id.slot();
}

[[nodiscard]] auto readiness_identity(const DescriptorOwner& owner,
                                      const PendingConnectionGenerations& pending) noexcept
    -> platform::ReadinessIdentity {
  const auto domain = static_cast<std::uint64_t>(owner.kind) + 2U;
  switch (owner.kind) {
  case DescriptorKind::pane:
    return {.domain = domain,
            .owner = readiness_id(owner.session),
            .generation = readiness_id(owner.pane)};
  case DescriptorKind::client:
    return {.domain = domain,
            .owner = readiness_id(owner.session),
            .generation = readiness_id(owner.connection)};
  case DescriptorKind::pending:
    return {.domain = domain,
            .owner = owner.auxiliary_slot,
            .generation = std::span(pending).subspan(owner.auxiliary_slot, 1).front()};
  case DescriptorKind::extension_peer:
    return {.domain = domain, .owner = readiness_id(owner.extension_owner), .generation = 0};
  case DescriptorKind::configuration_loader:
  case DescriptorKind::capacity_rejection:
    // Short-lived cold-path descriptors have no retained identity. Use ordinary poll this turn.
    return {};
  case DescriptorKind::child_reaper:
  case DescriptorKind::extension_host:
    return {.domain = domain, .owner = 0, .generation = 0};
  }
  return {};
}

[[nodiscard]] auto
collect_readiness_identities(const std::span<const DescriptorOwner> owners,
                             const PendingConnectionGenerations& pending,
                             std::vector<platform::ReadinessIdentity>& storage) noexcept
    -> std::span<const platform::ReadinessIdentity> {
  try {
    storage.resize(owners.size());
  } catch (...) {
    return {}; // Allocation failure selects poll for this turn.
  }
  storage.front() = {
      .domain = 1, .owner = 0, .generation = 0}; // Listener lifetime is the entire reactor run.
  for (std::size_t index = 1; index < owners.size(); ++index) {
    std::span(storage).subspan(index, 1).front() =
        readiness_identity(owners.subspan(index, 1).front(), pending);
  }
  return storage;
}

// Publication keeps old-owner cancellation, borrower transfer, and cleanup ordered in one turn.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void service_configuration_reload(ReactorEnvironment& environment, ReloadState& reload,
                                  Sessions& sessions, PaneRuntimeStore& runtimes,
                                  extension::CommandRuntime& commands, PublicProcExecutions& procs,
                                  PendingConnections& connections, extension::Runtime& extensions,
                                  const int listener, bool& service_extensions) noexcept {
  auto* const owner = environment.configuration_reloader;
  if (owner == nullptr || owner->descriptor() < 0) {
    return;
  }
  auto result = reload.current.lock();
  if (result == nullptr) {
    owner->discard(); // Its CONTROL/extension owner disconnected before publication.
    return;
  }
  const auto candidate = owner->advance(reactor_now());
  if (!candidate.has_value()) {
    return;
  }
  // A Proc can outlive its transport until the round-robin scheduler revisits it. Check its
  // authoritative completion owner at publication, not just the shared result's lifetime.
  if (result != reload.interactive && !std::ranges::any_of(procs, [&](const auto& slot) {
        const auto* const execution = slot.execution.get();
        if (execution == nullptr || execution->reload != result) {
          return false;
        }
        if (owned_public_proc(connections, *execution) != nullptr) {
          return true;
        }
        if (const auto* hosted = std::get_if<HostedProcOwner>(&execution->owner)) {
          const auto* const invocation = commands.find(hosted->invocation);
          return invocation != nullptr && invocation_attachment(sessions, *invocation) != nullptr;
        }
        const auto* const runtime = std::get_if<RuntimeProcOwner>(&execution->owner);
        return runtime != nullptr && extensions.connected(runtime->generation);
      })) {
    owner->discard();
    return;
  }
  result->error = candidate->error;
  try {
    result->diagnostic = candidate->diagnostic;
  } catch (...) {
    // Reactor/result boundary: diagnostic allocation failure rejects the untouched candidate.
    // The failed result retains its stable reason even when display text cannot be retained.
    owner->discard();
    result->complete = true;
    return;
  }
  if (candidate->generation == nullptr) {
    owner->discard();
  } else {
    // The old map and descriptors remain alive until every borrower has moved. Old command
    // invocations cannot address the new host's reused invocation IDs.
    fail_hosted_commands(commands, sessions, "Error: Command cancelled by config reload");
    for (auto& slot : procs) {
      if (slot.execution != nullptr &&
          std::holds_alternative<HostedProcOwner>(slot.execution->owner)) {
        slot.execution.reset();
      }
    }
    const auto& generation = *candidate->generation;
    for (auto& session : sessions) {
      if (session == nullptr) {
        continue;
      }
      leave_copy_mode(*session, runtimes);
      reset_rename_prompt(*session, false);
      reset_command_line(*session, false);
      leave_message_view(*session, false);
      session->input_router.reconfigure(generation.input_map());
      session->interaction_router.reconfigure(generation.input_map());
      session->attachment_runtime.hosted_command.reset();
      schedule_frame(*session, FrameUrgency::state_change, false);
    }
    environment.input_map = &generation.input_map();
    environment.scrollback_lines = generation.scrollback_lines();
    environment.clipboard_read = generation.clipboard_read();
    environment.clipboard_write = generation.clipboard_write();
    environment.default_program = generation.default_program();
    environment.default_cwd = generation.default_cwd();
    environment.command_history_file = generation.history_file();
    environment.extension_descriptor = candidate->descriptor;
    environment.extension_commands = candidate->commands;
    environment.stop_extension = candidate->stop;
    environment.extension_context = candidate->context;
    commands = extension::CommandRuntime(candidate->descriptor, candidate->commands,
                                         candidate->stop, candidate->context, listener);
    service_extensions = commands.channel().descriptor() >= 0;
    owner->commit();
    result->success = true;
  }
  result->complete = true;
  if (reload.interactive == result) {
    for (auto& session : sessions) {
      if (session != nullptr && session->attachment_runtime.connection_id == reload.connection) {
        publish_status_message(
            *session, result->success ? StatusMessageKind::information : StatusMessageKind::error,
            result->success ? "Configuration reloaded" : result->diagnostic);
      }
    }
    reload.interactive.reset();
  }
}

// The branches are the explicit bounded stages of the current single-owner reactor.
[[nodiscard]] auto
// NOLINTNEXTLINE(readability-function-cognitive-complexity,bugprone-exception-escape)
run_server_impl(const int listener, const EndpointRelease release_endpoint,
                void* const release_context, const StopRequested stop_requested,
                const ChildReaper child_reaper, ReactorEnvironment environment) noexcept -> int {
  diagnostic::set_latency_trace_role(diagnostic::LatencyTraceRole::daemon);
  if (!environment.valid()) {
    return 1;
  }
  ReloadState reload;
  const ReactorEnvironmentGuard environment_guard(environment, reload);
  CommandHistoryGuard command_history_guard(environment.command_history_file);
  EndpointReleaseGuard endpoint_release(release_endpoint, release_context);
  Sessions sessions;
  std::unique_ptr<PaneRuntimeStore> pane_runtimes;
  PublicScratch public_scratch;
  try {
    pane_runtimes = std::make_unique<PaneRuntimeStore>();
  } catch (...) {
    return 1;
  }
  auto& runtimes = *pane_runtimes;
  PendingConnections pending_connections;
  PendingConnectionGenerations pending_generations{};
  PublicProcExecutions public_procs{};
  extension::CommandRuntime extensions(environment.extension_descriptor,
                                       environment.extension_commands, environment.stop_extension,
                                       environment.extension_context, listener);
  extension::Runtime extension_runtime;
  bool service_extensions = !environment.extension_commands.empty();
  CapacityRejectionConnections capacity_rejections{};
  if (!child_reaper.valid() || !set_nonblocking(listener)) {
    return 1;
  }
  constexpr auto descriptor_count_max =
      std::size_t{4} + limits::panes_hard_max +
      static_cast<std::size_t>(limits::sessions_hard_max) + limits::pending_connections_hard_max +
      capacity_rejection_connections_max + limits::extension_sessions_hard_max;
  std::array<pollfd, descriptor_count_max> descriptors{};
  std::array<DescriptorOwner, descriptor_count_max> owners{};
  platform::Readiness readiness(environment.poll == &production_poll ? descriptor_count_max : 0);
  std::vector<platform::ReadinessIdentity> readiness_identities;
  std::array<ClientFrameFlushTarget, static_cast<std::size_t>(limits::sessions_hard_max)>
      client_flush_targets{};
  std::array<ExtensionGeometryObservation, limits::sessions_hard_max>
      extension_geometry_observations{};
  std::array<ExtensionObservation, limits::extension_sessions_hard_max> extension_observations{};
  std::size_t pty_read_cursor = 0;
  std::size_t pty_flush_cursor = 0;
  std::size_t client_flush_cursor = 0;
  std::size_t search_cursor = 0;
  std::size_t compression_cursor = 0;
  std::size_t proc_cursor = 0;
  std::size_t observer_cursor = 0;
  std::size_t extension_observer_cursor = 0;
  std::size_t extension_io_cursor = 0;
  std::uint64_t activity_order = 0;
  bool owned_a_session = false;
  bool reaped_work_pending = false;

  while (true) {
    if (stop_requested != nullptr && stop_requested()) {
      return 0;
    }
    // Reaping may consume the only wakeup after PTY EOF. Run retirement and waiting Procs
    // before sleeping again so the recorded exit becomes a published Core outcome.
    reaped_work_pending =
        reap_exited_children(sessions, runtimes, child_reaper) || reaped_work_pending;
    expire_status_messages(sessions, reactor_now());
    const bool public_screen_work_pending = service_public_observers(
        pending_connections, sessions, runtimes, public_scratch, observer_cursor);
    service_extension_observers(extension_runtime, sessions, runtimes, public_scratch,
                                extension_observations, extension_observer_cursor);
    for (std::size_t slot = 0; slot < pending_connections.size(); ++slot) {
      const auto& pending = std::span(pending_connections).subspan(slot, 1).front();
      if (pending != nullptr && !pending->active()) {
        close_pending(pending_connections, slot, sessions);
      }
    }
    // Detach, Pane exit, and queue backpressure change focus outside command dispatch. Queue
    // their reports before descriptor interest is collected so the writes are polled this turn.
    for (auto& session : sessions) {
      if (session != nullptr) {
        reconcile_focus_reports(*session, runtimes);
      }
    }
    std::size_t descriptor_count = 2;
    descriptors.front() = {.fd = listener, .events = POLLIN, .revents = 0};
    std::span(descriptors).subspan(1, 1).front() = {
        .fd = child_reaper.wake_descriptor, .events = POLLIN, .revents = 0};
    std::span(owners).subspan(1, 1).front() = {.session = {},
                                               .tab = {},
                                               .pane = {},
                                               .connection = {},
                                               .auxiliary_slot = 0,
                                               .kind = DescriptorKind::child_reaper,
                                               .extension_owner = {}};
    for (const auto& session : sessions) {
      if (session == nullptr || !session->active) {
        continue;
      }
      for (const auto& pane_slot : session->panes) {
        const auto& pane = pane_slot.pane;
        if (pane == nullptr) {
          continue;
        }
        const auto address = pane_address(*session, *pane);
        const auto* const runtime = runtimes.get(address);
        LEMMA_ASSERT(runtime != nullptr);
        if (!runtime->pollable()) {
          continue;
        }
        const auto pane_events = static_cast<short>(
            POLLIN | (!runtime->pending_writes.empty() ? static_cast<short>(POLLOUT) : 0));
        std::span(descriptors).subspan(descriptor_count, 1).front() = {
            .fd = runtime->pty, .events = pane_events, .revents = 0};
        std::span(owners).subspan(descriptor_count, 1).front() = {.session = address.session,
                                                                  .tab = pane->tab,
                                                                  .pane = address.pane,
                                                                  .connection = {},
                                                                  .auxiliary_slot = 0,
                                                                  .kind = DescriptorKind::pane,
                                                                  .extension_owner = {}};
        ++descriptor_count;
      }
      if (session->attachment_runtime.client >= 0) {
        const auto client_events = static_cast<short>(
            (session->attachment_runtime.input_backpressured ||
                     session->attachment_runtime.client_work_pending ||
                     session->attachment_runtime.client_close_state != ConnectionCloseState::none ||
                     session->attachment.command_line.submit_requested ||
                     session->attachment.command_line.completion_requested
                 ? 0
                 : POLLIN) |
            (session->attachment_runtime.output.busy() ? static_cast<short>(POLLOUT) : 0));
        std::span(descriptors).subspan(descriptor_count, 1).front() = {
            .fd = session->attachment_runtime.client, .events = client_events, .revents = 0};
        std::span(owners).subspan(descriptor_count, 1).front() = {
            .session = session->id,
            .tab = {},
            .pane = {},
            .connection = session->attachment_runtime.connection_id,
            .auxiliary_slot = 0,
            .kind = DescriptorKind::client,
            .extension_owner = {}};
        ++descriptor_count;
      }
    }
    for (std::size_t slot = 0; slot < pending_connections.size(); ++slot) {
      const auto& pending = std::span(pending_connections).subspan(slot, 1).front();
      if (pending == nullptr || !pending->active()) {
        continue;
      }
      short events = 0;
      if (pending->state == PendingState::flush_response) {
        events = POLLOUT;
      } else if (pending->state != PendingState::prepare_public_observer) {
        events = POLLIN;
      }
      std::span(descriptors).subspan(descriptor_count, 1).front() = {
          .fd = pending->descriptor, .events = events, .revents = 0};
      std::span(owners).subspan(descriptor_count, 1).front() = {.session = {},
                                                                .tab = {},
                                                                .pane = {},
                                                                .connection = {},
                                                                .auxiliary_slot = slot,
                                                                .kind = DescriptorKind::pending,
                                                                .extension_owner = {}};
      ++descriptor_count;
    }
    for (std::size_t slot = 0; slot < capacity_rejections.size(); ++slot) {
      const auto& rejection = std::span(capacity_rejections).subspan(slot, 1).front();
      if (!rejection.active()) {
        continue;
      }
      const auto events = static_cast<short>(rejection.flush_response ? POLLOUT : POLLIN);
      std::span(descriptors).subspan(descriptor_count, 1).front() = {
          .fd = rejection.descriptor, .events = events, .revents = 0};
      std::span(owners).subspan(descriptor_count, 1).front() = {
          .session = {},
          .tab = {},
          .pane = {},
          .connection = {},
          .auxiliary_slot = slot,
          .kind = DescriptorKind::capacity_rejection,
          .extension_owner = {}};
      ++descriptor_count;
    }
    std::array<extension::PeerView, limits::extension_sessions_hard_max> extension_peers{};
    const auto active_extension_peers = extension_runtime.peer_views(extension_peers);
    if (!active_extension_peers.empty()) {
      extension_io_cursor %= active_extension_peers.size();
    }
    for (std::size_t visited = 0; visited < active_extension_peers.size(); ++visited) {
      const auto peer =
          active_extension_peers
              .subspan((extension_io_cursor + visited) % active_extension_peers.size(), 1)
              .front();
      std::span(descriptors).subspan(descriptor_count, 1).front() = {
          .fd = peer.descriptor, .events = peer.events, .revents = 0};
      std::span(owners).subspan(descriptor_count, 1).front() = {.session = {},
                                                                .tab = {},
                                                                .pane = {},
                                                                .connection = {},
                                                                .auxiliary_slot = peer.slot,
                                                                .kind =
                                                                    DescriptorKind::extension_peer,
                                                                .extension_owner = peer.owner};
      ++descriptor_count;
    }
    if (!active_extension_peers.empty()) {
      // Advance by one so a continuously readable peer cannot repeatedly own the global byte
      // budget when the active peer count shares a factor with the per-peer record allowance.
      extension_io_cursor = (extension_io_cursor + 1U) % active_extension_peers.size();
    }
    if (extensions.channel().descriptor() >= 0) {
      std::span(descriptors).subspan(descriptor_count, 1).front() = {
          .fd = extensions.channel().descriptor(),
          .events = extensions.channel().events(),
          .revents = 0};
      std::span(owners).subspan(descriptor_count, 1).front() = {.session = {},
                                                                .tab = {},
                                                                .pane = {},
                                                                .connection = {},
                                                                .auxiliary_slot = 0,
                                                                .kind =
                                                                    DescriptorKind::extension_host,
                                                                .extension_owner = {}};
      ++descriptor_count;
    }
    auto* const reloader = environment.configuration_reloader;
    if (reloader != nullptr && reloader->descriptor() >= 0) {
      std::span(descriptors).subspan(descriptor_count, 1).front() = {
          .fd = reloader->descriptor(), .events = POLLIN, .revents = 0};
      std::span(owners).subspan(descriptor_count, 1).front() = {
          .session = {},
          .tab = {},
          .pane = {},
          .connection = {},
          .kind = DescriptorKind::configuration_loader,
          .extension_owner = {}};
      ++descriptor_count;
    }
    const auto timeout =
        poll_timeout(sessions, runtimes, pending_connections, public_procs, capacity_rejections,
                     public_screen_work_pending || reaped_work_pending);
    const auto hosted_timeout =
        service_extensions ? extensions.poll_timeout(timeout, reactor_now()) : timeout;
    const auto ready_descriptors = std::span(descriptors).first(descriptor_count);
    auto ready_timeout = extension_runtime.buffered_work() ? 0 : hosted_timeout;
    if (reloader != nullptr && reloader->descriptor() >= 0) {
      const auto remaining =
          std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                                        reloader->deadline() - reactor_now())
                                        .count());
      ready_timeout = ready_timeout < 0 ? static_cast<int>(remaining)
                                        : std::min(ready_timeout, static_cast<int>(remaining));
    }
    int poll_result = 0;
    if (readiness.uses_native_wait()) {
      const auto identities = collect_readiness_identities(
          std::span(owners).first(descriptor_count), pending_generations, readiness_identities);
      poll_result = readiness.wait(ready_descriptors, identities, ready_timeout);
    } else {
      poll_result = reactor_poll(ready_descriptors, ready_timeout);
    }
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 1;
    }
    const auto child_reaper_events = std::span(descriptors).subspan(1, 1).front().revents;
    if ((child_reaper_events & (POLLIN | POLLHUP | POLLERR)) != 0) {
      static_cast<void>(reap_exited_children(sessions, runtimes, child_reaper));
    }
    expire_attached_client_frames(sessions, runtimes, reactor_now());
    expire_status_messages(sessions, reactor_now());

    // Drain every ready PTY before handling client input, then remove exited panes so input is
    // always routed to a live focused pane selected by close_pane.
    std::size_t pty_read_budget = std::size_t{256} * 1'024U;
    std::array<std::size_t, static_cast<std::size_t>(limits::sessions_hard_max)>
        blocked_session_read_budgets{};
    blocked_session_read_budgets.fill(blocked_sink_pty_read_bytes_per_turn_max);
    const auto ready_owner_count = descriptor_count - 2U;
    if (ready_owner_count > 0) {
      pty_read_cursor %= ready_owner_count;
      std::size_t visited = 0;
      for (; visited < ready_owner_count && pty_read_budget > 0; ++visited) {
        const auto index = 2U + ((pty_read_cursor + visited) % ready_owner_count);
        const auto owner = std::span(owners).subspan(index, 1).front();
        if (owner.kind == DescriptorKind::pane) {
          auto* const session = sessions.get(owner.session);
          auto* const tab = session == nullptr ? nullptr : find_tab(*session, owner.tab);
          auto* const pane = tab == nullptr ? nullptr : find_pane(*session, *tab, owner.pane);
          auto* const runtime = runtimes.get({.session = owner.session, .pane = owner.pane});
          LEMMA_ASSERT(session != nullptr && tab != nullptr && pane != nullptr &&
                       runtime != nullptr);
          const auto& events = std::span(descriptors).subspan(index, 1).front();
          auto& blocked_session_budget =
              std::span(blocked_session_read_budgets).subspan(owner.session.slot(), 1).front();
          process_pane_events(*session, *tab, *pane, *runtime, runtimes, events, pty_read_budget,
                              blocked_session_budget);
        }
      }
      pty_read_cursor = (pty_read_cursor + visited) % ready_owner_count;
    } else {
      pty_read_cursor = 0;
    }
    for (auto& session : sessions) {
      if (session != nullptr && session->active) {
        reclaim_dead_panes(*session, runtimes, public_procs);
        service_copy_input_timeout(*session, runtimes, reactor_now());
      }
    }
    // Clear only after retirement; an interrupted poll must retain the pending native turn.
    reaped_work_pending = false;
    std::array<std::size_t, static_cast<std::size_t>(limits::sessions_hard_max)>
        client_message_budgets{};
    std::array<std::size_t, static_cast<std::size_t>(limits::sessions_hard_max)>
        client_geometry_budgets{};
    std::array<std::size_t, static_cast<std::size_t>(limits::sessions_hard_max)>
        client_input_budgets{};
    client_message_budgets.fill(client_messages_per_turn_max);
    client_geometry_budgets.fill(client_geometry_messages_per_turn_max);
    client_input_budgets.fill(client_input_steps_per_turn_max);
    for (std::size_t index = 1; index < descriptor_count; ++index) {
      const auto owner = std::span(owners).subspan(index, 1).front();
      if (owner.kind == DescriptorKind::client) {
        auto* const session = sessions.get(owner.session);
        if (session == nullptr || !session->active ||
            session->attachment_runtime.connection_id != owner.connection) {
          continue;
        }
        const auto& events = std::span(descriptors).subspan(index, 1).front();
        if ((events.revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
          session->attachment_runtime.output.mark_write_ready();
        }
        auto& message_budget =
            std::span(client_message_budgets).subspan(session->id.slot(), 1).front();
        auto& geometry_budget =
            std::span(client_geometry_budgets).subspan(session->id.slot(), 1).front();
        auto& input_budget = std::span(client_input_budgets).subspan(session->id.slot(), 1).front();
        process_client_events(*session, runtimes, extension_runtime, events, message_budget,
                              geometry_budget, input_budget, &session_name_conflict, &sessions);
      }
    }
    service_attachment_command_lines(sessions, runtimes, activity_order, extensions,
                                     extension_runtime);
    std::array<SessionRecord*, static_cast<std::size_t>(limits::sessions_hard_max)>
        search_sessions{};
    for (auto& session : sessions) {
      if (session != nullptr && session->active) {
        std::span(search_sessions).subspan(session->id.slot(), 1).front() = session.get();
      }
    }
    std::size_t search_work_budget = limits::search_candidates_per_step;
    std::size_t visited = 0;
    for (; visited < search_sessions.size() && search_work_budget > 0; ++visited) {
      auto* const session = std::span(search_sessions)
                                .subspan((search_cursor + visited) % search_sessions.size(), 1)
                                .front();
      if (session != nullptr) {
        static_cast<void>(service_copy_search(*session, runtimes, search_work_budget));
      }
    }
    search_cursor = (search_cursor + visited) % search_sessions.size();
    std::size_t extension_read_budget = limits::extension_bytes_per_turn_max;
    std::size_t extension_record_budget = limits::extension_records_per_turn_max;
    std::size_t extension_record_work_budget = limits::extension_record_work_bytes_per_turn_max;
    std::size_t extension_output_budget = limits::extension_output_bytes_per_turn_max;
    for (std::size_t index = 1; index < descriptor_count; ++index) {
      const auto owner = std::span(owners).subspan(index, 1).front();
      if (owner.kind == DescriptorKind::pending) {
        const auto& pending =
            std::span(pending_connections).subspan(owner.auxiliary_slot, 1).front();
        if (pending == nullptr || !pending->active()) {
          continue;
        }
        const auto events = std::span(descriptors).subspan(index, 1).front().revents;
        const bool execution_pending = pending->state == PendingState::execute_public_proc;
        if (execution_pending && (events & (POLLIN | POLLHUP | POLLERR)) != 0) {
          process_public_read(*pending, public_procs);
          if (pending->state == PendingState::unused) {
            close_pending(pending_connections, owner.auxiliary_slot, sessions);
          }
        } else if (pending->state != PendingState::flush_response &&
                   pending->state != PendingState::prepare_public_observer &&
                   (events & (POLLIN | POLLHUP | POLLERR)) != 0) {
          process_pending_read(pending_connections, sessions, runtimes, activity_order,
                               public_procs, extension_runtime, extension_observations,
                               public_scratch, owner.auxiliary_slot);
        }
      } else if (owner.kind == DescriptorKind::extension_peer) {
        const auto events = std::span(descriptors).subspan(index, 1).front().revents;
        // Sleeping extensions have no reactor work. Retained complete records still run when
        // a previous turn exhausted its byte/record budget, even without fresh socket readiness.
        if (events == 0 &&
            !extension_runtime.buffered_record_bytes(owner.auxiliary_slot).has_value()) {
          continue;
        }
        if (extension_runtime.owner_at(owner.auxiliary_slot) != owner.extension_owner ||
            !extension_runtime.connected(owner.extension_owner)) {
          continue;
        }
        if ((events & POLLIN) != 0 && extension_read_budget > 0) {
          const auto received = extension_runtime.read_ready(
              owner.auxiliary_slot,
              std::min(limits::extension_io_bytes_per_turn_max, extension_read_budget));
          extension_read_budget -= std::min(received, extension_read_budget);
        }
        constexpr std::size_t records_per_peer_per_turn_max = 4;
        for (std::size_t processed = 0;
             processed < records_per_peer_per_turn_max && extension_record_budget > 0 &&
             process_extension_record(extension_runtime, public_procs, sessions,
                                      owner.auxiliary_slot, extension_record_work_budget);
             ++processed) {
          --extension_record_budget;
        }
        if ((events & POLLOUT) != 0 && extension_output_budget > 0) {
          const auto written = extension_runtime.write_ready(
              owner.auxiliary_slot,
              std::min(limits::extension_io_bytes_per_turn_max, extension_output_budget));
          extension_output_budget -= std::min(written, extension_output_budget);
        }
        if ((events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
          const auto generation = extension_runtime.owner_at(owner.auxiliary_slot);
          if (generation.is_valid()) {
            static_cast<void>(extension_runtime.disconnect(generation));
          }
        }
      } else if (owner.kind == DescriptorKind::extension_host) {
        const auto events = std::span(descriptors).subspan(index, 1).front().revents;
        if ((events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
          extensions.channel().disconnect();
        } else {
          if ((events & POLLIN) != 0) {
            extensions.channel().read_ready();
          }
          if ((events & POLLOUT) != 0) {
            extensions.channel().write_ready();
          }
        }
      } else if (owner.kind == DescriptorKind::capacity_rejection) {
        const auto& rejection =
            std::span(capacity_rejections).subspan(owner.auxiliary_slot, 1).front();
        const auto events = std::span(descriptors).subspan(index, 1).front().revents;
        if (rejection.active() && !rejection.flush_response &&
            (events & (POLLIN | POLLHUP | POLLERR)) != 0) {
          process_capacity_rejection_read(capacity_rejections, owner.auxiliary_slot);
        }
      }
    }
    if (service_extensions) {
      service_hosted_commands(extensions, sessions, public_procs);
      service_extensions = extensions.channel().descriptor() >= 0;
    }
    service_public_procs(public_procs, pending_connections, sessions, runtimes, activity_order,
                         public_scratch, proc_cursor, extensions, extension_runtime);
    service_configuration_reload(environment, reload, sessions, runtimes, extensions, public_procs,
                                 pending_connections, extension_runtime, listener,
                                 service_extensions);
    std::array<AttachmentId, limits::extension_sessions_hard_max> affected_attachments{};
    static_cast<void>(extension_runtime.reap_disconnected(affected_attachments));

    // Writes are attempted only from retained queue bytes and are bounded both per pane and across
    // this turn. A hard descriptor error retires the pane; EAGAIN leaves all bytes queued.
    std::size_t pty_write_budget = std::size_t{1} * 1'024U * 1'024U;
    // Only fully assigned entries below writable_pane_count are consumed.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    std::array<PaneRuntime*, static_cast<std::size_t>(limits::panes_hard_max)> writable_panes;
    std::size_t writable_pane_count = 0;
    for (auto& session : sessions) {
      if (session == nullptr || !session->active) {
        continue;
      }
      service_clipboard(*session, runtimes);
      service_interactive_paste(*session, sessions, runtimes, activity_order);
      for (auto& pane_slot : session->panes) {
        if (pane_slot.pane == nullptr) {
          continue;
        }
        auto* const runtime = find_pane_runtime(runtimes, *session, *pane_slot.pane);
        LEMMA_ASSERT(runtime != nullptr);
        if (runtime->pollable() && !runtime->pending_writes.empty()) {
          std::span(writable_panes).subspan(writable_pane_count, 1).front() = runtime;
          ++writable_pane_count;
        }
      }
    }
    if (writable_pane_count > 0) {
      pty_flush_cursor %= writable_pane_count;
      std::size_t writable_visited = 0;
      for (; writable_visited < writable_pane_count && pty_write_budget > 0; ++writable_visited) {
        const auto index = (pty_flush_cursor + writable_visited) % writable_pane_count;
        auto& runtime = *std::span(writable_panes).subspan(index, 1).front();
        if (!flush_pane_writes(runtime, pty_write_budget)) {
          runtime.fail(PaneRuntimeFailure::pty_write_error);
        }
      }
      pty_flush_cursor = (pty_flush_cursor + writable_visited) % writable_pane_count;
    } else {
      pty_flush_cursor = 0;
    }
    for (auto& session : sessions) {
      if (session != nullptr && session->active) {
        reclaim_dead_panes(*session, runtimes, public_procs);
      }
    }
    // Capacity may have become available without new client socket readiness.
    const pollfd no_events{.fd = -1, .events = 0, .revents = 0};
    for (auto& session : sessions) {
      if (session != nullptr && session->active && session->attachment_runtime.client >= 0 &&
          session->attachment_runtime.input_backpressured) {
        auto& message_budget =
            std::span(client_message_budgets).subspan(session->id.slot(), 1).front();
        auto& geometry_budget =
            std::span(client_geometry_budgets).subspan(session->id.slot(), 1).front();
        auto& input_budget = std::span(client_input_budgets).subspan(session->id.slot(), 1).front();
        process_client_events(*session, runtimes, extension_runtime, no_events, message_budget,
                              geometry_budget, input_budget, &session_name_conflict, &sessions);
      }
    }

    std::size_t pending_output_budget = std::size_t{256} * 1'024U;
    bool shutdown_after_outputs = false;
    for (std::size_t index = 1; index < descriptor_count; ++index) {
      const auto owner = std::span(owners).subspan(index, 1).front();
      const auto events = std::span(descriptors).subspan(index, 1).front().revents;
      if (owner.kind == DescriptorKind::pending) {
        const auto& pending =
            std::span(pending_connections).subspan(owner.auxiliary_slot, 1).front();
        if (pending == nullptr || !pending->active() ||
            pending->state != PendingState::flush_response) {
          continue;
        }
        if ((events & (POLLOUT | POLLHUP | POLLERR)) != 0 &&
            flush_pending_output(pending_connections, owner.auxiliary_slot, pending_output_budget,
                                 sessions, runtimes, extension_runtime, activity_order)) {
          shutdown_after_outputs = true;
          break;
        }
      } else if (owner.kind == DescriptorKind::capacity_rejection) {
        const auto& rejection =
            std::span(capacity_rejections).subspan(owner.auxiliary_slot, 1).front();
        if (rejection.active() && rejection.flush_response &&
            (events & (POLLOUT | POLLHUP | POLLERR)) != 0) {
          flush_capacity_rejection_output(capacity_rejections, owner.auxiliary_slot,
                                          pending_output_budget);
        }
      }
    }

    run_due_scrollback_compression(sessions, runtimes, compression_cursor);
    release_expired_presentation_gates(sessions, runtimes);
    reconcile_extension_geometry(sessions, runtimes, extension_runtime,
                                 extension_geometry_observations);
    queue_due_frames(sessions, runtimes, extension_runtime);
    // Attached frame writes are core-owned, daemon-wide bounded, and round-robin fair. Newly
    // composed and newly handed-off attach frames get one immediate attempt; a blocked frame is
    // retried only after poll reports write readiness or its progress deadline expires.
    flush_attached_client_frames(sessions, runtimes, client_flush_targets, client_flush_cursor,
                                 reactor_now());
    if (shutdown_after_outputs) {
      return 0;
    }
    expire_pending_connections(pending_connections, sessions);
    expire_capacity_rejections(capacity_rejections);
    owned_a_session = owned_a_session || sessions.size() > 0;
    reclaim_inactive_sessions(sessions, runtimes, extension_runtime);
    if (owned_a_session && sessions.size() == 0) {
      for (std::size_t slot = 0; slot < pending_connections.size(); ++slot) {
        const auto& pending = std::span(pending_connections).subspan(slot, 1).front();
        if (pending != nullptr && pending->public_connection &&
            pending->state != PendingState::flush_response) {
          close_pending(pending_connections, slot, sessions);
        }
      }
    }
    const bool pending_control =
        std::ranges::any_of(pending_connections, [](const auto& connection) {
          return connection != nullptr && connection->active();
        });
    if (owned_a_session && sessions.size() == 0 && !pending_control) {
      return 0;
    }

    if ((descriptors.front().revents & POLLIN) != 0) {
      accept_pending_connections(listener, pending_connections, pending_generations,
                                 capacity_rejections);
    }
    reclaim_inactive_sessions(sessions, runtimes, extension_runtime);
  }
}

} // namespace engine_detail

[[nodiscard]] auto production_reactor_environment() noexcept -> ReactorEnvironment {
  return {.context = nullptr,
          .poll = &engine_detail::production_poll,
          .now = &engine_detail::production_now,
          .send = &engine_detail::production_send,
          .input_map = nullptr,
          .scrollback_lines = std::nullopt,
          .default_program = {},
          .default_cwd = {},
          .command_history_file = {},
          .status_line = true,
          .extension_commands = {}};
}

[[nodiscard]] auto
run_server_with_environment(const int listener, const EndpointRelease release_endpoint,
                            void* const release_context, const StopRequested stop_requested,
                            const ChildReaper child_reaper,
                            const ReactorEnvironment environment) noexcept -> int {
  return engine_detail::run_server_impl(listener, release_endpoint, release_context, stop_requested,
                                        child_reaper, environment);
}

[[nodiscard]] auto run_server(const int listener, const EndpointRelease release_endpoint,
                              void* const release_context, const StopRequested stop_requested,
                              const ChildReaper child_reaper) noexcept -> int {
  return run_server_with_environment(listener, release_endpoint, release_context, stop_requested,
                                     child_reaper, production_reactor_environment());
}

} // namespace lemma::core
