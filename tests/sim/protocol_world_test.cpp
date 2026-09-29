#include "environment.hpp"
#include "random.hpp"
#include "trace.hpp"

#include "api/command.hpp"
#include "api/json.hpp"
#include "protocol/attachment.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frame::test::sim {
namespace {

struct ServerObservation final {
  protocol::ServerMessageKind kind{protocol::ServerMessageKind::disconnect};
  protocol::Dimensions dimensions{};
  protocol::DisconnectReason reason{protocol::DisconnectReason::internal_error};
  std::string diagnostic;
  std::vector<std::byte> ansi;
  std::uint32_t sequence{0};
  std::uint32_t generation{0};
  bool full{false};

  [[nodiscard]] auto operator==(const ServerObservation&) const -> bool = default;
};

struct ClientObservation final {
  protocol::ClientMessageKind kind{protocol::ClientMessageKind::detach};
  protocol::Dimensions dimensions{};
  protocol::PaneCommand pane_command{protocol::PaneCommand::none};
  protocol::KeyInput key{};
  protocol::FocusInput focus{protocol::FocusInput::lost};
  protocol::MouseInput mouse{};
  protocol::HostTerminalTheme host_theme{};
  std::vector<std::byte> input;
  std::string session;
  std::uint32_t sequence{0};
  bool has_theme{false};

  [[nodiscard]] auto operator==(const ClientObservation&) const -> bool = default;
};

[[nodiscard]] auto observe(const protocol::ClientMessage& message) -> ClientObservation {
  ClientObservation result{
      .kind = message.kind,
      .dimensions = message.dimensions,
      .pane_command = message.pane_command,
      .key = message.key,
      .focus = message.focus,
      .mouse = message.mouse,
      .input = std::vector(message.input.begin(), message.input.end()),
      .session = std::string(message.session),
      .sequence = message.sequence,
      .has_theme = message.host_theme != nullptr,
  };
  if (message.host_theme != nullptr) {
    result.host_theme = *message.host_theme;
  }
  return result;
}

// Decoder state branches are the bounded incremental protocol contract.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto feed_decoder(protocol::ClientDecoder& decoder,
                                const std::span<const std::byte> bytes,
                                const std::span<const std::size_t> chunks)
    -> std::optional<std::vector<ClientObservation>> {
  std::vector<ClientObservation> observations;
  std::size_t offset = 0;
  std::size_t chunk_index = 0;
  while (offset < bytes.size()) {
    auto writable = decoder.writable_bytes();
    if (writable.empty()) {
      return std::nullopt;
    }
    const auto requested = chunk_index < chunks.size() ? chunks.subspan(chunk_index, 1).front()
                                                       : bytes.size() - offset;
    ++chunk_index;
    const auto copied = std::min({requested, writable.size(), bytes.size() - offset});
    if (copied == 0) {
      continue;
    }
    std::ranges::copy(bytes.subspan(offset, copied), writable.begin());
    if (!decoder.commit(copied).has_value()) {
      return std::nullopt;
    }
    offset += copied;
    while (true) {
      const auto decoded = decoder.next();
      if (!decoded.has_value()) {
        return std::nullopt;
      }
      if (!decoded->has_value()) {
        break;
      }
      observations.emplace_back(observe(**decoded));
      decoder.consume();
    }
  }
  return observations;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto feed_server_decoder(protocol::ServerDecoder& decoder,
                                       const std::span<const std::byte> bytes,
                                       const std::span<const std::size_t> chunks)
    -> std::optional<std::vector<ServerObservation>> {
  std::vector<ServerObservation> observations;
  std::size_t offset = 0;
  std::size_t chunk_index = 0;
  while (offset < bytes.size()) {
    auto writable = decoder.writable_bytes();
    if (writable.empty()) {
      return std::nullopt;
    }
    const auto requested = chunk_index < chunks.size() ? chunks.subspan(chunk_index, 1).front()
                                                       : bytes.size() - offset;
    ++chunk_index;
    const auto copied = std::min({requested, writable.size(), bytes.size() - offset});
    if (copied == 0) {
      continue;
    }
    std::ranges::copy(bytes.subspan(offset, copied), writable.begin());
    if (!decoder.commit(copied).has_value()) {
      return std::nullopt;
    }
    offset += copied;
    while (true) {
      const auto decoded = decoder.next();
      if (!decoded.has_value()) {
        return std::nullopt;
      }
      if (!decoded->has_value()) {
        break;
      }
      observations.emplace_back(ServerObservation{
          .kind = (**decoded).kind,
          .dimensions = (**decoded).dimensions,
          .reason = (**decoded).reason,
          .diagnostic = std::string((**decoded).diagnostic),
          .ansi = std::vector((**decoded).ansi.begin(), (**decoded).ansi.end()),
          .sequence = (**decoded).sequence,
          .generation = (**decoded).full_redraw_generation,
          .full = (**decoded).full_redraw,
      });
      decoder.consume();
    }
  }
  return observations;
}

[[nodiscard]] auto encoded_message(Random& random, const std::size_t choice,
                                   const std::uint32_t sequence) -> protocol::SmallMessage {
  switch (choice) {
  case 0:
    return protocol::encode_resize({.columns = random.between(1, protocol::columns_max),
                                    .rows = random.between(1, protocol::rows_max)},
                                   sequence);
  case 1:
    return protocol::encode_pane_command(random.boolean() ? protocol::PaneCommand::focus_next
                                                          : protocol::PaneCommand::zoom,
                                         sequence);
  case 2: {
    const protocol::KeyInput key{
        .action =
            random.boolean() ? protocol::KeyInputAction::press : protocol::KeyInputAction::repeat,
        .key = protocol::KeyInputKey::a,
        .modifiers = static_cast<std::uint16_t>(random.index(16)),
        .unshifted_codepoint = 'a',
    };
    constexpr std::array text{std::byte{'a'}, std::byte{0xCC}, std::byte{0x81}};
    return protocol::encode_key(key, text, sequence);
  }
  case 3:
    return protocol::encode_focus(
        random.boolean() ? protocol::FocusInput::gained : protocol::FocusInput::lost, sequence);
  case 4:
    return protocol::encode_mouse({.action = protocol::MouseInputAction::motion,
                                   .button = protocol::MouseInputButton::none,
                                   .modifiers = static_cast<std::uint16_t>(random.index(8)),
                                   .column = random.between(0, 79),
                                   .row = random.between(0, 23),
                                   .geometry = {.columns = 80, .rows = 24},
                                   .any_button_pressed = random.boolean()},
                                  sequence);
  case 5: {
    protocol::HostTerminalTheme theme;
    theme.foreground = protocol::RgbColor{.red = static_cast<std::uint8_t>(random.next()),
                                          .green = static_cast<std::uint8_t>(random.next()),
                                          .blue = static_cast<std::uint8_t>(random.next())};
    theme.set_palette_color(random.index(theme.palette.size()), *theme.foreground);
    return protocol::encode_host_theme_update(theme, sequence);
  }
  case 6:
    return protocol::encode_detach(sequence);
  default:
    return protocol::encode_pane_command(protocol::PaneCommand::none, sequence);
  }
}

class ProtocolWorld final {
public:
  ProtocolWorld() {
    if (!whole_.prepare().has_value() || !fragmented_.prepare().has_value()) {
      std::abort();
    }
    const auto hello = protocol::encode_client_hello("protocol-sim", {.columns = 80, .rows = 24});
    if (!apply_encoded(hello.bytes(), std::array{hello.bytes().size()})) {
      std::abort();
    }
    sequence_ = 2;
  }

  [[nodiscard]] auto apply(Random& random) -> std::optional<std::string> {
    const auto encoded = encoded_message(random, random.index(7), sequence_);
    std::array<std::size_t, 8> chunks{};
    std::size_t chunk_count = 0;
    std::size_t remaining = encoded.bytes().size();
    while (remaining > 0 && chunk_count < chunks.size()) {
      const auto chunk = 1U + random.index(remaining);
      chunks.at(chunk_count) = chunk;
      ++chunk_count;
      remaining -= chunk;
    }
    if (remaining > 0) {
      chunks.at(chunk_count - 1U) += remaining;
    }
    if (!apply_encoded(encoded.bytes(), std::span(chunks).first(chunk_count))) {
      return std::string{"whole and fragmented client protocol decoding diverged"};
    }
    ++sequence_;
    return std::nullopt;
  }

  [[nodiscard]] auto hash() const noexcept -> std::uint64_t { return hash_; }

private:
  [[nodiscard]] auto apply_encoded(const std::span<const std::byte> encoded,
                                   const std::span<const std::size_t> chunks) -> bool {
    const std::array whole_chunk{encoded.size()};
    const auto first = feed_decoder(whole_, encoded, whole_chunk);
    const auto second = feed_decoder(fragmented_, encoded, chunks);
    if (!first.has_value() || !second.has_value() || *first != *second || first->size() != 1) {
      return false;
    }
    const auto& observation = first->front();
    hash_ ^= static_cast<std::uint64_t>(observation.kind) +
             (static_cast<std::uint64_t>(observation.sequence) << 8U);
    hash_ *= 1'099'511'628'211ULL;
    for (const auto byte : observation.input) {
      hash_ = (hash_ ^ std::to_integer<std::uint8_t>(byte)) * 1'099'511'628'211ULL;
    }
    return true;
  }

  protocol::ClientDecoder whole_;
  protocol::ClientDecoder fragmented_;
  std::uint64_t hash_{14'695'981'039'346'656'037ULL};
  std::uint32_t sequence_{1};
};

[[nodiscard]] auto run_protocol_world(const std::uint64_t seed, const std::size_t operations,
                                      std::uint64_t* const hash = nullptr)
    -> testing::AssertionResult {
  Random random(seed);
  ProtocolWorld world;
  for (std::size_t index = 0; index < operations; ++index) {
    if (const auto error = world.apply(random); error.has_value()) {
      return testing::AssertionFailure()
             << "operation " << index << ": " << *error
             << "\nreplay: FRAME_PROTOCOL_SIM_SEED=" << seed
             << " FRAME_PROTOCOL_SIM_OPERATIONS=" << operations << " ./test sim";
    }
  }
  if (hash != nullptr) {
    *hash = world.hash();
  }
  return testing::AssertionSuccess();
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ProtocolSimulationTest, GeneratedMessagesIgnoreTransportFragmentation) {
  constexpr std::array seeds{0ULL, 1ULL, 0xC0FFEEULL, 0xDEADBEEFULL};
  std::uint64_t selected_seed = 0;
  std::uint64_t selected_operations = 512;
  const bool configured = std::getenv("FRAME_PROTOCOL_SIM_SEED") != nullptr;
  ASSERT_TRUE(environment_u64("FRAME_PROTOCOL_SIM_SEED", selected_seed));
  ASSERT_TRUE(environment_u64("FRAME_PROTOCOL_SIM_OPERATIONS", selected_operations));
  ASSERT_GT(selected_operations, 0U);
  ASSERT_LE(selected_operations, trace_operations_max);
  if (configured) {
    ASSERT_TRUE(run_protocol_world(selected_seed, selected_operations));
    return;
  }
  for (const auto seed : seeds) {
    ASSERT_TRUE(run_protocol_world(seed, selected_operations));
  }
}

TEST(ProtocolSimulationTest, SameSeedProducesTheSameDecodedTranscript) {
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  ASSERT_TRUE(run_protocol_world(0x50726F746F636F6CULL, 128, &first));
  ASSERT_TRUE(run_protocol_world(0x50726F746F636F6CULL, 128, &second));
  EXPECT_EQ(first, second);
}

// Assertions establish borrowed optional lifetimes before inspecting decoded payloads.
// NOLINTBEGIN(bugprone-unchecked-optional-access)
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ProtocolExhaustiveTest, ClientTranscriptDecodesAtEveryByteBoundary) {
  Random random(0xB0A1DA7EULL);
  std::vector<std::byte> transcript;
  const auto append = [&transcript](const std::span<const std::byte> bytes) {
    transcript.insert(transcript.end(), bytes.begin(), bytes.end());
  };
  append(protocol::encode_client_hello("boundary", {.columns = 80, .rows = 24}, 1).bytes());
  for (std::uint32_t sequence = 2; sequence <= 8; ++sequence) {
    append(encoded_message(random, sequence - 2U, sequence).bytes());
  }

  protocol::ClientDecoder reference;
  ASSERT_TRUE(reference.prepare().has_value());
  const std::array one_chunk{transcript.size()};
  const auto expected = feed_decoder(reference, transcript, one_chunk);
  ASSERT_TRUE(expected.has_value());
  ASSERT_EQ(expected->size(), 8U);

  for (std::size_t split = 0; split <= transcript.size(); ++split) {
    SCOPED_TRACE(testing::Message() << "split=" << split);
    protocol::ClientDecoder decoder;
    ASSERT_TRUE(decoder.prepare().has_value());
    const std::array chunks{split, transcript.size() - split};
    const auto actual = feed_decoder(decoder, transcript, chunks);
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(*actual, *expected);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ProtocolExhaustiveTest, ServerFramesDecodeAtEveryHeaderAndPayloadBoundary) {
  std::vector<std::byte> transcript;
  const auto append = [&transcript](const std::span<const std::byte> bytes) {
    transcript.insert(transcript.end(), bytes.begin(), bytes.end());
  };
  append(protocol::encode_daemon_hello({.columns = 80, .rows = 24}, 1).bytes());
  constexpr std::string_view ansi = "\x1B[2J\x1B[Hserver-frame";
  append(protocol::encode_render_frame_header(ansi.size(), 2, 1, true));
  append(std::as_bytes(std::span(ansi.data(), ansi.size())));
  append(protocol::encode_disconnect(protocol::DisconnectReason::normal, "done", 3).bytes());

  protocol::ServerDecoder reference;
  ASSERT_TRUE(reference.prepare().has_value());
  const std::array one_chunk{transcript.size()};
  const auto expected = feed_server_decoder(reference, transcript, one_chunk);
  ASSERT_TRUE(expected.has_value());
  ASSERT_EQ(expected->size(), 3U);

  for (std::size_t split = 0; split <= transcript.size(); ++split) {
    SCOPED_TRACE(testing::Message() << "split=" << split);
    protocol::ServerDecoder decoder;
    ASSERT_TRUE(decoder.prepare().has_value());
    const std::array chunks{split, transcript.size() - split};
    const auto actual = feed_server_decoder(decoder, transcript, chunks);
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(*actual, *expected);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AgentSchemaSimulationTest, ConcreteCommandsRoundTripThroughThePublicJsonBoundary) {
  std::vector<api::Command> commands;
  api::Command start;
  start.kind = api::CommandKind::session_start;
  start.name = "agent-world";
  start.working_directory = "/tmp";
  start.arguments = {"/bin/sh"};
  start.environment = {"A=B"};
  start.hold = true;
  start.environment_set = true;
  commands.emplace_back(std::move(start));

  api::Command resize;
  resize.kind = api::CommandKind::pane_resize;
  resize.session.name = "agent-world";
  resize.pane.id = PaneId::from_parts(0, 1);
  resize.direction = api::Direction::right;
  resize.amount = 3;
  commands.emplace_back(std::move(resize));

  api::Command wait;
  wait.kind = api::CommandKind::pane_wait;
  wait.session.name = "agent-world";
  wait.pane.id = PaneId::from_parts(0, 1);
  wait.contains = "prompt";
  wait.wait_condition = api::WaitCondition::contains;
  wait.wait_timeout_milliseconds = 2'000;
  commands.emplace_back(std::move(wait));

  api::Command capture;
  capture.kind = api::CommandKind::pane_capture;
  capture.session.name = "agent-world";
  capture.pane.id = PaneId::from_parts(0, 1);
  capture.capture_source = api::CaptureSource::recent;
  capture.capture_wrap = api::CaptureWrap::logical;
  capture.lines = 20;
  commands.emplace_back(std::move(capture));

  for (const auto& command : commands) {
    const auto encoded = api::encode_command(command);
    ASSERT_TRUE(encoded.has_value()) << api::command_name(command.kind);
    const auto parsed = api::parse_json(*encoded);
    ASSERT_TRUE(parsed.value.has_value()) << *encoded;
    const auto decoded = api::decode_command(*parsed.value);
    ASSERT_TRUE(decoded.command.has_value())
        << decoded.error.reason << " field=" << decoded.error.field;
    const auto reencoded = api::encode_command(*decoded.command);
    ASSERT_TRUE(reencoded.has_value());
    EXPECT_EQ(*reencoded, *encoded);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(AgentSchemaSimulationTest, ProcShapeRemainsBoundedAndItsCommandsDecode) {
  constexpr std::string_view proc =
      R"({"schema":"frame.proc/v1","commands":[{"command":"session.inspect","session":{"name":"world"}},{"command":"pane.wait","session":{"name":"world"},"pane":{"id":"0:1"},"contains":"ready","timeout_ms":2000}]})";
  const auto parsed = api::parse_json(proc);
  ASSERT_TRUE(parsed.value.has_value());
  EXPECT_EQ(api::json_string(*parsed.value, "schema"),
            std::optional<std::string_view>{"frame.proc/v1"});
  const auto* const commands = api::json_member(*parsed.value, "commands");
  ASSERT_NE(commands, nullptr);
  ASSERT_EQ(commands->kind, api::JsonKind::array);
  ASSERT_LE(commands->array.size(), api::json_nodes_max);
  for (const auto& document : commands->array) {
    const auto decoded = api::decode_command(document);
    ASSERT_TRUE(decoded.command.has_value())
        << decoded.error.reason << " field=" << decoded.error.field;
  }
}
// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace
} // namespace frame::test::sim
