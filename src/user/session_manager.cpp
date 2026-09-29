#include "user/session_manager.hpp"

#include "api/json.hpp"
#include "extension/client.hpp"
#include "extension/protocol.hpp"
#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <poll.h>

namespace frame::user {
namespace {
namespace ext = extension;
using api::JsonValue;
using Clock = std::chrono::steady_clock;

[[nodiscard]] auto member(const JsonValue& value, std::string_view key) -> const JsonValue& {
  const auto* found = api::json_member(value, key);
  if (found == nullptr) {
    throw std::runtime_error("missing picker response field");
  }
  return *found;
}
[[nodiscard]] auto text(const JsonValue& value, std::string_view key) -> std::string {
  return std::string(api::json_string(value, key).value_or(std::string_view{}));
}
[[nodiscard]] auto number(const JsonValue& value, std::string_view key) -> std::uint64_t {
  return api::json_unsigned(value, key).value_or(0);
}
[[nodiscard]] auto enabled(const JsonValue& value, std::string_view key) -> bool {
  return api::json_boolean(value, key).value_or(false);
}
[[nodiscard]] auto selector(std::string_view id) -> std::string {
  return "{\"id\":" + ext::json_quote(id) + '}';
}
[[nodiscard]] auto plain(std::string_view value) -> std::string {
  std::string result;
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    result += byte >= 32U && byte < 127U ? static_cast<char>(byte) : '?';
  }
  return result;
}
[[nodiscard]] auto command(ext::Client& client, std::string_view request) -> JsonValue {
  const auto result = client.proc('[' + std::string(request) + ']');
  if (!enabled(result, "ok") || member(result, "results").array.size() != 1U) {
    throw std::runtime_error("picker command failed: " + ext::json_encode(result));
  }
  return member(member(result, "results").array.front(), "result");
}
[[nodiscard]] auto scoped_hello(std::string_view session) -> std::string {
  return R"({"schema":"frame.extension/v1","name":"session-picker","capabilities":["observe","proc","surface"],"events":{"schema":"frame.events/v1","presentation":true,"session":)" +
         selector(session) + "}}";
}

struct Target final {
  std::string session;
  std::string tab;
  std::string pane;
  auto operator==(const Target&) const -> bool = default;
};
struct Pane final {
  std::string id;
  std::string command;
  std::string cwd;
  bool inspected{false};
};

[[nodiscard]] auto directory_label(std::string_view value) -> std::string {
  const auto* home = std::getenv("HOME");
  if (home != nullptr) {
    const std::string_view prefix(home);
    if (!prefix.empty() && prefix != "/" && value.starts_with(prefix) &&
        (value.size() == prefix.size() || value.at(prefix.size()) == '/')) {
      return '~' + std::string(value.substr(prefix.size()));
    }
  }
  return std::string(value);
}

[[nodiscard]] auto tail_label(std::string_view value, std::size_t width) -> std::string {
  if (value.size() <= width) {
    return std::string(value);
  }
  return width > 3 ? "..." + std::string(value.substr(value.size() - width + 3))
                   : std::string(value.substr(0, width));
}

[[nodiscard]] auto process_label(const JsonValue& process) -> std::string {
  auto name = plain(text(process, "observed_title"));
  if (!name.empty()) {
    return name;
  }
  const auto& argv = member(member(process, "launch"), "argv").array;
  if (argv.empty()) {
    return "process";
  }
  name = plain(argv.front().string);
  const auto slash = name.find_last_of('/');
  return slash == std::string::npos ? name : name.substr(slash + 1);
}
struct Tab final {
  std::string id;
  std::string title;
  std::string focused;
  std::size_t position{0};
  std::vector<Pane> panes;
};
struct Session final {
  std::string id;
  std::string name;
  std::string active_tab;
  std::uint64_t revision{0};
  std::size_t tab_count{0};
  bool attached{false};
  bool loaded{false};
  std::vector<Tab> tabs;
};
struct ContentSize final {
  std::size_t width{40};
  std::size_t rows{1};
};
struct Location final {
  Target scope;
  std::string query;
  std::size_t cursor{0};
  std::optional<Target> selected;
  std::optional<ContentSize> size;
};
struct Match final {
  Target target;
  std::string label;
  std::string detail;
  std::vector<std::size_t> highlights;
  int score{0};
};
struct Rectangle final {
  std::size_t x{0};
  std::size_t y{0};
  std::size_t width{0};
  std::size_t height{0};
  auto operator==(const Rectangle&) const -> bool = default;
};
struct Layout final {
  Rectangle surface;
  Rectangle list;
};

// Fit the unfiltered catalogue once per location, then clip only for terminal resizing.
[[nodiscard]] auto layout(std::size_t columns, std::size_t rows, ContentSize content, bool message)
    -> Layout {
  const auto width = std::min(std::clamp<std::size_t>(columns * 80 / 100, 1, 96), content.width);
  const auto height = std::min(std::clamp<std::size_t>(rows * 85 / 100, 1, 18),
                               std::max<std::size_t>(1, content.rows) + 4 + (message ? 1 : 0));
  return {
      .surface = {.x = (columns - width) / 2,
                  .y = (rows - height) / 2,
                  .width = width,
                  .height = height},
      .list = {
          .x = 0, .y = 0, .width = width, .height = message && height > 1 ? height - 1 : height}};
}

struct Cell final {
  std::string glyph{" "};
  unsigned style{0};
};
struct EncodedRow final {
  std::string text;
  std::size_t nodes{3}; // Row object, index, and run array; each run adds four JSON values.
};
class Grid final {
public:
  Grid(std::size_t columns, std::size_t rows) : columns_(columns), cells_(rows * columns) {}
  void glyph(std::size_t x, std::size_t y, std::string_view value, unsigned style = 0) {
    if (x < columns_ && y < cells_.size() / columns_) {
      cells_.at((y * columns_) + x) = {.glyph = std::string(value), .style = style};
    }
  }
  void put(std::size_t x, std::size_t y, std::string_view value, std::size_t width,
           unsigned style = 0, const std::vector<std::size_t>& highlights = {}) {
    const auto safe = plain(value);
    // Leave room for borders and detail runs when long queries alternate highlighted cells.
    const auto marked =
        std::span(highlights)
            .first(std::min(highlights.size(), (limits::surface_runs_per_row_max - 16) / 2));
    for (std::size_t i = 0; i < std::min(width, safe.size()); ++i) {
      const auto highlighted = std::ranges::binary_search(marked, i);
      const auto match_style = style == 3 ? 5U : 4U;
      glyph(x + i, y, safe.substr(i, 1), highlighted ? match_style : style);
    }
  }
  void box(const Rectangle& rect, std::string_view title) {
    if (rect.width < 2 || rect.height < 2) {
      return;
    }
    const auto right = rect.x + rect.width - 1;
    const auto bottom = rect.y + rect.height - 1;
    for (auto x = rect.x + 1; x < right; ++x) {
      glyph(x, rect.y, "─", 1);
      glyph(x, bottom, "─", 1);
    }
    for (auto y = rect.y + 1; y < bottom; ++y) {
      glyph(rect.x, y, "│", 1);
      glyph(right, y, "│", 1);
    }
    glyph(rect.x, rect.y, "╭", 1);
    glyph(right, rect.y, "╮", 1);
    glyph(rect.x, bottom, "╰", 1);
    glyph(right, bottom, "╯", 1);
    if (rect.width > 6) {
      const auto label = ' ' + plain(title).substr(0, rect.width - 6) + ' ';
      put(rect.x + ((rect.width - label.size()) / 2), rect.y, label, label.size(), 2);
    }
  }
  [[nodiscard]] auto row(std::size_t y) const -> EncodedRow {
    EncodedRow result{.text = R"({"row":)" + std::to_string(y) + R"(,"runs":[)"};
    std::size_t x = 0;
    while (x < columns_) {
      const auto begin = x;
      const auto style = cells_.at((y * columns_) + x).style;
      std::string run;
      while (x < columns_ && cells_.at((y * columns_) + x).style == style) {
        run += cells_.at((y * columns_) + x).glyph;
        ++x;
      }
      if (begin != 0) {
        result.text += ',';
      }
      result.nodes += 4;
      result.text += R"({"column":)" + std::to_string(begin) + R"(,"style":)" +
                     std::to_string(style) + R"(,"text":)" + ext::json_quote(run) + '}';
    }
    result.text += "]}";
    return result;
  }

private:
  std::size_t columns_;
  std::vector<Cell> cells_;
};

[[nodiscard]] auto lower(char value) -> char {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}
struct FuzzyMatch final {
  int score{0};
  std::vector<std::size_t> positions;
};
// Score one term with adjacency and word-boundary bonuses.
[[nodiscard]] auto fuzzy_term(std::string_view value, std::string_view term, FuzzyMatch& result)
    -> bool {
  std::size_t cursor = 0;
  for (const auto character : term) {
    const auto start = cursor;
    while (cursor < value.size() && lower(value.at(cursor)) != lower(character)) {
      ++cursor;
    }
    if (cursor == value.size()) {
      return false;
    }
    result.score += cursor == start ? 12 : 2;
    if (cursor == 0 || std::string_view(" /_-.").contains(value.at(cursor - 1))) {
      result.score += 8;
    }
    result.positions.push_back(cursor++);
  }
  return true;
}
// Terms may match different path fields. This is not fzf's extended operator grammar.
[[nodiscard]] auto fuzzy(std::string_view value, std::string_view query)
    -> std::optional<FuzzyMatch> {
  FuzzyMatch result;
  while (!query.empty()) {
    const auto end = query.find(' ');
    if (!fuzzy_term(value, query.substr(0, end), result)) {
      return std::nullopt;
    }
    if (end == std::string_view::npos) {
      break;
    }
    query.remove_prefix(end + 1);
  }
  std::ranges::sort(result.positions);
  const auto duplicates = std::ranges::unique(result.positions);
  result.positions.erase(duplicates.begin(), duplicates.end());
  return result;
}

enum class JobKind : std::uint8_t { outline, metadata };
struct Job final {
  JobKind kind{JobKind::outline};
  std::uint32_t sequence{0};
  std::string session;
  std::uint64_t revision{0};
  Clock::time_point deadline;
};

class SessionManager final {
public:
  explicit SessionManager(const JsonValue& context)
      : client_(text(context, "endpoint"), scoped_hello(text(context, "session"))),
        observer_(
            text(context, "endpoint"),
            R"({"schema":"frame.extension/v1","name":"picker-data","capabilities":["observe","proc"],"events":{"schema":"frame.events/v1"}})"),
        connection_(text(context, "connection")), current_(text(context, "session")) {
    const auto inspected =
        command(client_, R"({"command":"session.inspect","session":)" + selector(current_) + '}');
    const auto& state = member(inspected, "session_state");
    const auto& geometry = member(state, "geometry");
    columns_ = number(geometry, "columns");
    rows_ = number(geometry, "rows");
    const auto listed = command(client_, R"({"command":"session.list"})");
    reconcile(member(listed, "sessions"));
    location_.selected = Target{.session = current_, .tab = text(state, "active_tab"), .pane = {}};
    paint();
    refocus();
  }
  [[nodiscard]] auto run() -> int;

private:
  ext::Client client_;
  ext::Client observer_;
  std::string connection_;
  std::string current_;
  std::string surface_;
  std::vector<Session> sessions_;
  Location location_;
  std::vector<Location> history_;
  std::vector<Match> matches_;
  std::size_t candidate_count_{0};
  std::size_t label_width_{0};
  std::size_t detail_width_{0};
  std::optional<Job> job_;
  Layout layout_;
  std::size_t columns_{0};
  std::size_t rows_{0};
  std::vector<std::string> painted_;
  std::string message_;
  std::string pending_;
  bool dirty_{true};
  bool lost_selection_{false};
  bool repairing_{false};
  bool creating_{false};
  std::string new_name_;
  std::size_t new_cursor_{0};
  Clock::time_point escape_deadline_;

  [[nodiscard]] auto session(std::string_view id) -> Session* {
    const auto found = std::ranges::find(sessions_, id, &Session::id);
    return found == sessions_.end() ? nullptr : &*found;
  }
  [[nodiscard]] auto selected() const -> const Match* {
    if (!location_.selected.has_value()) {
      return nullptr;
    }
    const auto found = std::ranges::find(matches_, *location_.selected, &Match::target);
    return found == matches_.end() ? nullptr : &*found;
  }
  [[nodiscard]] auto selected_index() const -> std::size_t {
    const auto* item = selected();
    return item == nullptr ? 0 : static_cast<std::size_t>(item - matches_.data());
  }
  void refocus() {
    const auto focused =
        command(client_, R"({"command":"surface.focus","surface":)" + selector(surface_) + '}');
    // An already-focused Surface emits no focused Event. Only ignore a queued resize blur when
    // an actual focus transition will finish the repair.
    repairing_ = text(focused, "status") == "applied";
  }
  void resize(std::size_t columns, std::size_t rows);
  [[nodiscard]] auto content_size() const -> ContentSize;
  [[nodiscard]] auto catalogue_ready() const -> bool;
  void reconcile(const JsonValue& list);
  void rebuild(bool choose_first = false);
  void validate_location();
  void add_match(Target target, std::string label, std::string detail, const std::string& search);
  void move(int step);
  void browse();
  void back();
  void refresh();
  void fetch();
  void reply(const ext::ClientRecord& record);
  static void outline(Session& target, const JsonValue& result);
  static void metadata(Session& target, const JsonValue& result);
  void paint();
  [[nodiscard]] auto activate() -> bool;
  [[nodiscard]] auto switch_session(std::string_view id) -> bool;
  [[nodiscard]] auto mutation(std::string_view request) -> bool;
  [[nodiscard]] auto event(const JsonValue& value) -> bool;
  [[nodiscard]] auto key(std::string_view value) -> bool;
  [[nodiscard]] auto consume() -> bool;
  void edit(std::string_view value);
  [[nodiscard]] auto turn() -> std::optional<int>;
  [[nodiscard]] auto wait_ready() -> bool;
  [[nodiscard]] auto drain_input() -> bool;
  [[nodiscard]] auto drain_data() -> bool;
};

void SessionManager::resize(std::size_t columns, std::size_t rows) {
  const auto content = location_.size.has_value() ? *location_.size : content_size();
  const auto next = layout(columns, rows, content, !message_.empty());
  if (!surface_.empty() && columns == columns_ && rows == rows_ &&
      next.surface == layout_.surface && next.list == layout_.list) {
    return;
  }
  columns_ = columns;
  rows_ = rows;
  layout_ = next;
  const auto& rect = layout_.surface;
  const auto placement = R"({"kind":"float","column":)" + std::to_string(rect.x) + R"(,"row":)" +
                         std::to_string(rect.y) + R"(,"columns":)" + std::to_string(rect.width) +
                         R"(,"rows":)" + std::to_string(rect.height) + '}';
  if (surface_.empty()) {
    const auto created =
        command(client_, R"({"command":"surface.create","placement":)" + placement + '}');
    surface_ = text(created, "surface");
  } else {
    static_cast<void>(command(client_, R"({"command":"surface.configure","surface":)" +
                                           selector(surface_) + R"(,"placement":)" + placement +
                                           '}'));
    refocus();
  }
  painted_.clear();
  dirty_ = true;
}

auto SessionManager::content_size() const -> ContentSize {
  auto count = candidate_count_;
  if (location_.scope.tab.empty()) {
    count = 0;
    for (const auto& item : sessions_) {
      count += item.tab_count;
    }
  }
  return {.width = catalogue_ready()
                       ? std::clamp<std::size_t>(label_width_ + detail_width_ + 6, 40, 96)
                       : 48,
          .rows = std::max<std::size_t>(1, count)};
}

auto SessionManager::catalogue_ready() const -> bool {
  return std::ranges::all_of(sessions_, [this](const Session& item) {
    if (!location_.scope.session.empty() && item.id != location_.scope.session) {
      return true;
    }
    return item.loaded && std::ranges::all_of(item.tabs, [](const Tab& tab) {
             return std::ranges::all_of(tab.panes, &Pane::inspected);
           });
  });
}

void SessionManager::reconcile(const JsonValue& list) {
  std::vector<Session> updated;
  for (const auto& value : list.array) {
    const auto id = text(value, "id");
    auto* previous = session(id);
    Session item = previous == nullptr ? Session{} : std::move(*previous);
    const auto revision = number(value, "revision");
    item.loaded = item.loaded && item.revision == revision;
    item.id = id;
    item.name = text(value, "name");
    item.active_tab = text(value, "active_tab");
    item.revision = revision;
    item.tab_count = number(value, "tabs");
    item.attached = enabled(value, "attached");
    updated.push_back(std::move(item));
  }
  sessions_ = std::move(updated);
  if (!location_.scope.session.empty() && session(location_.scope.session) == nullptr) {
    location_ = {};
    history_.clear();
    message_ = "Location closed";
  }
  rebuild();
}

void SessionManager::add_match(Target target, std::string label, std::string detail,
                               const std::string& search) {
  ++candidate_count_;
  label_width_ = std::max(label_width_, label.size());
  detail_width_ = std::max(detail_width_, detail.size());
  auto matched = fuzzy(search, location_.query);
  if (!matched.has_value()) {
    return;
  }
  auto highlights = fuzzy(label, location_.query);
  matches_.push_back({.target = std::move(target),
                      .label = std::move(label),
                      .detail = std::move(detail),
                      .highlights = highlights.has_value() ? std::move(highlights->positions)
                                                           : std::vector<std::size_t>{},
                      .score = matched->score});
}

// A deleted browsing scope returns to its surviving parent without activating a replacement.
void SessionManager::validate_location() {
  const auto* item = session(location_.scope.session);
  if (item == nullptr || !item->loaded || location_.scope.tab.empty() ||
      std::ranges::find(item->tabs, location_.scope.tab, &Tab::id) != item->tabs.end()) {
    return;
  }
  location_ = {};
  history_.clear();
  lost_selection_ = true;
  message_ = "Tab closed; choose another result";
}

// Builds a bounded search projection from cached metadata, with stable ties and ID-based selection.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void SessionManager::rebuild(bool choose_first) {
  validate_location();
  const auto previous = location_.selected;
  const auto searching = location_.query.find_first_not_of(' ') != std::string::npos;
  matches_.clear();
  candidate_count_ = 0;
  label_width_ = 0;
  detail_width_ = 0;
  for (const auto& item : sessions_) {
    if (!location_.scope.session.empty() && item.id != location_.scope.session) {
      continue;
    }
    for (const auto& tab : item.tabs) {
      if (!location_.scope.tab.empty() && tab.id != location_.scope.tab) {
        continue;
      }
      const auto name = tab.title.empty() ? "tab" : tab.title;
      const auto path = item.name + " / " + std::to_string(tab.position) + ':' + name;
      if (location_.scope.tab.empty()) {
        auto search = path;
        for (const auto& pane : tab.panes) {
          search += ' ' + pane.command + ' ' + pane.cwd;
        }
        const auto focused = std::ranges::find(tab.panes, tab.focused, &Pane::id);
        const auto* pane = tab.panes.empty() ? nullptr : &tab.panes.front();
        if (focused != tab.panes.end()) {
          pane = &*focused;
        }
        auto detail = pane == nullptr ? std::string{} : directory_label(pane->cwd);
        if (tab.panes.size() > 1) {
          detail += (detail.empty() ? "" : "  ") + std::to_string(tab.panes.size()) + " panes";
        }
        if (item.attached && item.id != current_) {
          detail += detail.empty() ? "[attached]" : "  [attached]";
        }
        const auto current = item.id == current_ && tab.id == item.active_tab;
        add_match({.session = item.id, .tab = tab.id, .pane = {}}, path + (current ? " *" : ""),
                  std::move(detail), search);
        continue;
      }
      for (std::size_t index = 0; index < tab.panes.size(); ++index) {
        const auto& pane = tab.panes.at(index);
        const auto label = std::to_string(index + 1) + " " + pane.command;
        add_match({.session = item.id, .tab = tab.id, .pane = pane.id}, label,
                  directory_label(pane.cwd), label + ' ' + pane.cwd);
      }
    }
  }
  if (!location_.size.has_value() && catalogue_ready()) {
    location_.size = content_size();
  }
  if (searching) {
    std::ranges::stable_sort(
        matches_, [](const Match& left, const Match& right) { return left.score > right.score; });
  }
  if (choose_first) {
    lost_selection_ = false;
    location_.selected.reset();
  } else if (previous.has_value() && selected() == nullptr) {
    const auto* item = session(previous->session);
    if (item == nullptr || item->loaded) {
      location_.selected.reset();
      lost_selection_ = true;
      message_ = "Selection disappeared; choose another result";
    }
  }
  if (!location_.selected.has_value() && !lost_selection_ && !matches_.empty()) {
    location_.selected = matches_.front().target;
  }
  dirty_ = true;
}

void SessionManager::move(int step) {
  if (matches_.empty()) {
    return;
  }
  const auto current = selected() == nullptr ? -1 : static_cast<std::int64_t>(selected_index());
  const auto index =
      std::clamp(current + step, std::int64_t{0}, static_cast<std::int64_t>(matches_.size() - 1));
  location_.selected = matches_.at(static_cast<std::size_t>(index)).target;
  lost_selection_ = false;
  message_.clear();
  dirty_ = true;
}
void SessionManager::browse() {
  const auto* chosen = selected();
  if (chosen == nullptr || !chosen->target.pane.empty()) {
    return;
  }
  const auto target = chosen->target;
  history_.push_back(location_);
  location_ = {
      .scope = target, .query = {}, .cursor = 0, .selected = std::nullopt, .size = std::nullopt};
  message_.clear();
  rebuild(true);
}
void SessionManager::back() {
  if (history_.empty()) {
    return;
  }
  location_ = std::move(history_.back());
  history_.pop_back();
  lost_selection_ = false;
  message_.clear();
  rebuild();
}
void SessionManager::refresh() {
  for (auto& item : sessions_) {
    item.loaded = false;
  }
  message_.clear();
  dirty_ = true;
}

// One outstanding data Proc; it never waits on the focused input connection. The selected
// Session takes priority for discovery and replies are checked against stable IDs/revisions.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void SessionManager::fetch() {
  if (job_.has_value()) {
    return;
  }
  std::vector<Session*> order;
  if (location_.selected.has_value()) {
    if (auto* item = session(location_.selected->session); item != nullptr) {
      order.push_back(item);
    }
  }
  for (auto& item : sessions_) {
    if (order.empty() || order.front() != &item) {
      order.push_back(&item);
    }
  }
  for (auto* item : order) {
    Job job{.kind = JobKind::outline,
            .sequence = 0,
            .session = item->id,
            .revision = item->revision,
            .deadline = Clock::now() + std::chrono::seconds(3)};
    const auto suffix = R"(,"session":)" + selector(item->id) + R"(,"if_session_revision":)" +
                        std::to_string(item->revision) + '}';
    std::string commands;
    if (!item->loaded) {
      commands = R"([{"command":"tab.list")" + suffix;
      commands += R"(,{"command":"pane.list")" + suffix + ']';
    } else {
      job.kind = JobKind::metadata;
      std::size_t pane_count = 0;
      for (const auto& tab : item->tabs) {
        for (const auto& pane : tab.panes) {
          if (pane.inspected || pane_count == 16) {
            continue;
          }
          ++pane_count;
          commands += commands.empty() ? "[" : ",";
          commands += R"({"command":"pane.inspect","pane":)";
          commands += selector(pane.id) + suffix;
        }
      }
      if (!commands.empty()) {
        commands += ']';
      }
    }
    if (!commands.empty()) {
      job.sequence = observer_.submit_proc(commands);
      job_ = std::move(job);
      return;
    }
  }
}

void SessionManager::outline(Session& target, const JsonValue& result) {
  const auto& values = member(result, "results").array;
  std::vector<Tab> tabs;
  for (const auto& value : member(member(values.at(0), "result"), "tabs").array) {
    tabs.push_back({.id = text(value, "id"),
                    .title = plain(text(value, "title")),
                    .focused = text(value, "focused_pane"),
                    .position = number(value, "position"),
                    .panes = {}});
  }
  for (const auto& value : member(member(values.at(1), "result"), "panes").array) {
    const auto found = std::ranges::find(tabs, text(value, "tab"), &Tab::id);
    if (found != tabs.end()) {
      auto name = plain(text(value, "observed_title"));
      found->panes.push_back({.id = text(value, "id"),
                              .command = name.empty() ? "process" : std::move(name),
                              .cwd = {}});
    }
  }
  target.tabs = std::move(tabs);
  target.loaded = true;
}

void SessionManager::metadata(Session& target, const JsonValue& result) {
  for (const auto& value : member(result, "results").array) {
    const auto& info = member(member(value, "result"), "pane_state");
    const auto id = text(member(info, "pane"), "id");
    for (auto& tab : target.tabs) {
      const auto found = std::ranges::find(tab.panes, id, &Pane::id);
      if (found == tab.panes.end()) {
        continue;
      }
      const auto& process = member(info, "process");
      found->cwd = plain(text(member(member(info, "terminal"), "pwd"), "value"));
      if (found->cwd.empty()) {
        found->cwd = plain(text(member(process, "launch"), "cwd"));
      }
      found->command = process_label(process);
      found->inspected = true;
    }
  }
}

void SessionManager::reply(const ext::ClientRecord& record) {
  if (!job_.has_value() || record.sequence != job_->sequence) {
    throw std::runtime_error("unexpected picker data reply");
  }
  const auto job = std::move(*job_);
  job_.reset();
  auto* item = session(job.session);
  if (item == nullptr || item->revision != job.revision) {
    return;
  }
  if (!enabled(record.document, "ok")) {
    // Do not spin on a failed or racing observation. A new revision or Ctrl-R retries discovery.
    item->loaded = true;
    for (auto& tab : item->tabs) {
      for (auto& pane : tab.panes) {
        pane.inspected = true;
      }
    }
    return;
  }
  if (job.kind == JobKind::outline) {
    outline(*item, record.document);
  } else {
    metadata(*item, record.document);
  }
  rebuild();
}

// Native composition retains these bounded row patches. Default backgrounds on the fill and
// border labels inherit the terminal theme; only selection supplies a contrasting background.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void SessionManager::paint() {
  resize(columns_, rows_);
  Grid grid(layout_.surface.width, layout_.surface.height);
  const auto& list = layout_.list;
  std::string title = "Session";
  if (const auto* item = session(location_.scope.session); item != nullptr) {
    title += " / " + item->name;
    for (const auto& tab : item->tabs) {
      if (tab.id == location_.scope.tab) {
        title +=
            " / " + std::to_string(tab.position) + ':' + (tab.title.empty() ? "tab" : tab.title);
      }
    }
  }
  grid.box(list, title);
  std::size_t cursor_column = 0;
  if (list.width >= 7 && list.height >= 5) {
    for (std::size_t x = 1; x + 1 < list.width; ++x) {
      grid.glyph(x, 2, "─", 1);
    }
    grid.put(2, 1, creating_ ? "+" : ">", 1, 2);
    const auto& query = creating_ ? new_name_ : location_.query;
    const auto cursor = creating_ ? new_cursor_ : location_.cursor;
    const auto count =
        list.width >= 18 ? std::to_string(matches_.size()) + '/' + std::to_string(candidate_count_)
                         : std::string{};
    const auto capacity = list.width - 5 - (count.empty() ? 0 : count.size() + 1);
    const auto offset = cursor >= capacity ? cursor - capacity + 1 : 0;
    const std::string placeholder = creating_ ? "New session" : "";
    grid.put(4, 1, query.empty() ? placeholder : query.substr(offset), capacity,
             query.empty() ? 1U : 0U);
    cursor_column = 4 + cursor - offset;
    grid.put(list.width - count.size() - 2, 1, count, count.size(), 1);
    const auto visible = list.height - 4;
    const auto selected_row = selected_index();
    const auto start = selected_row >= visible ? selected_row - visible + 1 : 0;
    const auto available = list.width - 4;
    const auto label_space =
        detail_width_ == 0
            ? available
            : std::min(label_width_, available - std::min(detail_width_, available / 2) - 2);
    for (std::size_t row = 0; row < visible && start + row < matches_.size(); ++row) {
      const auto& match = matches_.at(start + row);
      const bool active = location_.selected == match.target;
      const auto style = active ? 3U : 0U;
      grid.put(1, row + 3, std::string(list.width - 2, ' '), list.width - 2, style);
      grid.put(2, row + 3, match.label, label_space, style, match.highlights);
      if (!match.detail.empty()) {
        const auto detail_space = available - label_space - 2;
        grid.put(4 + label_space, row + 3, tail_label(match.detail, detail_space), detail_space,
                 active ? 3U : 1U);
      }
    }
    if (matches_.empty()) {
      grid.put(2, 3, catalogue_ready() ? "No matches" : "Loading...", list.width - 4, 1);
    }
  } else {
    grid.put(0, 0, "Esc close", list.width, 1);
  }
  if (layout_.surface.height > 1 && !message_.empty()) {
    grid.put(0, layout_.surface.height - 1, message_, layout_.surface.width, 1);
  }
  const std::string header =
      R"({"schema":"frame.surface-update/v1","surface":)" + ext::json_quote(surface_) +
      R"(,"styles":[{},{"faint":true},{"foreground":"#a7bbdf","bold":true},{"background":"#303847","foreground":"#e1e6ef","bold":true},{"bold":true,"underline":true},{"background":"#303847","foreground":"#a7bbdf","bold":true,"underline":true}],"rows":[)";
  std::string update = header;
  std::size_t nodes = 0;
  for (std::size_t row = 0; row < layout_.surface.height; ++row) {
    auto encoded = grid.row(row);
    if (row < painted_.size() && painted_.at(row) == encoded.text) {
      continue;
    }
    // Tall terminals and heavily highlighted lists must still respect the public parser budget.
    if (nodes + encoded.nodes > api::json_nodes_max - 64) {
      client_.update(update + "]}");
      update = header;
      nodes = 0;
    }
    if (nodes != 0) {
      update += ',';
    }
    nodes += encoded.nodes;
    update += encoded.text;
    if (row >= painted_.size()) {
      painted_.push_back(std::move(encoded.text));
    } else {
      painted_.at(row) = std::move(encoded.text);
    }
  }
  update += R"(],"cursor":{"row":)" + std::to_string(list.height >= 5 ? 1 : 0) + R"(,"column":)" +
            std::to_string(cursor_column) + R"(,"visible":)" +
            (list.width >= 7 && list.height >= 5 ? "true}}" : "false}}");
  client_.update(update);
  dirty_ = false;
}

[[nodiscard]] auto failure_reason(const JsonValue& result) -> std::string {
  const auto& values = member(result, "results").array;
  const auto& failure = values.empty() ? result : member(values.back(), "result");
  const auto* error = api::json_member(failure, "error");
  return error == nullptr ? text(failure, "status") : text(*error, "reason");
}

auto SessionManager::mutation(std::string_view request) -> bool {
  const auto result = client_.proc('[' + std::string(request) + ']');
  if (enabled(result, "ok")) {
    return true;
  }
  message_ = "Cannot switch: " + failure_reason(result);
  dirty_ = true;
  refocus();
  return false;
}

auto SessionManager::switch_session(std::string_view id) -> bool {
  if (id == current_) {
    return false;
  }
  const auto request = R"([{"command":"attachment.switch","connection":)" +
                       ext::json_quote(connection_) + R"(,"session":)" + selector(id) + "}]";
  // Only retry the uncommitted transfer, never a preceding tab/pane mutation.
  for (unsigned attempt = 0; attempt < 50; ++attempt) {
    const auto result = client_.proc(request);
    if (enabled(result, "ok")) {
      return false;
    }
    const auto reason = failure_reason(result);
    if (reason != "output_pending") {
      message_ = "Cannot switch: " + reason;
      refocus();
      dirty_ = true;
      return true;
    }
    static_cast<void>(::poll(nullptr, 0, 10));
  }
  message_ = "Output is busy; try again";
  refocus();
  dirty_ = true;
  return true;
}

auto SessionManager::activate() -> bool {
  const auto* choice = selected();
  if (choice == nullptr) {
    return true;
  }
  const auto target = choice->target;
  // Check availability before any selection mutation, including results found below a Session.
  const auto inspected =
      client_.proc(R"([{"command":"session.inspect","session":)" + selector(target.session) + "}]");
  if (!enabled(inspected, "ok")) {
    refresh();
    message_ = "Cannot switch: target disappeared";
    return true;
  }
  const auto& state =
      member(member(member(inspected, "results").array.front(), "result"), "session_state");
  if (target.session != current_ && number(member(state, "attachments"), "connected") != 0) {
    message_ = "Cannot switch: target_attached";
    dirty_ = true;
    return true;
  }
  if (!target.tab.empty()) {
    const auto suffix = R"(,"session":)" + selector(target.session);
    std::string commands = R"({"command":"tab.select","tab":)" + selector(target.tab) + suffix +
                           R"(,"if_session_revision":)" +
                           std::to_string(number(state, "revision")) + '}';
    if (!target.pane.empty()) {
      // Validate the pane first so an already stale pane cannot select its surviving parent tab.
      const auto pane = client_.proc(R"([{"command":"pane.inspect","pane":)" +
                                     selector(target.pane) + suffix + "}]");
      if (!enabled(pane, "ok")) {
        message_ = "Cannot switch: pane disappeared";
        dirty_ = true;
        return true;
      }
      commands += R"(,{"command":"pane.focus","pane":)" + selector(target.pane) + suffix + '}';
    }
    if (!mutation(commands)) {
      return true;
    }
  }
  return switch_session(target.session);
}

// Query editing operates only on bounded, printable display text; transport chunks are not keys.
void SessionManager::edit(std::string_view value) {
  if (!location_.size.has_value()) {
    location_.size = content_size();
  }
  auto& query = creating_ ? new_name_ : location_.query;
  auto& cursor = creating_ ? new_cursor_ : location_.cursor;
  const auto limit = creating_ ? limits::session_name_bytes_max : limits::search_query_bytes_max;
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (query.size() == limit) {
      break;
    }
    if (byte >= 32 && byte < 127) {
      query.insert(cursor++, 1, static_cast<char>(byte));
    }
  }
  if (!creating_) {
    rebuild(true);
  }
  message_.clear();
  dirty_ = true;
}

// Explicit editor/navigation actions keep printable j/k/n/q available to fuzzy queries.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto SessionManager::key(std::string_view value) -> bool {
  if (!location_.size.has_value()) {
    location_.size = content_size();
  }
  auto& query = creating_ ? new_name_ : location_.query;
  auto& cursor = creating_ ? new_cursor_ : location_.cursor;
  if (value == "escape" || value == "\x03" || value == "\x07") {
    if (!creating_) {
      return false;
    }
    creating_ = false;
    new_name_.clear();
    new_cursor_ = 0;
  } else if (value == "enter" || value == "\r" || value == "\n") {
    if (!creating_) {
      return activate();
    }
    const auto result =
        client_.proc(R"([{"command":"session.start","name":)" + ext::json_quote(new_name_) + "}]");
    if (enabled(result, "ok")) {
      return switch_session(
          text(member(member(member(result, "results").array.front(), "result"), "session"), "id"));
    }
    message_ = "Session name unavailable or invalid";
  } else if (value == "down" || value == "\x0e") {
    if (!creating_) {
      move(1);
    }
  } else if (value == "up" || value == "\x10") {
    if (!creating_) {
      move(-1);
    }
  } else if (value == "tab" || value == "\t") {
    if (!creating_) {
      browse();
    }
  } else if (value == "backtab") {
    if (!creating_) {
      back();
    }
  } else if (value == "\x12") {
    refresh();
  } else if (value == "\x0f") {
    creating_ = true;
    new_name_.clear();
    new_cursor_ = 0;
  } else if (value == "left") {
    cursor = cursor > 0 ? cursor - 1 : 0;
  } else if (value == "right") {
    cursor = std::min(cursor + 1, query.size());
  } else if (value == "home" || value == "\x01") {
    cursor = 0;
  } else if (value == "end" || value == "\x05") {
    cursor = query.size();
  } else if (value == "backspace" || value == "\x7f" || value == "\b") {
    if (cursor > 0) {
      query.erase(--cursor, 1);
      if (!creating_) {
        rebuild(true);
      }
    }
  } else if (value == "delete") {
    if (cursor < query.size()) {
      query.erase(cursor, 1);
      if (!creating_) {
        rebuild(true);
      }
    }
  } else if (value == "\x15") {
    query.erase(0, cursor);
    cursor = 0;
    if (!creating_) {
      rebuild(true);
    }
  } else if (value == "\x17") {
    const auto end = cursor;
    while (cursor > 0 && query.at(cursor - 1) == ' ') {
      --cursor;
    }
    while (cursor > 0 && query.at(cursor - 1) != ' ') {
      --cursor;
    }
    query.erase(cursor, end - cursor);
    if (!creating_) {
      rebuild(true);
    }
  } else {
    edit(value);
  }
  dirty_ = true;
  return true;
}

[[nodiscard]] auto bytes(const JsonValue& event) -> std::string {
  if (api::json_member(event, "text") != nullptr) {
    return text(event, "text");
  }
  const auto hex = text(event, "bytes_hex");
  std::string result;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    unsigned byte = 0;
    const auto digits = std::string_view(hex).substr(index, 2);
    // from_chars consumes a bounded iterator pair, not a C string.
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    if (std::from_chars(digits.data(), std::to_address(digits.end()), byte, 16).ec != std::errc{}) {
      throw std::runtime_error("invalid picker input");
    }
    result += static_cast<char>(byte);
  }
  return result;
}

// Decode a bounded transport chunk, retaining partial escape sequences.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto SessionManager::consume() -> bool {
  while (!pending_.empty()) {
    if (pending_.front() != '\x1b') {
      const auto value = pending_.substr(0, 1);
      pending_.erase(0, 1);
      if (!key(value)) {
        return false;
      }
      continue;
    }
    if (pending_.size() < 2) {
      break;
    }
    const auto end = pending_.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ~", 2);
    if (end == std::string::npos) {
      break;
    }
    const auto sequence = pending_.substr(0, end + 1);
    pending_.erase(0, end + 1);
    constexpr std::array sequences{std::pair{"\x1b[A", "up"},      std::pair{"\x1b[B", "down"},
                                   std::pair{"\x1b[C", "right"},   std::pair{"\x1b[D", "left"},
                                   std::pair{"\x1b[Z", "backtab"}, std::pair{"\x1b[H", "home"},
                                   std::pair{"\x1b[F", "end"},     std::pair{"\x1b[3~", "delete"},
                                   std::pair{"\x1bOA", "up"},      std::pair{"\x1bOB", "down"}};
    for (const auto& [encoded, logical] : sequences) {
      if (sequence == encoded && !key(logical)) {
        return false;
      }
    }
  }
  if (!pending_.empty()) {
    escape_deadline_ = Clock::now() + std::chrono::milliseconds(50);
  }
  return true;
}

// Physical keys and legacy byte input are two representations of the same user actions.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto SessionManager::event(const JsonValue& value) -> bool {
  if (const auto* presentation = api::json_member(value, "presentation"); presentation != nullptr) {
    if (text(*presentation, "connection") != connection_) {
      return false;
    }
    resize(number(*presentation, "columns"), number(*presentation, "rows"));
  }
  if (text(value, "surface") != surface_) {
    return true;
  }
  const auto type = text(value, "event");
  if (type == "surface.closed") {
    return false;
  }
  if (type == "surface.focused") {
    repairing_ = false;
  }
  if (type == "surface.blurred" && !repairing_) {
    // A shrinking viewport suspends an oversized float before publishing its new presentation.
    // Repair that suspension, but honor an ordinary focus loss/native recovery immediately.
    const auto inspected =
        command(client_, R"({"command":"session.inspect","session":)" + selector(current_) + '}');
    const auto& geometry = member(member(inspected, "session_state"), "geometry");
    if (number(geometry, "columns") == columns_ && number(geometry, "rows") == rows_) {
      return false;
    }
    resize(number(geometry, "columns"), number(geometry, "rows"));
  }
  if (type == "surface.paste") {
    edit(bytes(value));
    return true;
  }
  if (type == "surface.mouse" && number(value, "action") == 0 && number(value, "button") == 1) {
    const auto row = api::json_unsigned(value, "row");
    const auto column = api::json_unsigned(value, "column");
    const auto& list = layout_.list;
    if (row && column && *row >= 3 && *row < list.height - 1 && *column > 0 &&
        *column < list.width - 1) {
      const auto visible = list.height - 4;
      const auto start = selected_index() >= visible ? selected_index() - visible + 1 : 0;
      const auto index = start + static_cast<std::size_t>(*row) - 3;
      if (index < matches_.size()) {
        location_.selected = matches_.at(index).target;
        lost_selection_ = false;
        message_.clear();
        dirty_ = true;
      }
    }
  }
  if (type != "surface.key" || number(value, "action") == 0) {
    return true;
  }
  const auto physical = number(value, "key");
  const auto modifiers = number(value, "modifiers");
  if ((modifiers & 2U) != 0 && physical >= 1 && physical <= 26) {
    pending_.clear();
    return key(std::string(1, static_cast<char>(physical)));
  }
  constexpr std::array keys{
      std::pair{27U, "enter"},  std::pair{28U, "tab"},   std::pair{29U, "backspace"},
      std::pair{30U, "escape"}, std::pair{32U, "up"},    std::pair{33U, "down"},
      std::pair{34U, "left"},   std::pair{35U, "right"}, std::pair{36U, "home"},
      std::pair{37U, "end"},    std::pair{39U, "delete"}};
  for (const auto& [code, logical] : keys) {
    if (physical == code) {
      pending_.clear();
      return key(physical == 28 && (modifiers & 1U) != 0 ? "backtab" : logical);
    }
  }
  pending_ += bytes(value);
  if (pending_.size() > limits::extension_input_bytes_max) {
    return false;
  }
  return consume();
}

// No polling timer while idle. Data work and partial legacy escapes have finite deadlines.
auto SessionManager::turn() -> std::optional<int> {
  if (dirty_) {
    paint();
  }
  fetch();
  if (!wait_ready()) {
    return 1;
  }
  if (!pending_.empty() && Clock::now() >= escape_deadline_) {
    pending_.clear();
    if (!key("escape")) {
      return 0;
    }
  }
  if (job_.has_value() && Clock::now() >= job_->deadline) {
    throw std::runtime_error("picker data request timed out");
  }
  if (!drain_input()) {
    return 0;
  }
  if (!drain_data()) {
    return 1;
  }
  return std::nullopt;
}

auto SessionManager::wait_ready() -> bool {
  auto deadline = job_.has_value() ? job_->deadline : Clock::time_point::max();
  if (!pending_.empty()) {
    deadline = std::min(deadline, escape_deadline_);
  }
  int timeout =
      deadline == Clock::time_point::max()
          ? -1
          : static_cast<int>(std::max<std::int64_t>(
                0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
                       .count()));
  if (client_.ready() || observer_.ready()) {
    timeout = 0;
  }
  std::array<pollfd, 2> descriptors{
      {{.fd = client_.descriptor(), .events = POLLIN, .revents = 0},
       {.fd = observer_.descriptor(), .events = POLLIN, .revents = 0}}};
  return ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), timeout) >= 0 ||
         errno == EINTR;
}

auto SessionManager::drain_input() -> bool {
  for (unsigned count = 0; count < 32; ++count) {
    auto record = client_.next(0);
    if (!record.has_value()) {
      break;
    }
    if (record->kind == ext::RecordKind::error || !event(record->document)) {
      return false;
    }
  }
  return true;
}

auto SessionManager::drain_data() -> bool {
  for (unsigned count = 0; count < 32; ++count) {
    auto record = observer_.next(0);
    if (!record.has_value()) {
      break;
    }
    if (record->kind == ext::RecordKind::proc_result) {
      reply(*record);
    } else if (const auto* list = api::json_member(record->document, "sessions"); list != nullptr) {
      reconcile(*list);
    } else if (record->kind == ext::RecordKind::error) {
      return false;
    }
  }
  return true;
}

auto SessionManager::run() -> int {
  while (true) {
    if (const auto result = turn(); result.has_value()) {
      return *result;
    }
  }
}

} // namespace

auto run_session_manager(const api::JsonValue& context) -> int {
  return SessionManager(context).run();
}
} // namespace frame::user
