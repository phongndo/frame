#include "environment.hpp"
#include "random.hpp"
#include "trace.hpp"

#include "core/client_frame_output.hpp"
#include "core/frame_scheduler.hpp"
#include "core/input.hpp"
#include "core/presentation_gate.hpp"
#include "core/pty_writer.hpp"
#include "lemma/terminal/terminal.hpp"
#include "protocol/attachment.hpp"
#include "render/frame_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace lemma::test::sim {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t presentation_operations_default = 256;
constexpr render::Viewport initial_viewport{.columns = 32, .rows = 8};
constexpr std::array presentation_payloads{
    std::string_view{"plain"},
    std::string_view{"\r\nline two\r\nline three"},
    std::string_view{"\x1B[1;31mred\x1B[0m"},
    std::string_view{"e\xCC\x81 and \xE7\x95\x8C"},
    std::string_view{"history-abcdefghijklmnopqrstuvwxyz\r\n"},
    std::string_view{"\x1B[?2026hheld"},
    std::string_view{"released\x1B[?2026l"},
};

enum class WriterMode : std::uint8_t {
  ready,
  blocked,
  interrupted,
  hard_error,
};

struct BoundedPtyWriter final {
  core::InteractiveDamageLatch* latch{nullptr};
  // The child output readable on the modeled PTY master before each write.
  core::PtyReadableOutput readable{nullptr};
  void* readable_context{nullptr};
  std::size_t chunk_max{1};
  std::size_t bytes_written{0};
  WriterMode mode{WriterMode::ready};
};

[[nodiscard]] auto write_pty(void* const context, const std::span<const std::byte> bytes) noexcept
    -> core::PtyWriteAttempt {
  auto& writer = *static_cast<BoundedPtyWriter*>(context);
  if (writer.latch != nullptr) {
    writer.latch->prepare_write(bytes.size(), writer.readable, writer.readable_context);
  }
  switch (writer.mode) {
  case WriterMode::blocked:
    return {.bytes = -1, .error = EAGAIN};
  case WriterMode::interrupted:
    writer.mode = WriterMode::ready;
    return {.bytes = -1, .error = EINTR};
  case WriterMode::hard_error:
    return {.bytes = -1, .error = EIO};
  case WriterMode::ready:
    break;
  }
  const auto written = std::min(bytes.size(), writer.chunk_max);
  if (written == 0) {
    return {.bytes = -1, .error = EAGAIN};
  }
  writer.bytes_written += written;
  if (writer.latch != nullptr) {
    writer.latch->record_write(written);
  }
  return {.bytes = static_cast<std::ptrdiff_t>(written)};
}

struct BoundedWriter final {
  std::array<std::byte, core::attached_client_write_bytes_per_client_turn_max> bytes{};
  std::size_t size{0};
  std::size_t chunk_max{1};
  std::size_t calls{0};
  WriterMode mode{WriterMode::ready};
};

[[nodiscard]] auto write_client(void* const context, const core::ClientFrameBytes bytes) noexcept
    -> core::ClientFrameWriteAttempt {
  auto& writer = *static_cast<BoundedWriter*>(context);
  ++writer.calls;
  switch (writer.mode) {
  case WriterMode::blocked:
    return {.bytes = -1, .error = EAGAIN};
  case WriterMode::interrupted:
    writer.mode = WriterMode::ready;
    return {.bytes = -1, .error = EINTR};
  case WriterMode::hard_error:
    return {.bytes = -1, .error = EPIPE};
  case WriterMode::ready:
    break;
  }
  const auto available = writer.bytes.size() - writer.size;
  const auto written = std::min({bytes.size(), writer.chunk_max, available});
  if (written == 0) {
    return {.bytes = -1, .error = EAGAIN};
  }
  const auto sent = bytes.first(written);
  const auto destination = std::span(writer.bytes).subspan(writer.size, written);
  std::ranges::copy(sent.head, destination.begin());
  std::ranges::copy(sent.tail, destination.subspan(sent.head.size()).begin());
  writer.size += written;
  return {.bytes = static_cast<std::ptrdiff_t>(written)};
}

[[nodiscard]] auto make_terminal(const render::Viewport viewport) -> vt::Terminal {
  vt::TerminalOptions options;
  options.size = {.columns = viewport.columns, .rows = viewport.rows};
  options.scrollback_lines_max = 256;
  auto terminal = vt::Terminal::create(options);
  if (!terminal.has_value()) {
    std::abort();
  }
  return std::move(*terminal);
}

[[nodiscard]] auto formatted(vt::Terminal& terminal) -> std::optional<std::string> {
  std::array<std::byte, std::size_t{64} * 1'024U> storage{};
  const auto size =
      terminal.format_visible_tail(vt::ScreenFormat::plain, terminal.size().rows, storage, false);
  if (!size.has_value()) {
    return std::nullopt;
  }
  // The formatter owns valid text bytes; this copy is only a bounded test observation. A composed
  // physical viewport explicitly visits blank rows that the canonical formatter may omit.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  std::string text(reinterpret_cast<const char*>(storage.data()), *size);
  while (text.ends_with('\n')) {
    text.pop_back();
  }
  return text;
}

class PresentationWorld final {
public:
  PresentationWorld()
      : terminal_(make_terminal(initial_viewport)), projected_(make_terminal(initial_viewport)) {
    frame_.bind_capacity_budget(frame_budget_);
    if (!frame_.prepare(initial_viewport) || !decoder_.prepare().has_value()) {
      std::abort();
    }
    decoder_.reset(1, false);
    pty_writer_.latch = &latch_;
    pty_writer_.readable = &readable_child_output;
    pty_writer_.readable_context = this;
    scheduler_.request(core::FrameUrgency::state_change, true, now_, sink_state());
  }

  [[nodiscard]] auto apply(Random& random, const std::size_t operation_index)
      -> std::optional<std::string> {
    switch (random.index(10)) {
    case 0:
      last_operation_ = "write";
      write_output(random);
      break;
    case 1:
      last_operation_ = "advance-time";
      advance_time(random);
      break;
    case 2:
      last_operation_ = "configure-writer";
      configure_writer(random);
      break;
    case 3:
      last_operation_ = "flush";
      flush(random);
      break;
    case 4:
      last_operation_ = "interactive-round-trip";
      interactive_round_trip(random);
      break;
    case 5:
      last_operation_ = "resize";
      if (const auto error = resize(random); error.has_value()) {
        return error;
      }
      break;
    case 6:
      last_operation_ = "detach-or-attach";
      detach_or_attach();
      break;
    case 7:
      last_operation_ = "service";
      service();
      break;
    case 8:
      last_operation_ = "configure-pty-writer";
      configure_pty_writer(random);
      break;
    case 9:
      last_operation_ = "flush-pty";
      flush_pty();
      break;
    default:
      return std::string{"unknown presentation operation"};
    }
    if (const auto error = validate(); error.has_value()) {
      return "operation " + std::to_string(operation_index) + ": " + *error;
    }
    return std::nullopt;
  }

  [[nodiscard]] auto last_operation() const noexcept -> std::string_view { return last_operation_; }

  // Safety recovery intentionally enumerates each outstanding boundary.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto heal() -> std::optional<std::string> {
    writer_.mode = WriterMode::ready;
    writer_.chunk_max = writer_.bytes.size();
    pty_writer_.mode = WriterMode::ready;
    pty_writer_.chunk_max = core::pty_write_bytes_per_pane_turn_max;
    while (!pty_writes_.empty() || !child_output_.empty()) {
      flush_pty();
    }
    Random healing_random(0);
    // First resolve an already queued frame at its current time. Advancing past its progress
    // deadline would correctly disconnect it rather than model a healed writable boundary.
    for (std::size_t turn = 0; turn < 16 && attached_ && output_.busy(); ++turn) {
      output_.mark_write_ready();
      flush(healing_random);
    }
    if (!attached_) {
      attach();
    }
    const auto synchronized = terminal_.synchronized_output();
    if (!synchronized.has_value()) {
      return std::string{"failed to inspect synchronized-output mode while healing"};
    }
    if (*synchronized) {
      write_terminal("\x1B[?2026l");
    }
    if (const auto deadline = gate_.deadline(); deadline.has_value()) {
      now_ = std::max(now_, *deadline);
    }
    const auto release = gate_.release_if_expired(now_);
    request_from_gate(release, core::FrameUrgency::state_change);
    if (!scheduler_.pending() && !output_.busy()) {
      scheduler_.request(core::FrameUrgency::state_change, true, now_, sink_state());
    }
    for (std::size_t turn = 0; turn < 64 && (scheduler_.pending() || output_.busy()); ++turn) {
      service();
      if (output_.busy()) {
        output_.mark_write_ready();
        flush(healing_random);
      }
      now_ += 20ms;
    }
    if (scheduler_.pending() || output_.busy()) {
      return std::string{"healed presentation world did not quiesce"};
    }
    return compare_projection();
  }

  [[nodiscard]] auto state_hash() -> std::uint64_t {
    constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    auto hash = offset;
    const auto text = formatted(terminal_).value_or("<format-error>");
    for (const auto character : text) {
      hash = (hash ^ static_cast<std::uint8_t>(character)) * prime;
    }
    hash = (hash ^ static_cast<std::uint64_t>(attached_)) * prime;
    hash = (hash ^ output_.size()) * prime;
    hash = (hash ^ output_.offset()) * prime;
    hash = (hash ^ static_cast<std::uint64_t>(scheduler_.pending())) * prime;
    hash = (hash ^ gate_.watchdog_releases()) * prime;
    return hash;
  }

private:
  [[nodiscard]] auto sink_state() const noexcept -> core::FrameSinkState {
    if (!attached_) {
      return core::FrameSinkState::unavailable;
    }
    return output_.busy() ? core::FrameSinkState::blocked : core::FrameSinkState::ready;
  }

  void request_from_gate(const core::PresentationGateUpdate update,
                         const core::FrameUrgency urgency) noexcept {
    if (update.visible_damage || update.urgent_render) {
      scheduler_.request(urgency, update.force_full, now_, sink_state());
    }
  }

  void write_terminal(const std::string_view payload) {
    const auto damage =
        terminal_.write_and_report_damage(std::as_bytes(std::span(payload.data(), payload.size())));
    const auto synchronized = terminal_.synchronized_output();
    if (!damage.has_value() || !synchronized.has_value()) {
      integrity_failed_ = true;
      return;
    }
    const auto update = gate_.observe(*synchronized, *damage != vt::DirtyState::clean, now_);
    request_from_gate(update, core::FrameUrgency::burst);
  }

  void write_output(Random& random) {
    write_terminal(presentation_payloads.at(random.index(presentation_payloads.size())));
  }

  void advance_time(Random& random) {
    now_ += std::chrono::milliseconds(random.index(41));
    request_from_gate(gate_.release_if_expired(now_), core::FrameUrgency::state_change);
    service();
  }

  void configure_writer(Random& random) noexcept {
    writer_.mode = static_cast<WriterMode>(random.index(4));
    writer_.chunk_max = 1U + random.index(2'048);
    if (writer_.mode != WriterMode::blocked && output_.busy()) {
      output_.mark_write_ready();
    }
  }

  void interactive_round_trip(Random& random) {
    constexpr std::array input{std::byte{'i'}, std::byte{'n'}, std::byte{'p'}, std::byte{'u'},
                               std::byte{'t'}};
    const auto accepted = 1U + random.index(input.size());
    // The child may already have written output that the reactor has not drained.
    if (const auto background = random.index(64); background > 0) {
      child_output_.push_back({.bytes = background, .response_to = 0});
    }
    const auto queued_before = pty_writes_.size();
    if (!pty_writes_.append(std::span(input).first(accepted))) {
      integrity_failed_ = true;
      return;
    }
    latch_.await_write(queued_before, pty_writes_.size());
  }

  void configure_pty_writer(Random& random) noexcept {
    pty_writer_.mode = static_cast<WriterMode>(random.index(4));
    pty_writer_.chunk_max = 1U + random.index(8);
  }

  [[nodiscard]] static auto readable_child_output(void* const context) noexcept -> std::size_t {
    const auto& world = *static_cast<const PresentationWorld*>(context);
    std::size_t readable = 0;
    for (const auto& output : world.child_output_) {
      readable += output.bytes;
    }
    return readable;
  }

  void flush_pty() {
    const bool was_waiting = latch_.waiting_for_write();
    const bool was_pending = latch_.pending();
    std::size_t budget = core::pty_write_bytes_per_pane_turn_max;
    const auto status = core::flush_pty_write_queue(pty_writes_, budget, &write_pty, &pty_writer_);
    if (status == core::PtyFlushStatus::hard_error) {
      pty_writes_.clear();
      latch_.reset();
    } else if (was_waiting && !latch_.waiting_for_write()) {
      // The accepted input reached the child, which answers after any output already queued.
      ++input_generation_;
      if (!was_pending) {
        latch_generation_ = input_generation_;
      }
      child_output_.push_back({.bytes = response.size(), .response_to = input_generation_});
    }
    drain_child_output();
  }

  // Reads a bounded chunk, as one reactor turn drains a PTY. A response to the input that armed
  // the latch, or to one written while it stayed armed, must present as interactive damage.
  void drain_child_output() {
    std::size_t budget = pty_writer_.chunk_max * 8U;
    std::size_t drained = 0;
    bool armed_response = false;
    while (budget > 0 && !child_output_.empty()) {
      auto& front = child_output_.front();
      const auto taken = std::min(budget, front.bytes);
      armed_response =
          armed_response || (front.response_to != 0 && front.response_to >= latch_generation_);
      drained += taken;
      budget -= taken;
      front.bytes -= taken;
      if (front.bytes == 0) {
        child_output_.pop_front();
      }
    }
    if (drained == 0) {
      return;
    }
    const bool pending = latch_.pending();
    if (latch_.take_response(drained, true)) {
      write_terminal(response);
      scheduler_.request(core::FrameUrgency::interactive, false, now_, sink_state());
    } else if (pending && armed_response) {
      response_lost_ = true;
    }
  }

  [[nodiscard]] auto resize(Random& random) -> std::optional<std::string> {
    const render::Viewport requested{
        .columns = static_cast<std::uint16_t>(8U + random.index(57)),
        .rows = static_cast<std::uint16_t>(3U + random.index(14)),
    };
    const auto retained = output_.busy() ? output_.frame_bytes() : 0;
    if (!frame_.prepare(requested, retained)) {
      return std::string{"frame resize allocation failed inside bounded simulation dimensions"};
    }
    const vt::TerminalSize terminal_size{.columns = requested.columns, .rows = requested.rows};
    if (!terminal_.resize(terminal_size).has_value() ||
        !projected_.resize(terminal_size).has_value()) {
      return std::string{"terminal resize failed inside bounded simulation dimensions"};
    }
    viewport_ = requested;
    scheduler_.request(core::FrameUrgency::state_change, true, now_, sink_state());
    return std::nullopt;
  }

  void detach_or_attach() {
    if (attached_) {
      output_.reset();
      scheduler_.cancel();
      attached_ = false;
      return;
    }
    attach();
  }

  void attach() {
    attached_ = true;
    output_.reset();
    next_sequence_ = 1;
    generation_ = 0;
    decoder_.reset(1, false);
    projected_ = make_terminal(viewport_);
    terminal_.invalidate_ansi_render_state();
    scheduler_.request(core::FrameUrgency::state_change, true, now_, sink_state());
  }

  void compose_due_frame() {
    if (!attached_ || !scheduler_.due(now_, sink_state())) {
      return;
    }
    const auto composed =
        render::compose_retained_single_pane(terminal_, frame_, scheduler_.force_full());
    if (!composed.has_value()) {
      integrity_failed_ = true;
      scheduler_.cancel();
      return;
    }
    if (composed->full) {
      generation_ =
          generation_ == std::numeric_limits<std::uint32_t>::max() ? 1U : generation_ + 1U;
    }
    const auto message_count = core::ClientFrameOutput::frame_message_count(composed->bytes);
    if (!output_.queue_frame(composed->bytes, next_sequence_, generation_, composed->full, now_)) {
      integrity_failed_ = true;
      terminal_.invalidate_ansi_render_state();
      scheduler_.cancel();
      return;
    }
    next_sequence_ += static_cast<std::uint32_t>(message_count);
    scheduler_.complete();
  }

  // Incremental protocol consumption mirrors all bounded decoder outcomes.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void consume_client_bytes(Random* random = nullptr) {
    std::size_t offset = 0;
    while (offset < writer_.size) {
      auto writable = decoder_.writable_bytes();
      if (writable.empty()) {
        integrity_failed_ = true;
        break;
      }
      const auto random_limit =
          random == nullptr ? writer_.size - offset : 1U + random->index(writer_.size - offset);
      const auto copied = std::min({writable.size(), writer_.size - offset, random_limit});
      std::ranges::copy(std::span(writer_.bytes).subspan(offset, copied), writable.begin());
      if (!decoder_.commit(copied).has_value()) {
        integrity_failed_ = true;
        break;
      }
      offset += copied;
      while (true) {
        const auto decoded = decoder_.next();
        if (!decoded.has_value()) {
          integrity_failed_ = true;
          break;
        }
        if (!decoded->has_value()) {
          break;
        }
        if ((**decoded).kind != protocol::ServerMessageKind::render_frame) {
          integrity_failed_ = true;
          break;
        }
        projected_.write((**decoded).ansi);
        decoder_.consume();
      }
      if (integrity_failed_) {
        break;
      }
    }
    writer_.size = 0;
  }

  void flush(Random& random) {
    if (!attached_ || !output_.busy()) {
      service();
      return;
    }
    if (writer_.mode != WriterMode::blocked) {
      output_.mark_write_ready();
    }
    std::size_t budget = core::attached_client_write_bytes_per_turn_max;
    core::ClientFrameFlushTarget target{
        .descriptor = 7,
        .frame = &frame_,
        .output = &output_,
        .write = &write_client,
        .context = &writer_,
    };
    const auto status = core::flush_client_frame(target, budget, now_);
    consume_client_bytes(&random);
    if (status == core::ClientFrameFlushStatus::hard_error ||
        status == core::ClientFrameFlushStatus::deadline_exceeded) {
      output_.reset();
      scheduler_.cancel();
      attached_ = false;
    }
    service();
  }

  void service() {
    request_from_gate(gate_.release_if_expired(now_), core::FrameUrgency::state_change);
    compose_due_frame();
  }

  [[nodiscard]] auto compare_projection() -> std::optional<std::string> {
    const auto canonical = formatted(terminal_);
    const auto projected = formatted(projected_);
    if (!canonical.has_value() || !projected.has_value()) {
      return std::string{"failed to format a quiescent presentation projection"};
    }
    if (*canonical != *projected) {
      return "quiescent client projection differs: canonical=[" + *canonical + "] projected=[" +
             *projected + "]";
    }
    return std::nullopt;
  }

  [[nodiscard]] auto validate() -> std::optional<std::string> {
    if (integrity_failed_ || terminal_.integrity_failed() || projected_.integrity_failed()) {
      return std::string{"terminal, protocol, or presentation integrity failed"};
    }
    if (response_lost_) {
      return std::string{"an input response drained as pre-input backlog"};
    }
    if (frame_budget_.used() != frame_.capacity() || frame_.capacity() > frame_budget_.maximum()) {
      return std::string{"retained frame accounting diverged"};
    }
    if (writer_.size > writer_.bytes.size() || output_.offset() > output_.size() ||
        pty_writes_.size() > core::PanePtyWriteQueue::capacity()) {
      return std::string{"a bounded output offset exceeded its storage"};
    }
    if (!attached_ && (output_.busy() || scheduler_.pending())) {
      return std::string{"detached presentation retained client work"};
    }
    if (attached_ && !output_.busy() && !scheduler_.pending() && !gate_.presentation_suppressed()) {
      return compare_projection();
    }
    return std::nullopt;
  }

  vt::Terminal terminal_;
  vt::Terminal projected_;
  render::FrameCapacityBudget frame_budget_;
  render::FrameBuffer frame_;
  protocol::ServerDecoder decoder_;
  core::ClientFrameOutput output_;
  core::FrameScheduler scheduler_;
  core::InteractiveDamageLatch latch_;
  // Child output not yet drained from the modeled PTY, in the order the child wrote it.
  struct ChildOutput final {
    std::size_t bytes{0};
    // The input generation this output answers, or zero for background output.
    std::uint64_t response_to{0};
  };
  static constexpr std::string_view response = "interactive";
  std::deque<ChildOutput> child_output_;
  std::uint64_t input_generation_{0};
  // The input that armed the latch from idle; later inputs keep its backlog while it is pending.
  std::uint64_t latch_generation_{0};
  bool response_lost_{false};
  core::PresentationGate gate_;
  core::PanePtyWriteQueue pty_writes_;
  BoundedPtyWriter pty_writer_;
  BoundedWriter writer_;
  render::Viewport viewport_{initial_viewport};
  core::ClientFrameOutput::TimePoint now_;
  std::uint32_t next_sequence_{1};
  std::uint32_t generation_{0};
  bool attached_{true};
  bool integrity_failed_{false};
  std::string_view last_operation_{"initial"};
};

[[nodiscard]] auto run_presentation_world(const std::uint64_t seed, const std::size_t operations,
                                          std::uint64_t* const final_hash = nullptr)
    -> testing::AssertionResult {
  Random random(seed);
  auto world = std::make_unique<PresentationWorld>();
  std::string history;
  for (std::size_t index = 0; index < operations; ++index) {
    const auto error = world->apply(random, index);
    history += std::to_string(index) + " " + std::string(world->last_operation()) + "\n";
    if (error.has_value()) {
      return testing::AssertionFailure()
             << *error << "\n"
             << history << "replay: LEMMA_PRESENTATION_SIM_SEED=" << seed
             << " LEMMA_PRESENTATION_SIM_OPERATIONS=" << operations << " ./test sim";
    }
  }
  if (const auto error = world->heal(); error.has_value()) {
    return testing::AssertionFailure()
           << *error << "\n"
           << history << "replay: LEMMA_PRESENTATION_SIM_SEED=" << seed
           << " LEMMA_PRESENTATION_SIM_OPERATIONS=" << operations << " ./test sim";
  }
  if (final_hash != nullptr) {
    *final_hash = world->state_hash();
  }
  return testing::AssertionSuccess();
}

TEST(PresentationSimulationTest, SameSeedAndConfigurationReachTheSameQuiescentState) {
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  ASSERT_TRUE(run_presentation_world(0x50524553454E54ULL, 128, &first));
  ASSERT_TRUE(run_presentation_world(0x50524553454E54ULL, 128, &second));
  EXPECT_EQ(first, second);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(RuntimeSimulationTest, FaultedBoundaryHistoriesHealToCurrentCanonicalTerminalState) {
  constexpr std::array<std::uint64_t, 6> seeds{
      0ULL,
      1ULL,
      0xC0FFEEULL,
      0x51A7E123ULL,
      0xDEADBEEFCAFEBABEULL,
      std::numeric_limits<std::uint64_t>::max(),
  };
  std::uint64_t selected_seed = 0;
  std::uint64_t selected_operations = presentation_operations_default;
  const bool configured = std::getenv("LEMMA_PRESENTATION_SIM_SEED") != nullptr;
  ASSERT_TRUE(environment_u64("LEMMA_PRESENTATION_SIM_SEED", selected_seed));
  ASSERT_TRUE(environment_u64("LEMMA_PRESENTATION_SIM_OPERATIONS", selected_operations));
  ASSERT_GT(selected_operations, 0U);
  ASSERT_LE(selected_operations, trace_operations_max);
  if (configured) {
    ASSERT_TRUE(run_presentation_world(selected_seed, selected_operations));
    return;
  }
  for (const auto seed : seeds) {
    ASSERT_TRUE(run_presentation_world(seed, selected_operations));
  }
}

} // namespace
} // namespace lemma::test::sim
