#include "api/command.hpp"
#include "api/json.hpp"
#include "extension/protocol.hpp"
#include "extension/runtime.hpp"
#include "frame/geometry.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"
#include "render/scene.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace frame::extension {
namespace {

class SocketPair final {
public:
  SocketPair() { EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors_.data()), 0); }
  SocketPair(const SocketPair&) = delete;
  auto operator=(const SocketPair&) -> SocketPair& = delete;
  SocketPair(SocketPair&&) = delete;
  auto operator=(SocketPair&&) -> SocketPair& = delete;
  ~SocketPair() {
    for (const auto descriptor : descriptors_) {
      if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
      }
    }
  }

  [[nodiscard]] auto take_reader() noexcept -> int {
    auto descriptors = std::span(descriptors_);
    const auto descriptor = descriptors.front();
    descriptors.front() = -1;
    return descriptor;
  }
  [[nodiscard]] auto writer() const noexcept -> int { return descriptors_.back(); }

private:
  std::array<int, 2> descriptors_{-1, -1};
};

[[nodiscard]] auto decode_hello_text(const std::string_view json) -> std::optional<Hello> {
  const auto parsed = api::parse_json(json);
  return parsed.value.has_value() ? decode_hello(*parsed.value) : std::nullopt;
}

void set_header_byte(std::array<std::byte, protocol_header_bytes>& header, const std::size_t index,
                     const std::byte value) {
  std::span(header).subspan(index, 1).front() = value;
}

[[nodiscard]] auto record(const RecordKind kind, const std::uint32_t sequence,
                          const std::string_view payload) -> std::vector<std::byte> {
  const auto header = encode_header(kind, payload.size(), sequence);
  std::vector<std::byte> bytes(header.begin(), header.end());
  const auto payload_bytes = std::as_bytes(std::span(payload.data(), payload.size()));
  bytes.insert(bytes.end(), payload_bytes.begin(), payload_bytes.end());
  return bytes;
}

// GoogleTest assertion macros expand into deliberately branch-heavy control flow.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(ExtensionProtocolTest, RetainsMultipleBufferedRecordsAcrossConsumption) {
  SocketPair sockets;
  FramedPeer peer(sockets.take_reader());
  auto bytes = record(RecordKind::proc, 4, R"({"one":1})");
  const auto second = record(RecordKind::surface_update, 5, R"({"two":2})");
  bytes.insert(bytes.end(), second.begin(), second.end());
  ASSERT_EQ(::send(sockets.writer(), bytes.data(), bytes.size(), 0),
            static_cast<ssize_t>(bytes.size()));

  EXPECT_EQ(peer.read_ready(), bytes.size());
  EXPECT_EQ(peer.buffered_record_bytes(),
            protocol_header_bytes + std::string_view{R"({"one":1})"}.size());
  const auto first = peer.receive();
  EXPECT_TRUE(first.has_value());
  if (first.has_value()) {
    EXPECT_EQ(first->kind, RecordKind::proc);
    EXPECT_EQ(first->sequence, 4U);
  }
  peer.consume();
  EXPECT_TRUE(peer.buffered_record());
  EXPECT_EQ(peer.buffered_record_bytes(), second.size());
  const auto next = peer.receive();
  EXPECT_TRUE(next.has_value());
  if (next.has_value()) {
    EXPECT_EQ(next->kind, RecordKind::surface_update);
    EXPECT_EQ(next->sequence, 5U);
  }
}

TEST(ExtensionProtocolTest, ReadReadyHonorsCallerByteBudget) {
  SocketPair sockets;
  FramedPeer peer(sockets.take_reader());
  const auto bytes = record(RecordKind::proc, 3, R"({"bounded":true})");
  ASSERT_EQ(::send(sockets.writer(), bytes.data(), bytes.size(), 0),
            static_cast<ssize_t>(bytes.size()));

  EXPECT_EQ(peer.read_ready(0), 0U);
  EXPECT_EQ(peer.read_ready(5), 5U);
  EXPECT_EQ(peer.input_bytes(), 5U);
  EXPECT_FALSE(peer.buffered_record());
  EXPECT_EQ(peer.read_ready(bytes.size() - 5U), bytes.size() - 5U);
  EXPECT_TRUE(peer.buffered_record());
}

TEST(ExtensionProtocolTest, ReadinessDoesNotClaimBufferedRecordAfterServiceBudget) {
  SocketPair sockets;
  FramedPeer peer(sockets.take_reader());
  std::vector<std::byte> bytes;
  for (std::uint32_t sequence = 1; sequence <= 5; ++sequence) {
    const auto next = record(RecordKind::surface_update, sequence, "{}");
    bytes.insert(bytes.end(), next.begin(), next.end());
  }
  ASSERT_EQ(::send(sockets.writer(), bytes.data(), bytes.size(), 0),
            static_cast<ssize_t>(bytes.size()));
  ASSERT_EQ(peer.read_ready(), bytes.size());
  for (std::uint32_t sequence = 1; sequence <= 4; ++sequence) {
    const auto next = peer.receive();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next.value_or(Record{}).sequence, sequence);
    peer.consume();
  }
  const auto sixth = record(RecordKind::surface_update, 6, "{}");
  ASSERT_EQ(::send(sockets.writer(), sixth.data(), sixth.size(), 0),
            static_cast<ssize_t>(sixth.size()));
  EXPECT_EQ(peer.read_ready(), 0U);
  ASSERT_TRUE(peer.buffered_record());
  const auto fifth = peer.receive();
  ASSERT_TRUE(fifth.has_value());
  EXPECT_EQ(fifth.value_or(Record{}).sequence, 5U);
  peer.consume();
  EXPECT_FALSE(peer.buffered_record());
  EXPECT_EQ(peer.read_ready(), sixth.size());
  const auto last = peer.receive();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last.value_or(Record{}).sequence, 6U);
  peer.consume();
  EXPECT_FALSE(peer.buffered_record());
  EXPECT_FALSE(peer.receive().has_value());
  EXPECT_EQ(peer.read_ready(), 0U);
}

TEST(ExtensionProtocolTest, FragmentedHeaderDoesNotScheduleBufferedWork) {
  SocketPair sockets;
  FramedPeer peer(sockets.take_reader());
  const auto bytes = record(RecordKind::surface_update, 1, "{}");
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    ASSERT_EQ(::send(sockets.writer(), std::span(bytes).subspan(index).data(), 1, 0), 1);
    ASSERT_EQ(peer.read_ready(), 1U);
    if (index + 1U < bytes.size()) {
      EXPECT_FALSE(peer.buffered_record());
      EXPECT_FALSE(peer.receive().has_value());
    }
  }
  EXPECT_TRUE(peer.buffered_record());
  ASSERT_TRUE(peer.receive().has_value());
  peer.consume();
  EXPECT_FALSE(peer.buffered_record());
}

// NOLINTEND(readability-function-cognitive-complexity)

TEST(ExtensionProtocolTest, RejectsInvalidBufferedHeaderWithoutWaitingForPayload) {
  SocketPair sockets;
  FramedPeer peer(sockets.take_reader());
  auto header = encode_header(RecordKind::proc, 100, 2);
  header.front() = std::byte{0};
  ASSERT_EQ(::send(sockets.writer(), header.data(), header.size(), 0),
            static_cast<ssize_t>(header.size()));

  EXPECT_EQ(peer.read_ready(), header.size());
  EXPECT_TRUE(peer.buffered_record());
  EXPECT_FALSE(peer.receive().has_value());
  EXPECT_FALSE(peer.connected());
}

TEST(ExtensionRuntimeTest, RollsBackStructuralSurfaceTransactions) {
  SocketPair sockets;
  Runtime runtime;
  const auto owner =
      runtime.admit(FramedPeer(sockets.take_reader()),
                    Hello{.name = "test", .subscription = {}, .capabilities = capability_surface},
                    SessionId::from_parts(0, 1), AttachmentId::from_parts(0, 1),
                    R"({"schema":"frame.event/v1"})");
  ASSERT_TRUE(owner.has_value());
  const auto owner_id = owner.value_or(ExtensionGenerationId{});
  constexpr render::Viewport viewport{.columns = 10, .rows = 4};
  const api::SurfacePlacement placement{.kind = api::SurfacePlacementKind::dock_right,
                                        .columns = 2};

  ASSERT_TRUE(runtime.begin_surface_transaction(owner_id));
  const auto abandoned = runtime.create_surface(owner_id, placement, true, true, viewport);
  ASSERT_EQ(abandoned.status, SurfaceOperationStatus::applied);
  ASSERT_TRUE(runtime.rollback_surface_transaction(owner_id));
  EXPECT_FALSE(runtime.surface_owner(abandoned.surface).is_valid());
  constexpr PaneRectangle full_viewport{.columns = 10, .rows = 4};
  EXPECT_EQ(runtime.pane_viewport(AttachmentId::from_parts(0, 1), viewport), full_viewport);

  ASSERT_TRUE(runtime.begin_surface_transaction(owner_id));
  const auto created = runtime.create_surface(owner_id, placement, true, true, viewport);
  ASSERT_EQ(created.status, SurfaceOperationStatus::applied);
  runtime.commit_surface_transaction(owner_id);
  ASSERT_TRUE(runtime.begin_surface_transaction(owner_id));
  ASSERT_EQ(runtime.close_surface(owner_id, created.surface, viewport).status,
            SurfaceOperationStatus::applied);
  ASSERT_TRUE(runtime.rollback_surface_transaction(owner_id));
  EXPECT_EQ(runtime.surface_owner(created.surface), owner_id);
  constexpr PaneRectangle docked_viewport{.columns = 8, .rows = 4};
  EXPECT_EQ(runtime.pane_viewport(AttachmentId::from_parts(0, 1), viewport), docked_viewport);
}

// GoogleTest assertion macros expand into deliberately branch-heavy control flow.
// NOLINTBEGIN(readability-function-cognitive-complexity)
TEST(ExtensionProtocolTest, RejectsEveryInvalidHeaderFieldImmediately) {
  const auto valid = encode_header(RecordKind::proc, 0, 7);
  std::array invalid_headers{valid, valid, valid, valid, valid, valid};
  set_header_byte(invalid_headers.at(0), 4, std::byte{protocol_major + 1U});
  set_header_byte(invalid_headers.at(1), 5, std::byte{protocol_minor + 1U});
  set_header_byte(invalid_headers.at(2), 6, std::byte{0});
  set_header_byte(invalid_headers.at(3), 7, std::byte{1});
  for (std::size_t index = 8; index < 12; ++index) {
    set_header_byte(invalid_headers.at(4), index, std::byte{0xff});
  }
  for (std::size_t index = 12; index < 16; ++index) {
    set_header_byte(invalid_headers.at(5), index, std::byte{0});
  }

  for (const auto& header : invalid_headers) {
    SocketPair sockets;
    FramedPeer peer(sockets.take_reader());
    ASSERT_EQ(::send(sockets.writer(), header.data(), header.size(), 0),
              static_cast<ssize_t>(header.size()));
    EXPECT_EQ(peer.read_ready(), header.size());
    EXPECT_TRUE(peer.buffered_record());
    EXPECT_FALSE(peer.receive().has_value());
    EXPECT_FALSE(peer.connected());
  }
}

TEST(ExtensionProtocolTest, BoundsQueuedOutputWithoutDisconnectingThePeer) {
  SocketPair sockets;
  FramedPeer peer(sockets.take_reader());
  const std::string payload(limits::extension_record_bytes_max, 'x');
  EXPECT_TRUE(peer.send_json(RecordKind::error, 1, payload));
  EXPECT_FALSE(peer.send_json(RecordKind::error, 2, payload));
  EXPECT_LE(peer.output_bytes(), limits::extension_output_bytes_per_owner_max);
  EXPECT_TRUE(peer.connected());
}

TEST(ExtensionRuntimeTest, AccountsAggregateTransportProjectionAndCleanupState) {
  SocketPair first_sockets;
  SocketPair second_sockets;
  Runtime runtime;
  constexpr auto capabilities = capability_proc | capability_surface;
  const auto first =
      runtime.admit(FramedPeer(first_sockets.take_reader()),
                    Hello{.name = "first", .subscription = {}, .capabilities = capabilities},
                    SessionId::from_parts(0, 1), AttachmentId::from_parts(0, 1), {});
  const auto second =
      runtime.admit(FramedPeer(second_sockets.take_reader()),
                    Hello{.name = "second", .subscription = {}, .capabilities = capabilities},
                    SessionId::from_parts(1, 1), AttachmentId::from_parts(1, 1), {});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const auto first_owner = first.value_or(ExtensionGenerationId{});
  const auto second_owner = second.value_or(ExtensionGenerationId{});
  const auto input = record(RecordKind::surface_update, 2, "{}");
  ASSERT_EQ(::send(first_sockets.writer(), input.data(), input.size(), 0),
            static_cast<ssize_t>(input.size()));
  ASSERT_EQ(runtime.read_ready(first_owner.slot()), input.size());
  ASSERT_TRUE(runtime.reserve_proc(second_owner, 7));
  constexpr render::Viewport viewport{.columns = 80, .rows = 24};
  const auto surface =
      runtime.create_surface(first_owner,
                             api::SurfacePlacement{.kind = api::SurfacePlacementKind::overlay,
                                                   .column = 0,
                                                   .row = 0,
                                                   .columns = 4,
                                                   .rows = 2},
                             true, true, viewport);
  ASSERT_EQ(surface.status, SurfaceOperationStatus::applied);

  const auto accounting = runtime.accounting();
  EXPECT_EQ(accounting.peers, 2U);
  EXPECT_EQ(accounting.input_bytes, input.size());
  EXPECT_EQ(accounting.buffered_records, 1U);
  EXPECT_EQ(accounting.output_bytes,
            runtime.output_bytes(first_owner) + runtime.output_bytes(second_owner));
  EXPECT_EQ(accounting.output.reserved_records, 1U);
  EXPECT_EQ(accounting.output.reserved_bytes, api::json_bytes_max + protocol_header_bytes);
  EXPECT_EQ(accounting.surfaces, 1U);
  EXPECT_EQ(accounting.retained_surface_bytes, runtime.retained_surface_bytes());
  EXPECT_LE(accounting.input_bytes, limits::extension_input_bytes_aggregate_max);
  EXPECT_LE(accounting.output_bytes + accounting.output.reserved_bytes,
            limits::extension_output_bytes_aggregate_max);

  static_cast<void>(runtime.disconnect(first_owner));
  static_cast<void>(runtime.disconnect(second_owner));
  EXPECT_EQ(runtime.accounting(), RuntimeAccounting{});
}

TEST(ExtensionRuntimeTest, ValidatesHelloCapabilitiesAndObservationScope) {
  const auto valid = decode_hello_text(
      R"({"schema":"frame.extension/v1","name":"worker","capabilities":["proc"]})");
  ASSERT_TRUE(valid.has_value());
  const auto hello = valid.value_or(Hello{});
  EXPECT_EQ(hello.name, "worker");
  EXPECT_EQ(hello.capabilities, capability_proc);
  EXPECT_FALSE(
      decode_hello_text(
          R"({"schema":"frame.extension/v1","name":"worker","capabilities":["proc","proc"]})")
          .has_value());
  EXPECT_FALSE(decode_hello_text(
                   R"({"schema":"frame.extension/v1","name":"worker","capabilities":["unknown"]})")
                   .has_value());
  EXPECT_FALSE(decode_hello_text(
                   R"({"schema":"frame.extension/v1","name":"worker","capabilities":["observe"]})")
                   .has_value());
  EXPECT_FALSE(decode_hello_text(
                   R"({"schema":"frame.extension/v1","name":"worker","capabilities":["surface"]})")
                   .has_value());
}

TEST(ExtensionRuntimeTest, BoundsProcAndInteractionQueues) {
  SocketPair sockets;
  Runtime runtime;
  const auto admitted =
      runtime.admit(FramedPeer(sockets.take_reader()),
                    Hello{.name = "bounded", .subscription = {}, .capabilities = capability_proc},
                    SessionId::from_parts(0, 1), AttachmentId::from_parts(0, 1), {});
  ASSERT_TRUE(admitted.has_value());
  const auto owner = admitted.value_or(ExtensionGenerationId{});

  EXPECT_TRUE(runtime.reserve_proc(owner, 10));
  EXPECT_FALSE(runtime.reserve_proc(owner, 10));
  EXPECT_FALSE(runtime.reserve_proc(owner, 11));
  runtime.complete_proc(owner, 10, R"({"status":"ok"})");
  EXPECT_TRUE(runtime.reserve_proc(owner, 11));
  runtime.complete_proc(owner, 11, R"({"status":"ok"})");

  for (std::size_t index = 0; index < limits::extension_interaction_events_max; ++index) {
    ASSERT_TRUE(runtime.send_event(owner, R"({"event":"surface.key"})"));
  }
  EXPECT_FALSE(runtime.send_event(owner, R"({"event":"surface.key"})"));
  EXPECT_FALSE(runtime.connected(owner));
}

TEST(ExtensionRuntimeTest, ProcReservationSurvivesMixedOutputPressure) {
  SocketPair sockets;
  Runtime runtime;
  const auto admitted =
      runtime.admit(FramedPeer(sockets.take_reader()),
                    Hello{.name = "results", .subscription = {}, .capabilities = capability_proc},
                    SessionId::from_parts(0, 1), AttachmentId::from_parts(0, 1), {});
  ASSERT_TRUE(admitted.has_value());
  const auto owner = admitted.value_or(ExtensionGenerationId{});
  const auto result = [](const std::size_t size) {
    std::string json =
        R"({"schema":"frame.proc-result/v1","ok":false,"results":[],"error":{"reason":")";
    json.append(size - json.size() - 3U, 'x');
    json += "\"}}";
    return json;
  };
  const auto welcome = runtime.output_bytes(owner);
  ASSERT_TRUE(runtime.reserve_proc(owner, 1));
  runtime.complete_proc(owner, 1, result(api::json_bytes_max - welcome - 512U));
  ASSERT_TRUE(runtime.reserve_proc(owner, 2));
  EXPECT_EQ(runtime.output_accounting(owner).reserved_bytes,
            api::json_bytes_max + protocol_header_bytes);
  EXPECT_EQ(runtime.output_accounting(owner).reserved_records, 1U);
  ASSERT_TRUE(runtime.send_event(
      owner,
      R"({"schema":"frame.event/v1","sequence":2,"event":"surface.key","surface":"0:1","text":"x"})"));
  ASSERT_TRUE(runtime.send_error(owner, 3, "invalid"));
  // This error is valid, but its queue admission must not consume the result's reservation.
  EXPECT_FALSE(runtime.send_error(owner, 4, std::string(512, 'x')));
  ASSERT_TRUE(runtime.connected(owner));
  const auto before = runtime.output_bytes(owner);
  runtime.complete_proc(owner, 2, result(api::json_bytes_max));
  EXPECT_TRUE(runtime.connected(owner));
  EXPECT_EQ(runtime.output_bytes(owner), before + protocol_header_bytes + api::json_bytes_max);
  EXPECT_LE(runtime.output_bytes(owner), limits::extension_output_bytes_per_owner_max);
  EXPECT_EQ(runtime.output_accounting(owner).reserved_bytes, 0U);
  EXPECT_EQ(runtime.output_accounting(owner).reserved_records, 0U);
  FramedPeer reader(::dup(sockets.writer()));
  std::size_t records = 0;
  std::size_t results = 0;
  for (std::size_t turn = 0; turn < 512U && records < 5U; ++turn) {
    static_cast<void>(runtime.write_ready(owner.slot()));
    static_cast<void>(reader.read_ready());
    while (const auto next = reader.receive()) {
      ++records;
      if (next->kind == RecordKind::proc_result) {
        ++results;
        if (next->sequence == 2U) {
          const auto expected = result(api::json_bytes_max);
          EXPECT_TRUE(std::ranges::equal(
              next->payload, std::as_bytes(std::span(expected.data(), expected.size()))));
        }
      }
      reader.consume();
    }
  }
  EXPECT_EQ(records, 5U);
  EXPECT_EQ(results, 2U);
  EXPECT_EQ(runtime.output_bytes(owner), 0U);
  EXPECT_EQ(runtime.output_accounting(owner), OutputAccounting{});
}

TEST(ExtensionRuntimeTest, DrainingEventsDoNotAccumulateWhileQueueStaysNonempty) {
  SocketPair sockets;
  Runtime runtime;
  const auto admitted =
      runtime.admit(FramedPeer(sockets.take_reader()),
                    Hello{.name = "draining", .subscription = {}, .capabilities = capability_proc},
                    SessionId::from_parts(0, 1), AttachmentId::from_parts(0, 1), {});
  ASSERT_TRUE(admitted.has_value());
  const auto owner = admitted.value_or(ExtensionGenerationId{});
  std::array<std::byte, 1024> received{};
  const auto welcome = runtime.output_bytes(owner);
  static_cast<void>(runtime.write_ready(owner.slot()));
  ASSERT_EQ(::recv(sockets.writer(), received.data(), received.size(), MSG_DONTWAIT),
            static_cast<ssize_t>(welcome));
  constexpr std::string_view event = R"({"event":"surface.key"})";
  constexpr auto event_bytes = protocol_header_bytes + event.size();
  ASSERT_TRUE(runtime.send_event(owner, event));
  ASSERT_TRUE(runtime.send_error(owner, 1, "test"));
  const auto batch = runtime.output_bytes(owner);
  for (std::size_t index = 0; index < limits::extension_interaction_events_max * 2U; ++index) {
    ASSERT_TRUE(runtime.send_event(owner, event));
    static_cast<void>(runtime.write_ready(owner.slot(), batch));
    ASSERT_EQ(runtime.output_bytes(owner), event_bytes);
    EXPECT_EQ(runtime.output_accounting(owner).event_records, 1U);
    EXPECT_EQ(runtime.output_accounting(owner).event_bytes, event_bytes);
    ASSERT_EQ(::recv(sockets.writer(), received.data(), received.size(), MSG_DONTWAIT),
              static_cast<ssize_t>(batch));
    ASSERT_TRUE(runtime.send_error(owner, 1, "test"));
    ASSERT_EQ(runtime.output_bytes(owner), batch);
  }
  EXPECT_TRUE(runtime.connected(owner));
}

TEST(ExtensionProtocolTest, OutputAccountingConservesPartialHeadersPayloadsAndMoves) {
  SocketPair sockets;
  FramedPeer original(sockets.take_reader());
  constexpr auto reservation = protocol_header_bytes + limits::extension_record_bytes_max;
  ASSERT_TRUE(original.reserve_output(reservation));
  constexpr std::string_view error = R"({"error":"test"})";
  constexpr std::string_view event = R"({"event":"test"})";
  const std::string payload(257, 'x');
  ASSERT_TRUE(original.send_json(RecordKind::error, 1, error));
  ASSERT_TRUE(original.send_json(RecordKind::event, 2, payload));
  ASSERT_TRUE(original.send_json(RecordKind::proc_result, 3, error));
  ASSERT_TRUE(original.send_json(RecordKind::event, 4, event));
  const auto total = original.output_bytes();
  const auto first_begin = protocol_header_bytes + error.size();
  const auto first_end = first_begin + protocol_header_bytes + payload.size();
  const auto second_begin = first_end + protocol_header_bytes + error.size();
  FramedPeer peer(std::move(original));
  // The move contract explicitly resets accounting on the disconnected source.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(original.output_accounting(), OutputAccounting{});
  std::size_t sent = 0;
  std::array<std::byte, 7> bytes{};
  while (peer.output_bytes() != 0) {
    const auto before = peer.output_bytes();
    const auto count = peer.write_ready(bytes.size());
    EXPECT_EQ(count, before - peer.output_bytes());
    ASSERT_GT(count, 0U);
    ASSERT_EQ(::recv(sockets.writer(), bytes.data(), bytes.size(), MSG_DONTWAIT),
              static_cast<ssize_t>(count));
    sent += count;
    const auto first_remaining = first_end - std::clamp(sent, first_begin, first_end);
    const auto second_remaining = total - std::clamp(sent, second_begin, total);
    const auto accounting = peer.output_accounting();
    EXPECT_EQ(accounting.event_bytes, first_remaining + second_remaining);
    EXPECT_EQ(accounting.event_records, static_cast<std::size_t>(first_remaining > 0) +
                                            static_cast<std::size_t>(second_remaining > 0));
    EXPECT_EQ(accounting.reserved_bytes, reservation);
    EXPECT_EQ(accounting.reserved_records, 1U);
    EXPECT_EQ(peer.output_bytes() + sent, total);
  }
  peer.release_output(reservation);
  EXPECT_EQ(peer.output_accounting(), OutputAccounting{});
}

TEST(ExtensionProtocolTest, EagainPreservesLedgerAndDisconnectCancelsReservations) {
  SocketPair sockets;
  FramedPeer peer(sockets.take_reader());
  constexpr int buffer_bytes = 1024;
  ASSERT_EQ(
      ::setsockopt(peer.descriptor(), SOL_SOCKET, SO_SNDBUF, &buffer_bytes, sizeof(buffer_bytes)),
      0);
  constexpr auto reservation = protocol_header_bytes + limits::extension_record_bytes_max;
  ASSERT_TRUE(peer.reserve_output(reservation));
  ASSERT_TRUE(peer.send_json(RecordKind::event, 1, std::string(std::size_t{64} * 1024U, 'x')));
  ASSERT_TRUE(peer.send_json(RecordKind::error, 2, std::string(std::size_t{64} * 1024U, 'y')));
  bool blocked = false;
  for (std::size_t turn = 0; turn < 256U && !blocked; ++turn) {
    const auto before = peer.output_bytes();
    const auto ledger = peer.output_accounting();
    peer.write_ready();
    blocked = before == peer.output_bytes();
    if (blocked) {
      EXPECT_EQ(peer.output_accounting(), ledger);
    }
  }
  ASSERT_TRUE(blocked);
  ASSERT_GT(peer.output_bytes(), 0U);
  ASSERT_TRUE(peer.connected());
  std::array<std::byte, std::size_t{16} * 1024U> bytes{};
  for (std::size_t turn = 0; turn < 256U && peer.output_bytes() != 0; ++turn) {
    static_cast<void>(::recv(sockets.writer(), bytes.data(), bytes.size(), MSG_DONTWAIT));
    peer.write_ready();
  }
  EXPECT_EQ(peer.output_bytes(), 0U);
  EXPECT_EQ(peer.output_accounting().event_bytes, 0U);
  EXPECT_EQ(peer.output_accounting().event_records, 0U);
  EXPECT_EQ(peer.output_accounting().reserved_records, 1U);
  peer.disconnect();
  EXPECT_EQ(peer.output_accounting(), OutputAccounting{});
}

TEST(ExtensionRuntimeTest, DisconnectCancelsAdmittedResultReservation) {
  SocketPair sockets;
  Runtime runtime;
  const auto admitted = runtime.admit(
      FramedPeer(sockets.take_reader()),
      Hello{.name = "cancel", .subscription = {}, .capabilities = capability_proc}, {}, {}, {});
  ASSERT_TRUE(admitted.has_value());
  const auto owner = admitted.value_or(ExtensionGenerationId{});
  ASSERT_TRUE(runtime.reserve_proc(owner, 1));
  EXPECT_EQ(runtime.output_accounting(owner).reserved_records, 1U);
  static_cast<void>(runtime.disconnect(owner));
  runtime.complete_proc(owner, 1, "{}");
  EXPECT_EQ(runtime.output_accounting(owner), OutputAccounting{});
  EXPECT_EQ(runtime.output_bytes(owner), 0U);
  EXPECT_FALSE(runtime.reserve_proc(owner, 2));
}

TEST(ExtensionProtocolTest, InputEncodingPreservesTextBytesAndBounds) {
  for (const bool opaque : {false, true}) {
    for (const std::string_view text :
         {std::string_view{"é🙂"}, std::string_view{"\xe2"}, std::string_view{"\x82\xac"},
          std::string_view{"\xff\0x", 3}}) {
      std::string event = "{";
      // The encoder appends a member to an existing Event object.
      event += R"("schema":"frame.event/v1")";
      ASSERT_TRUE(append_input_payload(event, std::as_bytes(std::span(text)), opaque));
      event += '}';
      const auto parsed = api::parse_json(event);
      ASSERT_TRUE(parsed.value.has_value());
      const auto document = parsed.value.value_or(api::JsonValue{});
      if (!opaque && text == "é🙂") {
        EXPECT_EQ(api::json_string(document, "text"), text);
      } else {
        EXPECT_FALSE(api::json_string(document, "text").has_value());
        const auto hex = api::json_string(document, "bytes_hex");
        ASSERT_TRUE(hex.has_value());
        EXPECT_EQ(hex.value_or(std::string_view{}).size(), text.size() * 2U);
      }
    }
  }
  const std::string controls(limits::extension_input_bytes_max, '\0');
  std::string escaped = R"({"event":"surface.key")";
  ASSERT_TRUE(append_input_payload(escaped, std::as_bytes(std::span(controls)), false));
  escaped += '}';
  EXPECT_LT(escaped.size(), limits::extension_record_bytes_max);
  const auto parsed = api::parse_json(escaped);
  ASSERT_TRUE(parsed.value.has_value());
  EXPECT_EQ(api::json_string(parsed.value.value_or(api::JsonValue{}), "text"), controls);
  std::string rejected;
  const std::string oversized(limits::extension_input_bytes_max + 1U, 'x');
  EXPECT_FALSE(append_input_payload(rejected, std::as_bytes(std::span(oversized)), true));
  EXPECT_TRUE(rejected.empty());
}

TEST(ExtensionRuntimeTest, CanRepairAndCloseSurfacesAfterViewportShrink) {
  SocketPair sockets;
  Runtime runtime;
  constexpr auto attachment = AttachmentId::from_parts(0, 1);
  const auto admitted =
      runtime.admit(FramedPeer(sockets.take_reader()),
                    Hello{.name = "shrink", .subscription = {}, .capabilities = capability_surface},
                    SessionId::from_parts(0, 1), attachment, {});
  ASSERT_TRUE(admitted.has_value());
  const auto owner = admitted.value_or(ExtensionGenerationId{});
  constexpr render::Viewport large{.columns = 80, .rows = 24};
  constexpr render::Viewport small{.columns = 30, .rows = 10};
  const auto first = runtime.create_surface(
      owner, {.kind = api::SurfacePlacementKind::dock_right, .columns = 35}, true, true, large);
  const auto second = runtime.create_surface(
      owner, {.kind = api::SurfacePlacementKind::dock_left, .columns = 35}, true, true, large);
  ASSERT_EQ(first.status, SurfaceOperationStatus::applied);
  ASSERT_EQ(second.status, SurfaceOperationStatus::applied);
  const auto floating = runtime.create_surface(owner,
                                               {.kind = api::SurfacePlacementKind::float_surface,
                                                .column = 60,
                                                .row = 15,
                                                .columns = 10,
                                                .rows = 2},
                                               true, true, large);
  ASSERT_EQ(floating.status, SurfaceOperationStatus::applied);
  ASSERT_EQ(runtime.focus_surface(owner, floating.surface, large).status,
            SurfaceOperationStatus::applied);
  runtime.capture_surface_pointer(attachment, floating.surface);
  ASSERT_TRUE(runtime.resize_surfaces(attachment, small));
  EXPECT_EQ(runtime.pane_viewport(attachment, small), (PaneRectangle{.columns = 30, .rows = 10}));
  EXPECT_FALSE(runtime.surface_rectangle(first.surface, small).has_value());
  EXPECT_FALSE(runtime.surface_rectangle(second.surface, small).has_value());
  EXPECT_FALSE(runtime.surface_rectangle(floating.surface, small).has_value());
  EXPECT_FALSE(runtime.focused_surface(attachment).is_valid());
  EXPECT_FALSE(runtime.captured_surface_pointer(attachment).is_valid());
  EXPECT_EQ(runtime.focus_surface(owner, floating.surface, small).status,
            SurfaceOperationStatus::unavailable);
  ASSERT_TRUE(runtime.resize_surfaces(attachment, large));
  EXPECT_EQ(runtime.pane_viewport(attachment, large),
            (PaneRectangle{.column = 35, .columns = 10, .rows = 24}));
  EXPECT_EQ(runtime.surface_rectangle(first.surface, large),
            (PaneRectangle{.column = 45, .columns = 35, .rows = 24}));
  EXPECT_TRUE(runtime.surface_rectangle(floating.surface, large).has_value());
  EXPECT_FALSE(runtime.focused_surface(attachment).is_valid());
  EXPECT_EQ(runtime
                .configure_surface(owner, first.surface,
                                   {.kind = api::SurfacePlacementKind::dock_right, .columns = 5},
                                   small)
                .status,
            SurfaceOperationStatus::applied);
  EXPECT_EQ(runtime.close_surface(owner, first.surface, small).status,
            SurfaceOperationStatus::applied);
  EXPECT_EQ(runtime.close_surface(owner, second.surface, small).status,
            SurfaceOperationStatus::applied);
}

TEST(ExtensionRuntimeTest, SurfaceAdmissionRequiresValidNativeScope) {
  for (const auto attachment :
       std::array{AttachmentId{}, AttachmentId::from_parts(limits::sessions_hard_max, 1)}) {
    SocketPair sockets;
    Runtime runtime;
    EXPECT_FALSE(
        runtime
            .admit(FramedPeer(sockets.take_reader()),
                   Hello{.name = "scope", .subscription = {}, .capabilities = capability_surface},
                   SessionId::from_parts(0, 1), attachment, {})
            .has_value());
  }
}

TEST(ExtensionRuntimeTest, SessionRevocationReleasesOnlyMatchingGeneration) {
  SocketPair sockets;
  Runtime runtime;
  constexpr auto session = SessionId::from_parts(0, 1);
  constexpr auto attachment = AttachmentId::from_parts(0, 1);
  const auto admitted =
      runtime.admit(FramedPeer(sockets.take_reader()),
                    Hello{.name = "scope", .subscription = {}, .capabilities = capability_surface},
                    session, attachment, {});
  ASSERT_TRUE(admitted.has_value());
  const auto owner = admitted.value_or(ExtensionGenerationId{});
  constexpr render::Viewport viewport{.columns = 20, .rows = 10};
  constexpr api::SurfacePlacement overlay{
      .kind = api::SurfacePlacementKind::overlay, .columns = 2, .rows = 1};
  const auto created = runtime.create_surface(owner, overlay, true, true, viewport);
  ASSERT_EQ(created.status, SurfaceOperationStatus::applied);
  runtime.revoke_session(SessionId::from_parts(0, 2));
  EXPECT_TRUE(runtime.connected(owner));
  EXPECT_GT(runtime.retained_surface_bytes(), 0U);
  runtime.revoke_session(session);
  EXPECT_FALSE(runtime.connected(owner));
  EXPECT_EQ(runtime.retained_surface_bytes(), 0U);
  EXPECT_FALSE(runtime.surface_owner(created.surface).is_valid());
}

TEST(ExtensionRuntimeTest, FocusAndCaptureRequireFullAttachmentIdentity) {
  SocketPair sockets;
  Runtime runtime;
  constexpr auto original = AttachmentId::from_parts(0, 1);
  constexpr auto replacement = AttachmentId::from_parts(0, 2);
  const auto admitted =
      runtime.admit(FramedPeer(sockets.take_reader()),
                    Hello{.name = "scope", .subscription = {}, .capabilities = capability_surface},
                    SessionId::from_parts(0, 1), original, {});
  ASSERT_TRUE(admitted.has_value());
  const auto owner = admitted.value_or(ExtensionGenerationId{});
  constexpr render::Viewport viewport{.columns = 20, .rows = 10};
  constexpr api::SurfacePlacement overlay{
      .kind = api::SurfacePlacementKind::overlay, .columns = 2, .rows = 1};
  const auto created = runtime.create_surface(owner, overlay, true, true, viewport);
  ASSERT_EQ(created.status, SurfaceOperationStatus::applied);
  ASSERT_EQ(runtime.focus_surface(owner, created.surface, viewport).status,
            SurfaceOperationStatus::applied);
  runtime.capture_surface_pointer(original, created.surface);
  EXPECT_EQ(runtime.focused_surface(original), created.surface);
  EXPECT_EQ(runtime.captured_surface_pointer(original), created.surface);
  EXPECT_FALSE(runtime.focused_surface(replacement).is_valid());
  EXPECT_FALSE(runtime.captured_surface_pointer(replacement).is_valid());
  EXPECT_FALSE(runtime.focus_pane(replacement));
  runtime.release_surface_pointer(replacement);
  EXPECT_EQ(runtime.focused_surface(original), created.surface);
  EXPECT_EQ(runtime.captured_surface_pointer(original), created.surface);
  runtime.release_surface_pointer(original);
  runtime.capture_surface_pointer(replacement, created.surface);
  EXPECT_FALSE(runtime.captured_surface_pointer(original).is_valid());
}

TEST(ExtensionRuntimeTest, DisconnectedTransportCannotRetainInputOwnership) {
  SocketPair sockets;
  Runtime runtime;
  constexpr auto attachment = AttachmentId::from_parts(0, 1);
  const auto admitted =
      runtime.admit(FramedPeer(sockets.take_reader()),
                    Hello{.name = "scope", .subscription = {}, .capabilities = capability_surface},
                    SessionId::from_parts(0, 1), attachment, {});
  ASSERT_TRUE(admitted.has_value());
  const auto owner = admitted.value_or(ExtensionGenerationId{});
  constexpr render::Viewport viewport{.columns = 20, .rows = 10};
  constexpr api::SurfacePlacement overlay{
      .kind = api::SurfacePlacementKind::overlay, .columns = 2, .rows = 1};
  const auto created = runtime.create_surface(owner, overlay, true, true, viewport);
  ASSERT_EQ(created.status, SurfaceOperationStatus::applied);
  ASSERT_EQ(runtime.focus_surface(owner, created.surface, viewport).status,
            SurfaceOperationStatus::applied);
  runtime.capture_surface_pointer(attachment, created.surface);
  // Fail the transport without yet running reactor reclamation.
  ASSERT_EQ(::shutdown(sockets.writer(), SHUT_RDWR), 0);
  EXPECT_EQ(runtime.read_ready(owner.slot()), 0U);
  ASSERT_FALSE(runtime.connected(owner));
  EXPECT_FALSE(runtime.focused_surface(attachment).is_valid());
  EXPECT_FALSE(runtime.captured_surface_pointer(attachment).is_valid());
  EXPECT_FALSE(runtime.surface_at(attachment, viewport, 0, 0).is_valid());
  EXPECT_EQ(runtime.create_surface(owner, overlay, true, true, viewport).status,
            SurfaceOperationStatus::unavailable);
  std::array<AttachmentId, limits::extension_sessions_hard_max> affected{};
  EXPECT_EQ(runtime.reap_disconnected(affected).size(), 1U);
  EXPECT_EQ(runtime.retained_surface_bytes(), 0U);
}

TEST(ExtensionRuntimeTest, EnforcesSurfaceOwnershipCapacityAndGenerationCleanup) {
  SocketPair first_sockets;
  SocketPair second_sockets;
  Runtime runtime;
  constexpr auto session = SessionId::from_parts(0, 1);
  constexpr auto attachment = AttachmentId::from_parts(0, 1);
  const auto first =
      runtime.admit(FramedPeer(first_sockets.take_reader()),
                    Hello{.name = "first", .subscription = {}, .capabilities = capability_surface},
                    session, attachment, {});
  const auto second =
      runtime.admit(FramedPeer(second_sockets.take_reader()),
                    Hello{.name = "second", .subscription = {}, .capabilities = capability_surface},
                    session, attachment, {});
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const auto first_owner = first.value_or(ExtensionGenerationId{});
  const auto second_owner = second.value_or(ExtensionGenerationId{});
  constexpr render::Viewport viewport{.columns = 20, .rows = 10};
  constexpr api::SurfacePlacement overlay{
      .kind = api::SurfacePlacementKind::overlay, .columns = 2, .rows = 1};

  SurfaceId focused;
  for (std::size_t index = 0; index < limits::extension_surfaces_per_owner_max; ++index) {
    const auto created = runtime.create_surface(first_owner, overlay, true, true, viewport);
    ASSERT_EQ(created.status, SurfaceOperationStatus::applied);
    if (index == 0) {
      focused = created.surface;
    }
  }
  EXPECT_EQ(runtime.create_surface(first_owner, overlay, true, true, viewport).status,
            SurfaceOperationStatus::capacity);
  EXPECT_EQ(runtime.configure_surface(second_owner, focused, overlay, viewport).status,
            SurfaceOperationStatus::wrong_owner);
  constexpr api::SurfacePlacement dock{.kind = api::SurfacePlacementKind::dock_left, .columns = 1};
  EXPECT_EQ(runtime.create_surface(second_owner, dock, true, false, viewport).status,
            SurfaceOperationStatus::invalid);

  ASSERT_EQ(runtime.focus_surface(first_owner, focused, viewport).status,
            SurfaceOperationStatus::applied);
  runtime.capture_surface_pointer(attachment, focused);
  EXPECT_EQ(runtime.disconnect(first_owner), attachment);
  EXPECT_FALSE(runtime.surface_owner(focused).is_valid());
  EXPECT_FALSE(runtime.focused_surface(attachment).is_valid());
  EXPECT_FALSE(runtime.captured_surface_pointer(attachment).is_valid());
  constexpr PaneRectangle full_viewport{.columns = viewport.columns, .rows = viewport.rows};
  EXPECT_EQ(runtime.pane_viewport(attachment, viewport), full_viewport);

  SocketPair replacement_sockets;
  const auto replacement = runtime.admit(
      FramedPeer(replacement_sockets.take_reader()),
      Hello{.name = "replacement", .subscription = {}, .capabilities = capability_surface}, session,
      attachment, {});
  ASSERT_TRUE(replacement.has_value());
  const auto replacement_owner = replacement.value_or(ExtensionGenerationId{});
  EXPECT_EQ(replacement_owner.slot(), first_owner.slot());
  EXPECT_NE(replacement_owner.generation(), first_owner.generation());
  EXPECT_FALSE(runtime.connected(first_owner));
  EXPECT_EQ(runtime.configure_surface(replacement_owner, focused, overlay, viewport).status,
            SurfaceOperationStatus::stale);
}
// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace frame::extension
