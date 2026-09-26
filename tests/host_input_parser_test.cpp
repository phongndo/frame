#include "client/host_input_parser.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lemma::client {
namespace {

struct ObservedEvent final {
  HostInputKind kind{HostInputKind::ordinary};
  std::string bytes;
  protocol::KeyInput key{};
  protocol::FocusInput focus{protocol::FocusInput::lost};
  protocol::MouseInput mouse{};
  platform::WindowSize window_size{};
};

void collect(const HostInputBatch& batch, const std::span<const std::byte> storage,
             std::vector<ObservedEvent>& output) {
  for (const auto& event : std::span(batch.events).first(batch.event_count)) {
    const auto bytes = storage.subspan(event.offset, event.size);
    output.push_back({
        .kind = event.kind,
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        .bytes = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        .key = event.key,
        .focus = event.focus,
        .mouse = event.mouse,
        .window_size = event.window_size,
    });
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, PreservesTypedBoundariesAcrossEveryFragmentationPoint) {
  constexpr std::string_view encoded = "x\x1B[200~paste\x02"
                                       "bytes\x1B[201~\x1B[I\x1B[<0;5;3My";
  const auto encoded_bytes = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= encoded.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare().has_value());
    std::vector<ObservedEvent> observed;
    const auto first = encoded_bytes.first(split);
    const auto second = encoded_bytes.subspan(split);
    std::array<std::byte, protocol::input_bytes_max * 2U> storage{};
    const auto first_batch = parser.parse(first, storage, {.columns = 80, .rows = 24});
    ASSERT_TRUE(first_batch.has_value()) << split;
    collect(*first_batch, storage, observed);
    const auto second_batch = parser.parse(second, storage, {.columns = 80, .rows = 24});
    ASSERT_TRUE(second_batch.has_value()) << split;
    collect(*second_batch, storage, observed);
    ASSERT_FALSE(parser.has_pending_sequence()) << split;

    ASSERT_EQ(observed.size(), 5U) << split;
    EXPECT_EQ(observed.at(0).kind, HostInputKind::ordinary);
    EXPECT_EQ(observed.at(0).bytes, "x");
    EXPECT_EQ(observed.at(1).kind, HostInputKind::paste);
    EXPECT_EQ(observed.at(1).bytes, std::string("paste\x02"
                                                "bytes",
                                                11));
    EXPECT_EQ(observed.at(2).kind, HostInputKind::focus);
    EXPECT_EQ(observed.at(2).focus, protocol::FocusInput::gained);
    EXPECT_EQ(observed.at(3).kind, HostInputKind::mouse);
    EXPECT_EQ(observed.at(3).mouse.action, protocol::MouseInputAction::press);
    EXPECT_EQ(observed.at(3).mouse.button, protocol::MouseInputButton::left);
    EXPECT_EQ(observed.at(3).mouse.column, 4);
    EXPECT_EQ(observed.at(3).mouse.row, 2);
    EXPECT_EQ(observed.at(3).mouse.geometry, (protocol::Dimensions{.columns = 80, .rows = 24}));
    EXPECT_EQ(observed.at(4).kind, HostInputKind::ordinary);
    EXPECT_EQ(observed.at(4).bytes, "y");
  }
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, DistinguishesVerticalAndHorizontalSgrWheelButtons) {
  HostInputParser parser;
  ASSERT_TRUE(parser.prepare().has_value());
  std::array<std::byte, 64> storage{};
  constexpr std::string_view encoded = "\x1B[<64;5;3M\x1B[<65;5;3M\x1B[<66;5;3M\x1B[<67;5;3M"
                                       "\x1B[<128;5;3M\x1B[<129;5;3M\x1B[<130;5;3M\x1B[<131;5;3M";
  constexpr std::array expected{
      protocol::MouseInputButton::four,  protocol::MouseInputButton::five,
      protocol::MouseInputButton::six,   protocol::MouseInputButton::seven,
      protocol::MouseInputButton::eight, protocol::MouseInputButton::nine,
      protocol::MouseInputButton::ten,   protocol::MouseInputButton::eleven,
  };

  const auto parsed =
      parser.parse(std::as_bytes(std::span(encoded)), storage, {.columns = 80, .rows = 24});

  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->event_count, expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& event = parsed->events.subspan(index, 1).front();
    EXPECT_EQ(event.kind, HostInputKind::mouse);
    EXPECT_EQ(event.mouse.button, expected.at(index));
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, OutOfBoundsMouseReportsNeverBecomeKeyboardInput) {
  constexpr std::string_view encoded = "\x1b[<0;114;10M\x1b[<32;114;11M\x1b[<0;114;11m"
                                       "\x1b[<0;68;14M\x1b[<0;68;14mZ";
  const auto input = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= input.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare().has_value());
    std::array<std::byte, 1024> storage{};
    std::vector<ObservedEvent> events;
    for (const auto fragment : {input.first(split), input.subspan(split)}) {
      const auto batch = parser.parse(fragment, storage, {.columns = 42, .rows = 14});
      ASSERT_TRUE(batch.has_value());
      collect(*batch, storage, events);
    }
    ASSERT_EQ(events.size(), 1U) << split;
    EXPECT_EQ(events.front().kind, HostInputKind::ordinary);
    EXPECT_EQ(events.front().bytes, "Z");
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, NativeSizeReportsUpdateFollowingMouseAcrossEveryFragmentationPoint) {
  constexpr std::string_view encoded = "\x1b[48;50;160;1700;2560t\x1b[<0;114;35MZ";
  const auto input = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= input.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare().has_value());
    std::array<std::byte, 128> storage{};
    std::vector<ObservedEvent> events;
    protocol::Dimensions geometry{.columns = 42, .rows = 14};
    for (const auto fragment : {input.first(split), input.subspan(split)}) {
      const auto batch = parser.parse(fragment, storage, geometry);
      ASSERT_TRUE(batch.has_value());
      collect(*batch, storage, events);
      for (const auto& event : std::span(batch->events).first(batch->event_count)) {
        if (event.kind == HostInputKind::window_size) {
          geometry = {.columns = event.window_size.columns, .rows = event.window_size.rows};
        }
      }
    }
    ASSERT_EQ(events.size(), 3U) << split;
    EXPECT_EQ(events.at(0).kind, HostInputKind::window_size);
    EXPECT_EQ(events.at(0).window_size.columns, 160);
    EXPECT_EQ(events.at(0).window_size.rows, 50);
    EXPECT_EQ(events.at(0).window_size.cell_width_px, 16);
    EXPECT_EQ(events.at(0).window_size.cell_height_px, 34);
    EXPECT_EQ(events.at(1).kind, HostInputKind::mouse);
    EXPECT_EQ(events.at(1).mouse.column, 113);
    EXPECT_EQ(events.at(1).mouse.row, 34);
    EXPECT_EQ(events.at(1).mouse.geometry.columns, 160);
    EXPECT_EQ(events.at(2).bytes, "Z");
    EXPECT_FALSE(parser.pending_report().has_value());
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, DistinguishesKittyZeroKeysFromSizeReportsAcrossEveryFragmentationPoint) {
  constexpr std::string_view encoded = "\x1b[48;5u\x1b[48;1:2u\x1b[48;1:3u\x1b[48;;48u"
                                       "t\x1b[48;50;160;1700;2560t";
  const auto input = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= input.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare());
    std::array<std::byte, 128> storage{};
    std::vector<ObservedEvent> events;
    for (const auto fragment : {input.first(split), input.subspan(split)}) {
      const auto batch = parser.parse(fragment, storage, {.columns = 80, .rows = 24});
      ASSERT_TRUE(batch) << split;
      collect(*batch, storage, events);
    }
    ASSERT_EQ(events.size(), 6U) << split;
    for (std::size_t index = 0; index < 4; ++index) {
      EXPECT_EQ(events.at(index).kind, HostInputKind::key);
      EXPECT_EQ(events.at(index).key.unshifted_codepoint, static_cast<std::uint32_t>('0'));
    }
    EXPECT_EQ(events.at(0).key.modifiers, protocol::key_input_modifier_control);
    EXPECT_EQ(events.at(0).key.action, protocol::KeyInputAction::press);
    EXPECT_EQ(events.at(1).key.action, protocol::KeyInputAction::repeat);
    EXPECT_EQ(events.at(2).key.action, protocol::KeyInputAction::release);
    EXPECT_EQ(events.at(3).bytes, "0");
    EXPECT_EQ(events.at(4).kind, HostInputKind::ordinary);
    EXPECT_EQ(events.at(4).bytes, "t");
    EXPECT_EQ(events.at(5).kind, HostInputKind::window_size);
    EXPECT_EQ(events.at(5).window_size.columns, 160);
    EXPECT_EQ(events.at(5).window_size.rows, 50);
    EXPECT_FALSE(parser.has_pending_sequence());
    EXPECT_FALSE(parser.pending_report());
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, TransportReportIdentityDoesNotRenewWithPartialProgress) {
  HostInputParser parser;
  ASSERT_TRUE(parser.prepare().has_value());
  std::array<std::byte, 128> storage{};
  const auto parse = [&](const std::string_view input) {
    return parser.parse(std::as_bytes(std::span(input)), storage, {.columns = 42, .rows = 14});
  };
  ASSERT_TRUE(parse("\x1b[<").has_value());
  const auto first = parser.pending_report();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(parse("0;114;").has_value());
  EXPECT_EQ(parser.pending_report(), first);
  EXPECT_EQ(parser.flush_pending(storage).error(), HostInputError::incomplete_terminal_report);
  // Even a discarded stale report completes the old transport deadline.
  const auto batch = parse("10M\x1b[48;");
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->event_count, 0U);
  const auto second = parser.pending_report();
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(second, first);
  ASSERT_TRUE(parse("50;160;").has_value());
  EXPECT_EQ(parser.pending_report(), second);
  EXPECT_EQ(parser.flush_pending(storage).error(), HostInputError::incomplete_terminal_report);
  ASSERT_TRUE(parse("1700;2560t").has_value());
  EXPECT_FALSE(parser.pending_report().has_value());
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, RejectsMalformedSizeReportsWithoutTypingThem) {
  for (const std::string_view input : {"\x1b[48;0;80;0;0t", "\x1b[48;24;0;0;0t", "\x1b[48;24;80;0t",
                                       "\x1b[48;24;80;0;0;0t", "\x1b[48;24;80;4294967296;0t"}) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare().has_value());
    std::array<std::byte, 128> storage{};
    const auto parsed = parser.parse(std::as_bytes(std::span(input)), storage, {});
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), HostInputError::invalid_size_report);
  }
}

TEST(HostInputParserTest, SizeAndMouseBytesInsideBracketedPasteStayOpaque) {
  HostInputParser parser;
  ASSERT_TRUE(parser.prepare().has_value());
  std::array<std::byte, 128> storage{};
  constexpr std::string_view input = "\x1b[200~\x1b[48;50;160;1700;2560t\x1b[<0;114;35M\x1b[201~";
  const auto batch = parser.parse(std::as_bytes(std::span(input)), storage, {});
  ASSERT_TRUE(batch.has_value());
  std::vector<ObservedEvent> events;
  collect(*batch, storage, events);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front().kind, HostInputKind::paste);
  EXPECT_EQ(events.front().bytes, "\x1b[48;50;160;1700;2560t\x1b[<0;114;35M");
  EXPECT_FALSE(parser.pending_report().has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, ThemeReportsAndPastedThemeBytesKeepDistinctBoundaries) {
  constexpr std::string_view encoded = "x\x1b]10;rgb:ffff/ffff/ffff\a"
                                       "\x1b]4;1;rgb:ffff/0000/0000\x1b\\"
                                       "\x1b[200~\x1b]11;rgb:0000/0000/0000\x1b\\\x1b[201~Z";
  const auto input = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= input.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare().has_value());
    std::array<std::byte, 256> storage{};
    std::vector<ObservedEvent> events;
    for (const auto fragment : {input.first(split), input.subspan(split)}) {
      const auto batch = parser.parse(fragment, storage, {});
      ASSERT_TRUE(batch.has_value());
      collect(*batch, storage, events);
    }
    ASSERT_EQ(events.size(), 5U) << split;
    EXPECT_EQ(events.at(0).bytes, "x");
    EXPECT_EQ(events.at(1).kind, HostInputKind::theme_reply);
    EXPECT_EQ(events.at(1).bytes, "\x1b]10;rgb:ffff/ffff/ffff\a");
    EXPECT_EQ(events.at(2).kind, HostInputKind::theme_reply);
    EXPECT_EQ(events.at(2).bytes, "\x1b]4;1;rgb:ffff/0000/0000\x1b\\");
    EXPECT_EQ(events.at(3).kind, HostInputKind::paste);
    EXPECT_EQ(events.at(3).bytes, "\x1b]11;rgb:0000/0000/0000\x1b\\");
    EXPECT_EQ(events.at(4).bytes, "Z");
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest,
     Osc52AndCapabilityRepliesKeepTypedBoundariesAcrossEveryFragmentationPoint) {
  constexpr std::string_view encoded = "\x1b]52;c;YQ==\x1b\\\x1b[?5522;0$y\x1b[?5522;2$y"
                                       "\x1b[200~\x1b]52;c;Yg==\a\x1b[?5522;0$y\x1b[201~Z";
  const auto input = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= input.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare());
    std::array<std::byte, 256> storage{};
    std::vector<ObservedEvent> events;
    for (const auto fragment : {input.first(split), input.subspan(split)}) {
      const auto batch = parser.parse(fragment, storage, {});
      ASSERT_TRUE(batch);
      collect(*batch, storage, events);
    }
    ASSERT_EQ(events.size(), 5U) << split;
    EXPECT_EQ(events.at(0).kind, HostInputKind::terminal_reply_stream);
    EXPECT_EQ(events.at(0).bytes, "\x1b]52;c;YQ==\x1b\\");
    EXPECT_EQ(events.at(1).kind, HostInputKind::clipboard_unsupported);
    EXPECT_EQ(events.at(2).kind, HostInputKind::clipboard_supported);
    EXPECT_EQ(events.at(3).kind, HostInputKind::paste);
    EXPECT_EQ(events.at(3).bytes, "\x1b]52;c;Yg==\a\x1b[?5522;0$y");
    EXPECT_EQ(events.at(4).bytes, "Z");
    EXPECT_FALSE(parser.pending_report());
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, StreamingOsc52PartsDoNotRenewTheReportIdentityOrKeyboardDeadline) {
  HostInputParser parser;
  ASSERT_TRUE(parser.prepare());
  std::array<std::byte, host_input_output_bytes_max> storage{};
  const auto parse = [&](const std::string_view input) {
    return parser.parse(std::as_bytes(std::span(input)), storage, {});
  };
  ASSERT_TRUE(parse("\x1b]52;c;"));
  const auto report = parser.pending_report();
  ASSERT_TRUE(report);
  std::size_t parts = 0;
  for (unsigned i = 0; i < 3; ++i) {
    const auto batch = parse(std::string(5000, 'A'));
    ASSERT_TRUE(batch);
    for (const auto& event : std::span(batch->events).first(batch->event_count)) {
      EXPECT_EQ(event.kind, HostInputKind::terminal_reply_stream);
      EXPECT_LE(event.size, protocol::terminal_reply_bytes_max);
      ++parts;
    }
    EXPECT_EQ(parser.pending_report(), report);
  }
  EXPECT_GT(parts, 0U);
  EXPECT_EQ(parser.flush_pending(storage).error(), HostInputError::incomplete_terminal_reply);
  ASSERT_TRUE(parse("\a"));
  EXPECT_FALSE(parser.pending_report());
}

// GoogleTest assertion macros inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, RejectsMalformedClipboardCapabilityWithoutTypingIt) {
  for (const std::string_view input : {"\x1b[?5522;5$y", "\x1b[?5522;2y", "\x1b[?5522;x$y"}) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare());
    std::array<std::byte, 128> storage{};
    const auto batch = parser.parse(std::as_bytes(std::span(input)), storage, {});
    ASSERT_FALSE(batch);
    EXPECT_EQ(batch.error(), HostInputError::invalid_terminal_report);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, DecodesKittyKeyMetadataAcrossEveryFragmentationPoint) {
  constexpr std::string_view encoded = "\x1B[98;5:1;98u\x1B[57352;1:2u";
  const auto input = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= input.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare().has_value());
    std::array<std::byte, 64> storage{};
    std::vector<HostInputEvent> events;
    for (const auto fragment : {std::span(input).first(split), std::span(input).subspan(split)}) {
      const auto parsed = parser.parse(fragment, storage, {.columns = 80, .rows = 24});
      ASSERT_TRUE(parsed.has_value()) << split;
      for (const auto& event : std::span(parsed->events).first(parsed->event_count)) {
        events.push_back(event);
      }
    }
    ASSERT_EQ(events.size(), 2U) << split;
    EXPECT_EQ(events.at(0).kind, HostInputKind::key);
    EXPECT_EQ(events.at(0).key.key, protocol::KeyInputKey::b);
    EXPECT_EQ(events.at(0).key.modifiers, protocol::key_input_modifier_control);
    EXPECT_EQ(events.at(0).key.action, protocol::KeyInputAction::press);
    EXPECT_EQ(events.at(0).size, 1U);
    EXPECT_EQ(events.at(1).kind, HostInputKind::key);
    EXPECT_EQ(events.at(1).key.key, protocol::KeyInputKey::arrow_up);
    EXPECT_EQ(events.at(1).key.action, protocol::KeyInputAction::repeat);
    EXPECT_EQ(events.at(1).size, 0U);
  }
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, DecodesKittySpecialKeyEventsAcrossEveryFragmentationPoint) {
  constexpr std::string_view encoded = "\x1B[1;1:1D\x1B[1;1:3D\x1B[1;6:2A";
  const auto input = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= input.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare().has_value());
    std::array<std::byte, 64> storage{};
    std::vector<HostInputEvent> events;
    for (const auto fragment : {std::span(input).first(split), std::span(input).subspan(split)}) {
      const auto parsed = parser.parse(fragment, storage, {.columns = 80, .rows = 24});
      ASSERT_TRUE(parsed.has_value()) << split;
      for (const auto& event : std::span(parsed->events).first(parsed->event_count)) {
        events.push_back(event);
      }
    }
    ASSERT_EQ(events.size(), 3U) << split;
    EXPECT_EQ(events.at(0).kind, HostInputKind::key);
    EXPECT_EQ(events.at(0).key.key, protocol::KeyInputKey::arrow_left);
    EXPECT_EQ(events.at(0).key.action, protocol::KeyInputAction::press);
    EXPECT_EQ(events.at(0).key.modifiers, 0U);
    EXPECT_EQ(events.at(1).kind, HostInputKind::key);
    EXPECT_EQ(events.at(1).key.key, protocol::KeyInputKey::arrow_left);
    EXPECT_EQ(events.at(1).key.action, protocol::KeyInputAction::release);
    EXPECT_EQ(events.at(1).key.modifiers, 0U);
    EXPECT_EQ(events.at(2).kind, HostInputKind::key);
    EXPECT_EQ(events.at(2).key.key, protocol::KeyInputKey::arrow_up);
    EXPECT_EQ(events.at(2).key.action, protocol::KeyInputAction::repeat);
    EXPECT_EQ(events.at(2).key.modifiers,
              protocol::key_input_modifier_shift | protocol::key_input_modifier_control);
  }
}

TEST(HostInputParserTest, LeavesMalformedKittySpecialKeyAsOrdinaryInput) {
  HostInputParser parser;
  ASSERT_TRUE(parser.prepare().has_value());
  std::array<std::byte, 64> storage{};
  constexpr std::string_view encoded = "\x1B[1;0:1D";

  const auto parsed =
      parser.parse(std::as_bytes(std::span(encoded)), storage, {.columns = 80, .rows = 24});

  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->event_count, 1U);
  EXPECT_EQ(parsed->events.front().kind, HostInputKind::ordinary);
  EXPECT_EQ(parsed->events.front().size, encoded.size());
  EXPECT_FALSE(parser.has_pending_sequence());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HostInputParserTest, DecodesGhosttyAssociatedTextWithDefaultModifierField) {
  constexpr std::string_view encoded = "\x1B[108;;108u";
  const auto input = std::as_bytes(std::span(encoded));
  for (std::size_t split = 0; split <= input.size(); ++split) {
    HostInputParser parser;
    ASSERT_TRUE(parser.prepare().has_value());
    std::array<std::byte, 64> storage{};
    std::vector<ObservedEvent> events;
    for (const auto fragment : {std::span(input).first(split), std::span(input).subspan(split)}) {
      const auto parsed = parser.parse(fragment, storage, {.columns = 80, .rows = 24});
      ASSERT_TRUE(parsed.has_value()) << split;
      collect(*parsed, storage, events);
    }
    ASSERT_EQ(events.size(), 1U) << split;
    EXPECT_EQ(events.front().kind, HostInputKind::key);
    EXPECT_EQ(events.front().key.key, protocol::KeyInputKey::l);
    EXPECT_EQ(events.front().key.unshifted_codepoint, static_cast<std::uint32_t>('l'));
    EXPECT_EQ(events.front().bytes, "l");
  }
}

TEST(HostInputParserTest, LeavesUnknownEscapeSequenceAsOrdinaryInput) {
  HostInputParser parser;
  ASSERT_TRUE(parser.prepare().has_value());
  std::array<std::byte, 64> storage{};
  constexpr std::string_view first = "\x1B[";
  constexpr std::string_view second = "A";
  ASSERT_TRUE(parser.parse(std::as_bytes(std::span(first)), storage, {.columns = 80, .rows = 24})
                  .has_value());
  const auto parsed =
      parser.parse(std::as_bytes(std::span(second)), storage, {.columns = 80, .rows = 24});
  ASSERT_TRUE(parsed.has_value());
  ASSERT_EQ(parsed->event_count, 1U);
  const auto event = parsed->events.front();
  ASSERT_EQ(event.kind, HostInputKind::ordinary);
  const auto bytes = std::span(storage).subspan(event.offset, event.size);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "\x1B[A");
}

} // namespace
} // namespace lemma::client
