#include "lemma/limits.hpp"
#include "lemma/terminal/terminal.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lemma::vt {
namespace {

constexpr std::string_view link_close = "\x1B]8;;\x1B\\";

void write_text(Terminal& terminal, const std::string_view text) {
  terminal.write(std::as_bytes(std::span(text.data(), text.size())));
}

[[nodiscard]] auto make_terminal(const std::uint16_t columns, const std::uint16_t rows)
    -> Terminal {
  TerminalOptions options;
  options.size = {.columns = columns, .rows = rows};
  auto result = Terminal::create(options);
  EXPECT_TRUE(result.has_value());
  return std::move(result).value();
}

[[nodiscard]] auto linked(const std::string_view uri, const std::string_view text) -> std::string {
  return std::string("\x1B]8;;").append(uri).append("\x1B\\").append(text).append(link_close);
}

[[nodiscard]] auto link_open(const Terminal& terminal, const std::string_view uri) -> std::string {
  return std::string("\x1B]8;id=lemma-")
      .append(std::to_string(terminal.graphics_identity()))
      .append(";")
      .append(uri)
      .append("\x1B\\");
}

[[nodiscard]] auto view(const std::span<const std::byte> output, const std::size_t bytes)
    -> std::string_view {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const char*>(output.data()), bytes};
}

// Every open link closes before the cursor moves, a line or cells are erased, the screen scrolls,
// or the frame ends, so no link leaks onto content Lemma did not link.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void expect_links_contained(const std::string_view encoded) {
  constexpr std::string_view open = "\x1B]8;id=";
  bool active = false;
  std::size_t index = 0;
  while (index < encoded.size()) {
    const auto rest = encoded.substr(index);
    if (rest.starts_with(open)) {
      EXPECT_FALSE(active) << "nested open at " << index;
      active = true;
      const auto end = rest.find("\x1B\\");
      ASSERT_NE(end, std::string_view::npos);
      index += end + 2U;
    } else if (rest.starts_with(link_close)) {
      EXPECT_TRUE(active) << "unmatched close at " << index;
      active = false;
      index += link_close.size();
    } else if (rest.starts_with("\x1B[")) {
      auto final = std::size_t{2};
      while (final < rest.size() && (rest.at(final) < 0x40 || rest.at(final) > 0x7E)) {
        ++final;
      }
      ASSERT_LT(final, rest.size());
      if (active) {
        EXPECT_EQ(std::string_view("HKXST").find(rest.at(final)), std::string_view::npos)
            << "link open across " << rest.substr(0, final + 1U) << " at " << index;
      }
      index += final + 1U;
    } else {
      ++index;
    }
  }
  EXPECT_FALSE(active) << "link open at frame end";
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalHyperlinkTest, ForwardsOnlyBoundedPrintableUrisWithAScheme) {
  const auto forwardable = [](const std::string_view uri) {
    return outer_hyperlink_uri_forwardable(
        std::span(reinterpret_cast<const std::uint8_t*>(uri.data()), // NOLINT
                  uri.size()));
  };
  EXPECT_TRUE(forwardable("https://example.com/a?b=c;d#e"));
  EXPECT_TRUE(forwardable("file://host/tmp/%E2%98%83"));
  EXPECT_TRUE(forwardable("vscode+x-1.2://file/a b"));
  EXPECT_TRUE(forwardable(std::string("https://a/") +
                          std::string(limits::outer_hyperlink_uri_bytes_max - 10U, 'a')));

  EXPECT_FALSE(forwardable(""));
  EXPECT_FALSE(forwardable(std::string("https://a/") +
                           std::string(limits::outer_hyperlink_uri_bytes_max - 9U, 'a')));
  EXPECT_FALSE(forwardable("relative/path"));
  EXPECT_FALSE(forwardable(":no-scheme"));
  EXPECT_FALSE(forwardable("1http://a"));
  EXPECT_FALSE(forwardable("ht_tp://a"));
  EXPECT_FALSE(forwardable("https://a/\x1B\\x"));
  EXPECT_FALSE(forwardable("https://a/\x07"));
  EXPECT_FALSE(forwardable("https://a/\x7F"));
  EXPECT_FALSE(forwardable("https://a/\n"));
  EXPECT_FALSE(forwardable("https://a/\xC2\x9C"));
  EXPECT_FALSE(forwardable("https://a/\x9C"));
  EXPECT_FALSE(forwardable("https://a/\xE2\x98\x83"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalHyperlinkTest, EmitsPaneScopedLinkRunsAndClosesThemAtRunAndRowEnds) {
  auto terminal = make_terminal(10, 3);
  write_text(terminal, "a " + linked("https://a.test/x", "link") + " b\r\n" +
                           linked("https://a.test/row", "0123456789") + "\r\nplain");

  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());
  // Link resolution and presentation allocate nothing once the render rows exist.
  const auto allocations_before = terminal.allocation_stats().allocations_total;
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  const auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr("a " + link_open(terminal, "https://a.test/x") + "link" +
                                          std::string(link_close) + " b"));
  // A link reaching the last column closes before the next row is positioned.
  EXPECT_THAT(encoded,
              testing::HasSubstr(link_open(terminal, "https://a.test/row") + "\x1B[0m0123456789" +
                                 std::string(link_close) + "\x1B[3;1H"));
  expect_links_contained(encoded);
  EXPECT_EQ(terminal.allocation_stats().allocations_total, allocations_before);

  // The same content in another Pane never shares an outer link ID.
  auto other = make_terminal(10, 3);
  write_text(other, linked("https://a.test/x", "link"));
  const auto composed = other.render_pane_ansi(output, {.column = 20, .force_full = true});
  ASSERT_TRUE(composed.has_value());
  EXPECT_NE(other.graphics_identity(), terminal.graphics_identity());
  EXPECT_THAT(view(output, composed->bytes),
              testing::HasSubstr("\x1B[1;21H" + link_open(other, "https://a.test/x") +
                                 "\x1B[0mlink" + std::string(link_close)));
}

TEST(TerminalHyperlinkTest, LinkChangesRepaintOtherwiseIdenticalPlainText) {
  auto terminal = make_terminal(20, 4);
  write_text(terminal, linked("https://a.test/1", "same") + "\r\nsame");
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());

  write_text(terminal, "\x1B[1;1H" + linked("https://a.test/2", "same"));
  auto rendered = terminal.render_ansi(output);
  ASSERT_TRUE(rendered.has_value());
  EXPECT_EQ(rendered->rows, 1U);
  auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H" + link_open(terminal, "https://a.test/2") +
                                          "\x1B[0msame" + std::string(link_close)));
  expect_links_contained(encoded);

  // Removing the link rewrites the text without one; adding one to the plain row links it.
  write_text(terminal, "\x1B[1;1Hsame\x1B[2;1H" + linked("https://a.test/3", "same"));
  rendered = terminal.render_ansi(output);
  ASSERT_TRUE(rendered.has_value());
  EXPECT_EQ(rendered->rows, 2U);
  encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;1H\x1B[0msame"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[2;1H" + link_open(terminal, "https://a.test/3") +
                                          "\x1B[0msame" + std::string(link_close)));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("https://a.test/2")));
  expect_links_contained(encoded);

  // Unchanged links cost no bytes, including on a redraw that probes plain rows.
  write_text(terminal, "\x1B[4;1H");
  rendered = terminal.render_pane_ansi(output, {.focused = true});
  ASSERT_TRUE(rendered.has_value());
  EXPECT_EQ(rendered->rows, 0U);
}

TEST(TerminalHyperlinkTest, AdjacentRunsSharingRawCellsKeepDistinctLinks) {
  auto terminal = make_terminal(20, 2);
  write_text(terminal, linked("https://a.test/1", "aa") + linked("https://a.test/2", "aa"));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  const auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded,
              testing::HasSubstr(link_open(terminal, "https://a.test/1") + "\x1B[0maa" +
                                 std::string(link_close) + link_open(terminal, "https://a.test/2") +
                                 "aa" + std::string(link_close)));
  expect_links_contained(encoded);
}

TEST(TerminalHyperlinkTest, PartialRedrawReopensTheLinkOfTheChangedSpan) {
  auto terminal = make_terminal(20, 2);
  write_text(terminal, linked("https://a.test/", "abcdefgh"));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());

  write_text(terminal, "\x1B[1;4H" + linked("https://a.test/", "X"));
  const auto rendered = terminal.render_ansi(output);
  ASSERT_TRUE(rendered.has_value());
  const auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1;4H" + link_open(terminal, "https://a.test/") +
                                          "\x1B[0mX" + std::string(link_close)));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("abc")));
  expect_links_contained(encoded);
}

TEST(TerminalHyperlinkTest, ScrollMovesLinkedRowsAndPaintsOnlyTheNewRow) {
  auto terminal = make_terminal(20, 4);
  // Identical text with distinct links: only the links distinguish the rows.
  write_text(terminal, linked("https://a.test/1", "x") + "\r\n" + linked("https://a.test/2", "x") +
                           "\r\n" + linked("https://a.test/3", "x") + "\r\n" +
                           linked("https://a.test/4", "x"));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());

  write_text(terminal, "\r\n" + linked("https://a.test/5", "x"));
  const auto rendered = terminal.render_ansi(output);
  ASSERT_TRUE(rendered.has_value());
  EXPECT_EQ(rendered->scrolled_rows, 1);
  EXPECT_EQ(rendered->rows, 1U);
  const auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[1S"));
  EXPECT_THAT(encoded, testing::HasSubstr("\x1B[4;1H" + link_open(terminal, "https://a.test/5") +
                                          "\x1B[0mx" + std::string(link_close)));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("https://a.test/4")));
  expect_links_contained(encoded);
}

TEST(TerminalHyperlinkTest, WideCharactersAndSelectionStayWithinTheirLink) {
  auto terminal = make_terminal(20, 2);
  write_text(terminal, linked("https://a.test/w", "\xE6\x97\xA5\xE6\x9C\xAC") + " " +
                           linked("https://a.test/s", "word") + " tail");
  ASSERT_TRUE(terminal.select(SelectionUnit::word, {.space = PointSpace::viewport, .column = 6})
                  .value_or(false));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  const auto encoded = view(output, rendered->bytes);
  // Spacer tails change no link; the pair closes once, after the second character.
  EXPECT_THAT(encoded, testing::HasSubstr(link_open(terminal, "https://a.test/w") +
                                          "\x1B[0m\xE6\x97\xA5\xE6\x9C\xAC" +
                                          std::string(link_close) + " "));
  // A selected run keeps its link while its highlight style changes.
  const auto selected = encoded.find(link_open(terminal, "https://a.test/s"));
  ASSERT_NE(selected, std::string_view::npos);
  const auto run = encoded.substr(selected);
  EXPECT_THAT(run.substr(0, run.find(link_close)), testing::HasSubstr("48;2;"));
  EXPECT_THAT(run.substr(0, run.find(link_close)), testing::HasSubstr("word"));
  expect_links_contained(encoded);
}

TEST(TerminalHyperlinkTest, UnforwardableUrisPresentPlainText) {
  auto terminal = make_terminal(20, 3);
  write_text(terminal, linked("https://a.test/\xE2\x98\x83", "snow") + "\r\n" +
                           linked("relative", "rel") + "\r\n" +
                           linked("https://a.test/" + std::string(3'000, 'a'), "long"));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  const auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("\x1B]8")));
  EXPECT_THAT(encoded, testing::HasSubstr("snow"));
  EXPECT_THAT(encoded, testing::HasSubstr("rel"));
  EXPECT_THAT(encoded, testing::HasSubstr("long"));
}

TEST(TerminalHyperlinkTest, LinksBeyondTheRenderAllowancePresentPlainText) {
  // Eight cells allow 256 link bytes per render pass; this URI alone needs more.
  auto terminal = make_terminal(4, 2);
  const auto uri = "https://a.test/" + std::string(8U * pane_ansi_hyperlink_bytes_per_cell, 'a');
  write_text(terminal, linked(uri, "big") + "\r\n" + linked("https://a.test/", "ok"));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  const auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  const auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr(uri)));
  EXPECT_THAT(encoded, testing::HasSubstr("big"));
  EXPECT_THAT(encoded, testing::HasSubstr(link_open(terminal, "https://a.test/") + "\x1B[0mok" +
                                          std::string(link_close)));
  expect_links_contained(encoded);
}

TEST(TerminalHyperlinkTest, DisablingLinksRepaintsWithoutThemAndSkipsLookups) {
  auto terminal = make_terminal(20, 2);
  write_text(terminal, linked("https://a.test/", "link"));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_pane_ansi(output, {.force_full = true}).has_value());

  auto rendered = terminal.render_pane_ansi(output, {.hyperlinks = false});
  ASSERT_TRUE(rendered.has_value());
  EXPECT_TRUE(rendered->full);
  EXPECT_THAT(view(output, rendered->bytes), testing::Not(testing::HasSubstr("\x1B]8")));
  EXPECT_THAT(view(output, rendered->bytes), testing::HasSubstr("link"));

  rendered = terminal.render_pane_ansi(output, {.hyperlinks = false});
  ASSERT_TRUE(rendered.has_value());
  EXPECT_FALSE(rendered->full);
  EXPECT_EQ(rendered->rows, 0U);

  rendered = terminal.render_pane_ansi(output, {});
  ASSERT_TRUE(rendered.has_value());
  EXPECT_TRUE(rendered->full);
  EXPECT_THAT(view(output, rendered->bytes),
              testing::HasSubstr(link_open(terminal, "https://a.test/") + "\x1B[0mlink" +
                                 std::string(link_close)));
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TerminalHyperlinkTest, ScrollbackAndAlternateScreenResolveTheirOwnVisibleRows) {
  auto terminal = make_terminal(20, 4);
  for (std::size_t row = 1; row <= 200; ++row) {
    write_text(terminal, linked("https://a.test/" + std::to_string(row), "same") + "\r\n");
  }
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  terminal.scroll_viewport(ViewportScroll::top);
  auto rendered = terminal.render_ansi(output, true);
  ASSERT_TRUE(rendered.has_value());
  auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr(link_open(terminal, "https://a.test/1")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr(link_open(terminal, "https://a.test/200"))));
  expect_links_contained(encoded);

  write_text(terminal, "\x1B[?1049h" + linked("https://a.test/alternate", "same"));
  rendered = terminal.render_ansi(output);
  ASSERT_TRUE(rendered.has_value());
  encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr(link_open(terminal, "https://a.test/alternate")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr(link_open(terminal, "https://a.test/1"))));
  expect_links_contained(encoded);

  write_text(terminal, "\x1B[?1049l");
  terminal.scroll_viewport(ViewportScroll::bottom);
  rendered = terminal.render_ansi(output);
  ASSERT_TRUE(rendered.has_value());
  encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr(link_open(terminal, "https://a.test/200")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("https://a.test/alternate")));
  expect_links_contained(encoded);
}

TEST(TerminalHyperlinkTest, ReflowPreservesLinksAcrossNewRowsAndPanePlacement) {
  auto terminal = make_terminal(12, 4);
  write_text(terminal, linked("https://a.test/reflow", "abcdefghi"));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());
  ASSERT_TRUE(terminal.resize({.columns = 6, .rows = 4}).has_value());

  const auto rendered = terminal.render_pane_ansi(output, {.column = 10, .row = 2});
  ASSERT_TRUE(rendered.has_value());
  const auto encoded = view(output, rendered->bytes);
  const auto open = link_open(terminal, "https://a.test/reflow");
  EXPECT_THAT(encoded,
              testing::HasSubstr("\x1B[3;11H" + open + "\x1B[0mabcdef" + std::string(link_close)));
  EXPECT_THAT(encoded,
              testing::HasSubstr("\x1B[4;11H" + open + "\x1B[0mghi" + std::string(link_close)));
  expect_links_contained(encoded);
}

TEST(TerminalHyperlinkTest, FailedRenderInvalidatesPhysicalLinksBeforeRetry) {
  auto terminal = make_terminal(20, 4);
  write_text(terminal, linked("https://a.test/1", "same"));
  std::array<std::byte, std::size_t{16} * 1'024U> output{};
  ASSERT_TRUE(terminal.render_ansi(output, true).has_value());
  write_text(terminal, "\x1B[1;1H" + linked("https://a.test/2", "same"));
  const auto failed = terminal.render_ansi(std::span(output).first(32));
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error(), Error::out_of_space);

  const auto rendered = terminal.render_ansi(output);
  ASSERT_TRUE(rendered.has_value());
  EXPECT_TRUE(rendered->full);
  const auto encoded = view(output, rendered->bytes);
  EXPECT_THAT(encoded, testing::HasSubstr(link_open(terminal, "https://a.test/2")));
  EXPECT_THAT(encoded, testing::Not(testing::HasSubstr("https://a.test/1")));
  expect_links_contained(encoded);
}

} // namespace
} // namespace lemma::vt
