#include "api/json.hpp"
#include "core/session.hpp"
#include "user/attention.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lemma::user {
namespace {

[[nodiscard]] auto signals(const std::uint64_t generation, const std::uint64_t bells = 0,
                           const std::uint64_t notifications = 0, const std::uint64_t commands = 0,
                           const std::optional<std::int64_t> exit_code = std::nullopt)
    -> PaneSignals {
  return {.generation = generation,
          .bells = bells,
          .notifications = notifications,
          .commands = commands,
          .exit_code = exit_code,
          .progress = Progress::none,
          .percent = std::nullopt};
}

[[nodiscard]] auto progress(const std::uint64_t generation, const Progress state,
                            const std::optional<std::uint8_t> percent) -> PaneSignals {
  return {.generation = generation,
          .bells = 0,
          .notifications = 0,
          .commands = 0,
          .exit_code = std::nullopt,
          .progress = state,
          .percent = percent};
}

[[nodiscard]] auto document(const std::string_view text) -> api::JsonValue {
  auto parsed = api::parse_json(text);
  if (!parsed.value.has_value()) {
    throw std::runtime_error("invalid test JSON");
  }
  return std::move(*parsed.value);
}

[[nodiscard]] auto marker(const TabAttention& attention, const std::string_view tab)
    -> std::string {
  return std::string(attention.marker(tab).view());
}

TEST(StatusAttentionTest, FirstListingIsSeenButProgressInFlightShows) {
  TabAttention attention;
  const std::array panes{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(3, 2, 1, 1, 1)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(4, 5, 0, 3, 2)},
      PaneMember{
          .pane = "2:1", .tab = "2:1", .signals = progress(1, Progress::normal, std::uint8_t{42})},
  };
  attention.list(panes);
  attention.visit("0:1");

  EXPECT_EQ(marker(attention, "1:1"), "");
  EXPECT_EQ(marker(attention, "2:1"), "42%");
}

TEST(StatusAttentionTest, InactiveTabsMarkUnseenAlertsAndFailedCommandsUntilVisited) {
  TabAttention attention;
  const std::array panes{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(0)},
      PaneMember{.pane = "2:1", .tab = "1:1", .signals = signals(0)},
  };
  attention.list(panes);
  attention.visit("0:1");

  ASSERT_TRUE(attention.signal("1:1", signals(1, 1)));
  EXPECT_EQ(marker(attention, "1:1"), "!");
  ASSERT_TRUE(attention.signal("2:1", signals(2, 0, 0, 1, 2)));
  EXPECT_EQ(marker(attention, "1:1"), "x!");
  auto running = signals(3, 0, 0, 1, 2);
  running.progress = Progress::normal;
  running.percent = 7;
  ASSERT_TRUE(attention.signal("2:1", running));
  EXPECT_EQ(marker(attention, "1:1"), "7%x!");

  attention.visit("1:1");
  EXPECT_EQ(marker(attention, "1:1"), "");
  ASSERT_TRUE(attention.signal("1:1", signals(4, 2, 1)));
  attention.visit("0:1");
  EXPECT_EQ(marker(attention, "1:1"), "7%");
  EXPECT_EQ(marker(attention, "0:1"), "");

  ASSERT_TRUE(attention.signal("0:1", signals(5, 1)));
  EXPECT_EQ(marker(attention, "0:1"), "");
}

TEST(StatusAttentionTest, OnlyAFailureSinceTheVisitMarks) {
  TabAttention attention;
  const std::array panes{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(0)},
  };
  attention.list(panes);
  attention.visit("0:1");

  ASSERT_TRUE(attention.signal("1:1", signals(1, 0, 0, 1, 0)));
  EXPECT_EQ(marker(attention, "1:1"), "");
  ASSERT_TRUE(attention.signal("1:1", signals(2, 0, 0, 2, std::nullopt)));
  EXPECT_EQ(marker(attention, "1:1"), "");
  ASSERT_TRUE(attention.signal("1:1", signals(3, 0, 0, 3, -1)));
  EXPECT_EQ(marker(attention, "1:1"), "x");
  ASSERT_TRUE(attention.signal("1:1", signals(4, 0, 0, 4, 0)));
  EXPECT_EQ(marker(attention, "1:1"), "");
}

TEST(StatusAttentionTest, LaterPanesCountFromCreationAndClosedPanesDrop) {
  TabAttention attention;
  const std::array first{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(1, 1)},
  };
  attention.list(first);
  attention.visit("0:1");
  EXPECT_FALSE(attention.signal("2:1", signals(1, 1)));

  const std::array second{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(2, 2)},
      PaneMember{.pane = "2:1", .tab = "2:1", .signals = signals(1, 1)},
  };
  attention.list(second);
  EXPECT_EQ(marker(attention, "1:1"), "!");
  EXPECT_EQ(marker(attention, "2:1"), "!");

  const std::array third{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1", .tab = "2:1", .signals = signals(2, 2)},
  };
  attention.list(third);
  EXPECT_EQ(marker(attention, "1:1"), "");
  EXPECT_EQ(marker(attention, "2:1"), "!");
  EXPECT_FALSE(attention.signal("2:1", signals(2, 2)));
}

TEST(StatusAttentionTest, RetainedAttentionMarksWhatHappenedWhileDetached) {
  TabAttention attention;
  const std::array first{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(0)},
  };
  attention.list(first);
  attention.visit("0:1");

  attention.detach();
  const std::array reattached{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(1, 1)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(0)},
      PaneMember{.pane = "2:1", .tab = "2:1", .signals = signals(1, 0, 0, 1, 1)},
  };
  attention.list(reattached);
  attention.visit("1:1");

  EXPECT_EQ(marker(attention, "0:1"), "!");
  EXPECT_EQ(marker(attention, "1:1"), "");
  EXPECT_EQ(marker(attention, "2:1"), "x");
}

TEST(StatusAttentionTest, SessionsObservedFromCreationCountTheirFirstListing) {
  auto attention = TabAttention::since_creation();
  const std::array panes{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(1, 1)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(2, 1, 0, 1, 1)},
  };
  attention.list(panes);
  attention.visit("0:1");

  EXPECT_EQ(marker(attention, "0:1"), "");
  EXPECT_EQ(marker(attention, "1:1"), "x!");
}

static_assert(attention_panes_max == core::panes_per_session_max);

TEST(StatusAttentionTest, OverlongListingsAreClampedToTheSessionPaneBound) {
  std::vector<std::string> ids;
  ids.reserve(attention_panes_max + 1U);
  for (std::size_t index = 0; index <= attention_panes_max; ++index) {
    ids.push_back(std::to_string(index) + ":1");
  }
  std::vector<PaneMember> panes;
  panes.reserve(ids.size());
  for (const auto& id : ids) {
    panes.push_back({.pane = id, .tab = "1:1", .signals = signals(1, 1)});
  }
  auto attention = TabAttention::since_creation();
  attention.list(panes);
  attention.visit("0:1");

  EXPECT_TRUE(attention.signal(ids.at(attention_panes_max - 1U), signals(2, 1)));
  EXPECT_FALSE(attention.signal(ids.back(), signals(2, 1)));
  EXPECT_EQ(marker(attention, "1:1"), "!");
}

TEST(StatusAttentionTest, OlderRecordsDoNotReplaceNewerValues) {
  TabAttention attention;
  const std::array panes{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(0)},
  };
  attention.list(panes);
  attention.visit("0:1");
  ASSERT_TRUE(attention.signal("1:1", progress(5, Progress::normal, std::uint8_t{50})));
  ASSERT_TRUE(attention.signal("1:1", progress(4, Progress::normal, std::uint8_t{40})));
  EXPECT_EQ(marker(attention, "1:1"), "50%");

  const std::array stale{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1", .tab = "1:1", .signals = signals(3)},
  };
  attention.list(stale);
  EXPECT_EQ(marker(attention, "1:1"), "50%");
}

TEST(StatusAttentionTest, SevereProgressWinsAndStatesHaveDistinctMarkers) {
  TabAttention attention;
  const std::array panes{
      PaneMember{.pane = "0:1", .tab = "0:1", .signals = signals(0)},
      PaneMember{.pane = "1:1",
                 .tab = "1:1",
                 .signals = progress(1, Progress::indeterminate, std::nullopt)},
      PaneMember{
          .pane = "2:1", .tab = "1:1", .signals = progress(1, Progress::paused, std::uint8_t{30})},
      PaneMember{
          .pane = "3:1", .tab = "2:1", .signals = progress(1, Progress::normal, std::nullopt)},
      PaneMember{
          .pane = "4:1", .tab = "2:1", .signals = progress(1, Progress::error, std::uint8_t{100})},
  };
  attention.list(panes);
  attention.visit("0:1");

  EXPECT_EQ(marker(attention, "1:1"), "30%=");
  EXPECT_EQ(marker(attention, "2:1"), "100%x");
  auto failed_too = progress(2, Progress::error, std::uint8_t{100});
  failed_too.commands = 1;
  failed_too.exit_code = 1;
  ASSERT_TRUE(attention.signal("4:1", failed_too));
  EXPECT_EQ(marker(attention, "2:1"), "100%x");
  ASSERT_TRUE(attention.signal("4:1", signals(3, 1, 0, 1, 1)));
  EXPECT_EQ(marker(attention, "2:1"), "%x!");
  ASSERT_TRUE(attention.signal("2:1", progress(3, Progress::paused, std::nullopt)));
  EXPECT_EQ(marker(attention, "1:1"), "%=");
}

TEST(StatusAttentionTest, DecodesPublicSignalRecords) {
  const auto decoded = decode_signals(document(
      R"({"generation":9,"bells":1,"notifications":2,"notification":null,)"
      R"("progress":{"state":"error","percent":12},"commands":3,)"
      R"("command":{"state":"finished","exit_code":-2},"title_changes":0,"cwd_changes":0})"));
  EXPECT_EQ(decoded.generation, 9U);
  EXPECT_EQ(decoded.bells, 1U);
  EXPECT_EQ(decoded.notifications, 2U);
  EXPECT_EQ(decoded.commands, 3U);
  EXPECT_EQ(decoded.exit_code, std::optional<std::int64_t>{-2});
  EXPECT_EQ(decoded.progress, Progress::error);
  EXPECT_EQ(decoded.percent, std::optional<std::uint8_t>{12});

  const auto idle = decode_signals(document(
      R"({"generation":0,"bells":0,"notifications":0,"progress":null,"commands":0,)"
      R"("command":{"state":"running","exit_code":null},"title_changes":0,"cwd_changes":0})"));
  EXPECT_EQ(idle.progress, Progress::none);
  EXPECT_FALSE(idle.exit_code.has_value());

  EXPECT_ANY_THROW(static_cast<void>(decode_signals(document(R"({"generation":1})"))));
}

} // namespace
} // namespace lemma::user
