#include "diagnostic/latency_trace.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#ifdef FRAME_ENABLE_LATENCY_TRACE
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <unistd.h>
#endif

#include <gtest/gtest.h>

namespace frame::diagnostic {
namespace {

TEST(LatencyTraceTest, DerivesSameNonzeroTokenAcrossFragmentedMarker) {
  constexpr std::string_view marker = "__FRAME_OUTPUT_0007_ABCDEF__";
  const auto bytes = std::as_bytes(std::span(marker));

  const auto complete = latency_trace_marker_token(bytes);
  ASSERT_NE(complete, 0U);

  LatencyTraceMarkerMatcher matcher;
  EXPECT_EQ(matcher.observe(bytes.first(5)), 0U);
  EXPECT_EQ(matcher.observe(bytes.subspan(5, 11)), 0U);
  EXPECT_EQ(matcher.observe(bytes.subspan(16)), complete);

  LatencyTraceMarkerMatcher visible_matcher;
  constexpr std::string_view first = "\x1B[3GABCD";
  constexpr std::string_view second = "EF\x1B[0m";
  EXPECT_EQ(visible_matcher.observe_expected_visible(std::as_bytes(std::span(first)), complete),
            0U);
  EXPECT_EQ(visible_matcher.observe_expected_visible(std::as_bytes(std::span(second)), complete),
            complete);
}

TEST(LatencyTraceTest, IgnoresOrdinaryOutputAndRecoversAfterOversizedCandidate) {
  LatencyTraceMarkerMatcher matcher;
  std::array<std::byte, 160> oversized{};
  constexpr std::string_view prefix = "__FRAME_";
  std::ranges::copy(std::as_bytes(std::span(prefix)), oversized.begin());
  std::ranges::fill(std::span(oversized).subspan(prefix.size()), std::byte{'x'});

  EXPECT_EQ(matcher.observe(oversized), 0U);
  constexpr std::string_view marker = "__FRAME_IDLE_0001_QWERTY__";
  EXPECT_NE(matcher.observe(std::as_bytes(std::span(marker))), 0U);
}

// Assertion macros inflate the two bounded fixture/split loops.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(LatencyTraceTest, RecognizesCurrentProbeMarkersAtEveryByteBoundary) {
  for (const auto marker : {std::string_view{"__FRAME_OUTPUT_0000_AAAAAA__"},
                            std::string_view{"__FRAME_OUTPUT_9999_YYYYJP__"}}) {
    const auto bytes = std::as_bytes(std::span(marker));
    const auto expected = latency_trace_marker_token(bytes);
    ASSERT_NE(expected, 0U);
    for (std::size_t split = 0; split < bytes.size(); ++split) {
      LatencyTraceMarkerMatcher matcher;
      EXPECT_EQ(matcher.observe(bytes.first(split)), 0U);
      EXPECT_EQ(matcher.observe(bytes.subspan(split)), expected);
    }
  }
}

#ifdef FRAME_ENABLE_LATENCY_TRACE

struct TestTraceHeader final {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint16_t role;
  std::uint16_t event_size;
  std::uint32_t capacity;
  std::uint32_t process;
  std::uint64_t count;
  std::uint64_t dropped;
  std::array<std::uint64_t, 3> reserved;
};

struct TestTraceEvent final {
  std::uint64_t timestamp_ns;
  std::uint64_t sequence;
  std::uint64_t correlation;
  std::uint64_t value;
  std::uint32_t subject;
  std::uint16_t stage;
  std::uint16_t reserved;
};

static_assert(sizeof(TestTraceHeader) == 64);
static_assert(sizeof(TestTraceEvent) == 40);

TEST(LatencyTraceTest, CorrelatesOneExactReservedEventWithoutReorderingLaterEvents) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("frame-latency-trace-test-" + std::to_string(::getpid()));
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  const auto directory_text = directory.string();
  ASSERT_EQ(::setenv("FRAME_LATENCY_TRACE", directory_text.c_str(), 1), 0);

  set_latency_trace_role(LatencyTraceRole::attached_client);
  set_latency_trace_correlation(77);
  const auto pending = record_client_socket_read_latency_trace(9, 512);
  ASSERT_TRUE(pending.valid());
  record_latency_trace(LatencyTraceStage::client_outer_terminal_write_started, 1, 8);

  EXPECT_TRUE(correlate_client_socket_read_latency_trace(pending, 42));
  EXPECT_FALSE(correlate_client_socket_read_latency_trace(pending, 43));
  EXPECT_FALSE(correlate_client_socket_read_latency_trace(LatencyTraceEventHandle{}, 42));

  const auto path = directory / ("client-" + std::to_string(::getpid()) + ".ltrace");
  std::ifstream trace(path, std::ios::binary);
  ASSERT_TRUE(trace.is_open());
  std::array<char, sizeof(TestTraceHeader) + (2U * sizeof(TestTraceEvent))> encoded{};
  ASSERT_TRUE(
      static_cast<bool>(trace.read(encoded.data(), static_cast<std::streamsize>(encoded.size()))));

  TestTraceHeader header{};
  std::array<TestTraceEvent, 2> events{};
  const auto encoded_bytes = std::span(encoded);
  std::memcpy(&header, encoded_bytes.data(), sizeof(header));
  std::memcpy(events.data(), encoded_bytes.subspan(sizeof(header)).data(), sizeof(events));
  const auto event_span = std::span(events);
  const auto& first = event_span.front();
  const auto& second = event_span.subspan(1, 1).front();
  EXPECT_EQ(header.count, 2U);
  EXPECT_EQ(header.dropped, 0U);
  EXPECT_EQ(first.sequence, 1U);
  EXPECT_EQ(first.correlation, 42U);
  EXPECT_EQ(first.value, 512U);
  EXPECT_EQ(first.stage, static_cast<std::uint16_t>(LatencyTraceStage::client_socket_read));
  EXPECT_EQ(second.sequence, 2U);
  EXPECT_EQ(second.correlation, 77U);
  EXPECT_GE(second.timestamp_ns, first.timestamp_ns);

  trace.close();
  EXPECT_EQ(std::filesystem::remove_all(directory), 2U);
}

#endif

} // namespace
} // namespace frame::diagnostic
