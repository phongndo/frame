#include "api/command.hpp"
#include "core/client_frame_output.hpp"
#include "extension/protocol.hpp"
#include "extension/runtime.hpp"
#include "input/input_router.hpp"
#include "lemma/id.hpp"
#include "lemma/terminal/terminal.hpp"
#include "platform/readiness.hpp"
#include "render/frame_buffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

namespace {

std::atomic<bool> audit_enabled{false};
std::atomic<std::size_t> audited_allocations{0};
std::atomic<std::size_t> audited_bytes{0};

void record_allocation(const std::size_t bytes) noexcept {
  if (audit_enabled.load(std::memory_order_relaxed)) {
    audited_allocations.fetch_add(1, std::memory_order_relaxed);
    audited_bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

[[nodiscard]] auto allocate_unaligned(const std::size_t bytes) -> void* {
  const auto requested = bytes == 0 ? std::size_t{1} : bytes;
  // Global allocation replacement must delegate below the C++ allocation layer.
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
  void* const storage = std::malloc(requested);
  if (storage == nullptr) {
    throw std::bad_alloc{};
  }
  record_allocation(requested);
  return storage;
}

[[nodiscard]] auto allocate_aligned(const std::size_t bytes, const std::size_t alignment) -> void* {
  const auto requested = bytes == 0 ? std::size_t{1} : bytes;
  void* storage = nullptr;
  if (::posix_memalign(&storage, alignment, requested) != 0) {
    throw std::bad_alloc{};
  }
  record_allocation(requested);
  return storage;
}

struct WriteAudit final {
  std::size_t bytes{0};
  std::size_t attempts{0};
  std::size_t maximum_attempt_bytes{0};
};

[[nodiscard]] auto audited_write(void* const context,
                                 const std::span<const std::byte> bytes) noexcept
    -> lemma::core::ClientFrameWriteAttempt {
  auto& audit = *static_cast<WriteAudit*>(context);
  audit.bytes += bytes.size();
  ++audit.attempts;
  audit.maximum_attempt_bytes = std::max(audit.maximum_attempt_bytes, bytes.size());
  return {.bytes = static_cast<std::ptrdiff_t>(bytes.size())};
}

[[nodiscard]] auto write_and_compose(lemma::vt::Terminal& terminal,
                                     lemma::render::FrameBuffer& frame,
                                     const std::string_view text) noexcept -> std::size_t {
  const auto input = std::as_bytes(std::span(text.data(), text.size()));
  const auto damage = terminal.write_and_report_damage(input);
  if (!damage.has_value()) {
    return 0;
  }
  const auto composition = lemma::render::compose_retained_single_pane(terminal, frame, false);
  return composition.has_value() ? composition->bytes : 0;
}

// Alternates width and height so audited resizes exercise both the presented cell-hash shadow and
// the row-hash storage at capacities retained from warmup.
[[nodiscard]] constexpr auto audited_resize(const std::size_t iteration) noexcept
    -> lemma::vt::TerminalSize {
  return iteration % 2U == 0 ? lemma::vt::TerminalSize{.columns = 100, .rows = 24}
                             : lemma::vt::TerminalSize{.columns = 80, .rows = 30};
}

} // namespace

// Define every replaceable throwing allocation and deallocation form. ASan observes sized
// deallocation before runtime delegation, so each form must pair directly with the malloc-backed
// audit allocator. This target enables the compiler's sized-deallocation language mode.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,cppcoreguidelines-no-malloc,readability-identifier-naming)
void* operator new(std::size_t __sz) { return allocate_unaligned(__sz); }
void* operator new[](std::size_t __sz) { return allocate_unaligned(__sz); }
void* operator new(std::size_t __sz, std::align_val_t alignment) {
  return allocate_aligned(__sz, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t __sz, std::align_val_t alignment) {
  return allocate_aligned(__sz, static_cast<std::size_t>(alignment));
}
void operator delete(void* __p) noexcept { std::free(__p); }
void operator delete[](void* __p) noexcept { std::free(__p); }
void operator delete(void* __p, std::size_t /*size*/) noexcept { std::free(__p); }
void operator delete[](void* __p, std::size_t /*size*/) noexcept { std::free(__p); }
void operator delete(void* __p, std::align_val_t /*alignment*/) noexcept { std::free(__p); }
void operator delete[](void* __p, std::align_val_t /*alignment*/) noexcept { std::free(__p); }
void operator delete(void* __p, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  std::free(__p);
}
void operator delete[](void* __p, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
  std::free(__p);
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,cppcoreguidelines-no-malloc,readability-identifier-naming)

// The explicit failure branches make audit setup and each measured operation independently visible;
// output formatting runs only after measurement is disabled and can report allocation failure.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
int main() {
  constexpr std::size_t warmup_iterations = 256;
  constexpr std::size_t audited_iterations = 10'000;
  constexpr std::string_view first = "\x1B[12;1H\x1B[1;32msteady-state alpha \xE2\x98\x83\x1B[0m";
  constexpr std::string_view second = "\x1B[12;1H\x1B[1;34msteady-state beta  \xE2\x98\x83\x1B[0m";

  lemma::input::InputRouter input_router(lemma::input::default_input_map());
  auto extension_runtime = std::make_unique<lemma::extension::Runtime>();
  std::array<int, 2> extension_sockets{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, extension_sockets.data()) != 0) {
    return 2;
  }
  const auto extension_session = lemma::SessionId::from_parts(0, 1);
  const auto extension_attachment = lemma::AttachmentId::from_parts(0, 1);
  const auto extension_owner = extension_runtime->admit(
      lemma::extension::FramedPeer(std::exchange(extension_sockets.front(), -1)),
      lemma::extension::Hello{.name = "allocation-audit",
                              .subscription = {},
                              .capabilities = lemma::extension::capability_surface},
      extension_session, extension_attachment, {});
  if (!extension_owner.has_value()) {
    return 2;
  }
  const auto extension_surface = extension_runtime->create_surface(
      *extension_owner,
      lemma::api::SurfacePlacement{.kind = lemma::api::SurfacePlacementKind::overlay,
                                   .column = 0,
                                   .row = 0,
                                   .columns = 8,
                                   .rows = 1},
      true, true, {.columns = 80, .rows = 24});
  if (extension_surface.status != lemma::extension::SurfaceOperationStatus::applied) {
    return 2;
  }
  const auto surface_update = std::string(R"({"schema":"lemma.surface-update/v1","surface":")") +
                              std::to_string(extension_surface.surface.slot()) + ":" +
                              std::to_string(extension_surface.surface.generation()) +
                              R"(","rows":[{"row":0,"runs":[{"column":0,"text":"idle"}]}]})";
  if (extension_runtime
          ->apply_surface_update(*extension_owner, std::as_bytes(std::span(surface_update)))
          .status != lemma::extension::SurfaceOperationStatus::applied) {
    return 2;
  }
  std::array<std::byte, std::size_t{16} * 1'024U> extension_output{};
  for (std::size_t turn = 0; turn < 64U && extension_runtime->output_bytes(*extension_owner) != 0;
       ++turn) {
    static_cast<void>(extension_runtime->write_ready(extension_owner->slot()));
    static_cast<void>(::recv(extension_sockets.back(), extension_output.data(),
                             extension_output.size(), MSG_DONTWAIT));
  }
  if (extension_runtime->output_bytes(*extension_owner) != 0) {
    return 2;
  }
  std::array<lemma::extension::PeerView, lemma::limits::extension_sessions_hard_max>
      extension_peers{};
  std::array<lemma::render::GridSurface, lemma::limits::extension_surfaces_hard_max>
      extension_surfaces{};
  constexpr std::array routed_input{std::byte{'a'}};
  auto terminal_result = lemma::vt::Terminal::create({});
  if (!terminal_result.has_value()) {
    return 2;
  }
  auto terminal = std::move(*terminal_result);
  lemma::render::FrameBuffer frame;
  if (!frame.prepare({.columns = 80, .rows = 24}) ||
      !lemma::render::compose_retained_single_pane(terminal, frame, true).has_value()) {
    return 2;
  }
  auto resize_terminal_result = lemma::vt::Terminal::create({});
  if (!resize_terminal_result.has_value()) {
    return 2;
  }
  auto resize_terminal = std::move(*resize_terminal_result);
  // Present the resized terminal so its lazily allocated cell-hash shadow exists; audited resizes
  // must then reuse it rather than skip it.
  lemma::render::FrameBuffer resize_frame;
  if (!resize_frame.prepare({.columns = 100, .rows = 30}) ||
      !resize_terminal.resize({.columns = 81, .rows = 24}).has_value() ||
      !lemma::render::compose_retained_single_pane(resize_terminal, resize_frame, true)
           .has_value()) {
    return 2;
  }
  for (std::size_t iteration = 0; iteration < warmup_iterations; ++iteration) {
    const auto routed = input_router.route_legacy(routed_input, routed_input.size());
    const auto bytes = write_and_compose(terminal, frame, iteration % 2U == 0 ? first : second);
    if (routed.consumed != routed_input.size() || bytes == 0 ||
        !resize_terminal.resize(audited_resize(iteration)).has_value() ||
        !lemma::render::compose_retained_single_pane(resize_terminal, resize_frame, true)
             .has_value()) {
      return 2;
    }
  }

  lemma::platform::Readiness readiness{1};
  std::array<pollfd, 1> readiness_descriptors{
      {{.fd = extension_sockets.back(), .events = POLLOUT, .revents = 0}}};
  constexpr std::array<lemma::platform::ReadinessIdentity, 1> readiness_identities{
      {{.domain = 1, .owner = 0, .generation = 1}}};
  if (readiness.wait(readiness_descriptors, readiness_identities, 0) != 1) {
    return 2;
  }
  const auto terminal_before = terminal.allocation_stats();
  const auto resize_terminal_before = resize_terminal.allocation_stats();
  audited_allocations.store(0, std::memory_order_relaxed);
  audited_bytes.store(0, std::memory_order_relaxed);
  audit_enabled.store(true, std::memory_order_release);

  WriteAudit write_audit;
  std::size_t routed_input_bytes = 0;
  std::size_t frames_composed = 0;
  std::size_t composed_frame_bytes = 0;
  std::size_t frame_messages_queued = 0;
  std::size_t flush_calls = 0;
  std::size_t maximum_frame_bytes = 0;
  std::size_t maximum_queued_messages = 0;
  std::size_t extension_accounting_scans = 0;
  for (std::size_t iteration = 0; iteration < audited_iterations; ++iteration) {
    const auto routed = input_router.route_legacy(routed_input, routed_input.size());
    const auto frame_bytes =
        write_and_compose(terminal, frame, iteration % 2U == 0 ? first : second);
    const auto extension_peer_view = extension_runtime->peer_views(extension_peers);
    const auto extension_surface_view = extension_runtime->collect_surfaces(
        extension_attachment, {.columns = 80, .rows = 24}, extension_surfaces);
    const auto extension_viewport =
        extension_runtime->pane_viewport(extension_attachment, {.columns = 80, .rows = 24});
    const auto extension_accounting = extension_runtime->accounting();
    // Interest changes must retain registration without allocating, even when nothing is ready.
    readiness_descriptors.front().events = iteration % 2U == 0 ? POLLIN : POLLOUT;
    const auto ready = readiness.wait(readiness_descriptors, readiness_identities, 0);
    if (routed.consumed != routed_input.size() || frame_bytes == 0 ||
        !resize_terminal.resize(audited_resize(iteration)).has_value() ||
        extension_peer_view.size() != 1U || extension_surface_view.size() != 1U ||
        !extension_viewport.has_value() || extension_accounting.peers != 1U ||
        extension_accounting.surfaces != 1U || extension_accounting.input_bytes != 0 ||
        extension_accounting.output_bytes != 0 || ready != (iteration % 2U == 0 ? 0 : 1)) {
      audit_enabled.store(false, std::memory_order_release);
      return 2;
    }
    routed_input_bytes += routed.consumed;
    ++frames_composed;
    ++extension_accounting_scans;
    composed_frame_bytes += frame_bytes;
    maximum_frame_bytes = std::max(maximum_frame_bytes, frame_bytes);
    lemma::core::ClientFrameOutput output;
    constexpr auto now = lemma::core::ClientFrameOutput::TimePoint{};
    if (!output.queue_frame(frame_bytes, 2, 1, false, now)) {
      audit_enabled.store(false, std::memory_order_release);
      return 2;
    }
    frame_messages_queued += output.queued_message_count();
    maximum_queued_messages = std::max(maximum_queued_messages, output.queued_message_count());
    lemma::core::ClientFrameFlushTarget target{
        .descriptor = 7,
        .frame = &frame,
        .output = &output,
        .write = &audited_write,
        .context = &write_audit,
    };
    std::size_t budget = lemma::core::attached_client_write_bytes_per_turn_max;
    ++flush_calls;
    if (lemma::core::flush_client_frame(target, budget, now) !=
        lemma::core::ClientFrameFlushStatus::drained) {
      audit_enabled.store(false, std::memory_order_release);
      return 2;
    }
  }
  audit_enabled.store(false, std::memory_order_release);
  static_cast<void>(::close(std::exchange(extension_sockets.back(), -1)));

  const auto terminal_after = terminal.allocation_stats();
  const auto resize_terminal_after = resize_terminal.allocation_stats();
  const auto general_allocations = audited_allocations.load(std::memory_order_relaxed);
  const auto general_bytes = audited_bytes.load(std::memory_order_relaxed);
  const auto terminal_allocations =
      (terminal_after.allocations_total - terminal_before.allocations_total) +
      (resize_terminal_after.allocations_total - resize_terminal_before.allocations_total);
  const bool work_bounded =
      routed_input_bytes == audited_iterations && frames_composed == audited_iterations &&
      flush_calls == audited_iterations && frame_messages_queued >= frames_composed &&
      write_audit.attempts >= frame_messages_queued &&
      write_audit.attempts <= frame_messages_queued * 3U &&
      extension_accounting_scans == audited_iterations;
  const bool passed =
      general_allocations == 0 && general_bytes == 0 && terminal_allocations == 0 && work_bounded;
  std::println(R"({{
  "schema": 2,
  "suite": "steady-state-allocation-audit",
  "status": "{}",
  "warmup_iterations": {},
  "audited_iterations": {},
  "terminal_resize_iterations": {},
  "general_allocation_calls": {},
  "general_allocation_bytes": {},
  "terminal_quota_allocation_calls": {},
  "extension_accounting_scans": {},
  "active_extension_peers": 1,
  "active_extension_surfaces": 1,
  "routed_input_bytes": {},
  "frames_composed": {},
  "composed_frame_bytes": {},
  "frame_messages_queued": {},
  "flush_calls": {},
  "writer_attempts": {},
  "maximum_frame_bytes": {},
  "maximum_queued_messages": {},
  "maximum_write_attempt_bytes": {},
  "flushed_wire_bytes": {}
}})",
               passed ? "passed" : "failed", warmup_iterations, audited_iterations,
               audited_iterations, general_allocations, general_bytes, terminal_allocations,
               extension_accounting_scans, routed_input_bytes, frames_composed,
               composed_frame_bytes, frame_messages_queued, flush_calls, write_audit.attempts,
               maximum_frame_bytes, maximum_queued_messages, write_audit.maximum_attempt_bytes,
               write_audit.bytes);
  return passed ? 0 : 1;
}
