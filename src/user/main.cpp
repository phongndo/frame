#include "api/json.hpp"
#include "extension/client.hpp"
#include "extension/protocol.hpp"
#include "lemma/limits.hpp"
#include "render/status_line.hpp"
#include "render/ui.hpp"
#include "user/attention.hpp"
#include "user/session_manager.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

namespace {
namespace api = lemma::api;
namespace ext = lemma::extension;
namespace render = lemma::render;
using api::JsonValue;

void report_status_error(const std::string_view context, const std::exception& error) {
  // Bounded diagnostic strings do not need the generic formatter and its runtime mappings.
  const auto detail = std::string_view(error.what()).substr(0, 180);
  static_cast<void>(std::fwrite(context.data(), 1, context.size(), stderr));
  static_cast<void>(std::fwrite(detail.data(), 1, detail.size(), stderr));
  static_cast<void>(std::fputc('\n', stderr));
}

[[nodiscard]] auto member(const JsonValue& value, const std::string_view name) -> const JsonValue& {
  const auto* const found = api::json_member(value, name);
  if (found == nullptr) {
    throw std::runtime_error("missing extension response field");
  }
  return *found;
}

[[nodiscard]] auto text(const JsonValue& value, const std::string_view name) -> std::string_view {
  return api::json_string(value, name).value_or(std::string_view{});
}

[[nodiscard]] auto number(const JsonValue& value, const std::string_view name) -> std::uint64_t {
  return api::json_unsigned(value, name).value_or(0);
}

[[nodiscard]] auto enabled(const JsonValue& value, const std::string_view name) -> bool {
  return api::json_boolean(value, name).value_or(false);
}

[[nodiscard]] auto selector(const std::string_view value) -> std::string {
  return "{\"id\":" + ext::json_quote(value) + '}';
}

[[nodiscard]] auto command(ext::Client& client, const std::string_view document) -> JsonValue {
  auto result = client.proc('[' + std::string(document) + ']');
  const auto& results = member(result, "results");
  if (!enabled(result, "ok") || results.array.size() != 1U) {
    throw std::runtime_error("extension command failed: " + ext::json_encode(result));
  }
  return member(results.array.front(), "result");
}

[[nodiscard]] auto hello(const std::string_view session, const bool presentation) -> std::string {
  return std::string{
             R"({"schema":"lemma.extension/v1","name":"lemma-ui","capabilities":["observe","proc","surface"],"events":{"schema":"lemma.events/v1","session":)"} +
         selector(session) + (presentation ? R"(,"presentation":true,"signals":true}})" : "}}");
}

[[nodiscard]] auto plain(const std::string_view value) -> std::string {
  std::string result;
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    result += byte >= 32U && byte < 127U ? static_cast<char>(byte) : '?';
  }
  return result;
}

using Markers = std::array<lemma::user::Marker, render::status_tabs_max>;

struct Status final {
  ext::Client client;
  std::string session;
  std::string surface;
  JsonValue state;
  lemma::user::TabAttention attention;
  std::optional<std::uint64_t> revision;
  Markers painted{};
  std::string prompt_text;
  bool prompting{false};
  std::string dragged;
  std::optional<std::size_t> drag_position;
  std::vector<JsonValue> preview;

  Status(const std::string_view endpoint, std::string id)
      : client(endpoint, hello(id, true)), session(std::move(id)) {}

  [[nodiscard]] auto shown_tabs() const -> const std::vector<JsonValue>& {
    const auto& values = preview.empty() ? member(state, "tabs").array : preview;
    if (values.size() > render::status_tabs_max) {
      throw std::runtime_error("too many status tabs");
    }
    return values;
  }

  [[nodiscard]] auto markers() const -> Markers {
    Markers result{};
    const auto& values = shown_tabs();
    for (std::size_t index = 0; index < values.size(); ++index) {
      result.at(index) = attention.marker(text(values.at(index), "id"));
    }
    return result;
  }

  // Bounded UI projection/interaction branches have one owner.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto projection(std::array<render::StatusTab, render::status_tabs_max>& tabs,
                                const Markers& marks, std::string& context) const
      -> render::StatusLine {
    const auto& values = shown_tabs();
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto& value = values.at(index);
      const bool active = enabled(value, "active");
      tabs.at(index) = {.number = static_cast<std::uint16_t>(number(value, "position")),
                        .title = text(value, "title"),
                        .active = active,
                        .attention = active ? std::string_view{} : marks.at(index).view()};
    }
    const auto& prompt = member(state, "prompt");
    const auto kind = text(prompt, "kind");
    constexpr std::array names{"none",           "session",         "tab",    "command",
                               "search.forward", "search.backward", "message"};
    const auto* const found = std::ranges::find(names, kind);
    if (found == names.end()) {
      throw std::runtime_error("unknown editor kind");
    }
    const auto target = static_cast<render::StatusPromptTarget>(found - names.begin());
    const auto feedback = text(prompt, "feedback");
    auto prompt_feedback = render::StatusPromptFeedback::none;
    if (feedback == "invalid") {
      prompt_feedback = render::StatusPromptFeedback::invalid;
    } else if (feedback == "conflict") {
      prompt_feedback = render::StatusPromptFeedback::conflict;
    }
    context = text(state, "mode");
    const auto& copy = member(state, "copy");
    if (enabled(copy, "active")) {
      if (enabled(copy, "searching")) {
        context += enabled(copy, "backward") ? " ?" : " /";
        context += plain(text(copy, "query"));
      } else {
        context += " [" + std::to_string(number(copy, "below")) + '/' +
                   std::to_string(number(copy, "history")) + ']';
        constexpr std::array messages{
            "", "no match", "empty", "clipboard busy", "selection too large", "copy failed"};
        constexpr std::array feedback_names{"none",           "no_match",  "empty_selection",
                                            "clipboard_busy", "too_large", "failed"};
        const auto copy_feedback = static_cast<std::size_t>(
            std::ranges::find(feedback_names, text(copy, "feedback")) - feedback_names.begin());
        if (copy_feedback > 0 && copy_feedback < messages.size()) {
          context += ' ';
          context += messages.at(copy_feedback);
        }
      }
    }
    if (target == render::StatusPromptTarget::command_line ||
        target == render::StatusPromptTarget::copy_search_forward ||
        target == render::StatusPromptTarget::copy_search_backward) {
      context.clear();
    } else if (target == render::StatusPromptTarget::message) {
      context = text(state, "message");
    }
    return {.session_name = text(state, "name"),
            .tabs = std::span(tabs).first(values.size()),
            .prompt_target = target,
            .prompt_feedback = prompt_feedback,
            .prompt_value = prompt_text,
            .input_context = context,
            .prompt_cursor = number(prompt, "cursor"),
            .dirty = true};
  }

  // Bounded UI projection/interaction branches have one owner.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void paint() {
    const auto columns = static_cast<std::uint16_t>(number(state, "columns"));
    const auto rows = static_cast<std::uint16_t>(number(state, "rows"));
    if (columns == 0 || rows < 2 || columns > lemma::limits::terminal_columns_hard_max) {
      return;
    }
    if (surface.empty()) {
      const auto created = command(
          client,
          R"({"command":"surface.create","focusable":false,"placement":{"kind":"dock.top","size":1}})");
      surface = text(created, "surface");
    }
    std::array<render::StatusTab, render::status_tabs_max> tabs{};
    std::string context;
    painted = markers();
    const auto status = projection(tabs, painted, context);
    prompting = status.prompting();
    std::array<render::ui::Cell, lemma::limits::terminal_columns_hard_max> storage{};
    auto cells = std::span(storage).first(columns);
    std::uint16_t cursor = 0;
    if (!render::project_status_cells(status, {.columns = columns, .rows = rows}, cells, cursor)) {
      throw std::runtime_error("invalid status presentation");
    }
    std::string update =
        R"({"schema":"lemma.surface-update/v1","surface":)" + ext::json_quote(surface) +
        R"(,"styles":[{}, {"bold":true},{"bold":true,"underline":true}],"rows":[{"row":0,"runs":[)";
    std::size_t column = 0;
    bool separator = false;
    while (column < cells.size()) {
      const auto first = column;
      const auto style = cells.subspan(column, 1).front().style.attributes;
      std::string run;
      while (column < cells.size() && cells.subspan(column, 1).front().style.attributes == style) {
        const auto& cell = cells.subspan(column, 1).front();
        if (cell.text_size == 0) {
          run += ' ';
        } else {
          run.append(cell.text.data(), cell.text_size);
        }
        ++column;
      }
      if (separator) {
        update += ',';
      }
      separator = true;
      const auto style_index =
          (style & render::ui::attribute_underline) != 0 ? 2 : static_cast<int>(style != 0);
      update += R"({"column":)" + std::to_string(first) + R"(,"text":)" + ext::json_quote(run) +
                R"(,"style":)" + std::to_string(style_index) + '}';
    }
    update += R"(]}],"cursor":{"row":0,"column":)" + std::to_string(cursor) + R"(,"visible":)" +
              (prompting ? "true}}" : "false}}");
    client.update(update);
  }

  // Bounded UI projection/interaction branches have one owner.
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void mouse(const JsonValue& event) {
    if (prompting || surface.empty() || number(event, "button") != 1) {
      return;
    }
    const auto column = api::json_unsigned(event, "column");
    if (!column.has_value() || *column >= number(state, "columns")) {
      return;
    }
    std::array<render::StatusTab, render::status_tabs_max> tabs{};
    std::string context;
    const auto hit = render::status_target_at_column(
        projection(tabs, painted, context),
        {.columns = static_cast<std::uint16_t>(number(state, "columns")),
         .rows = static_cast<std::uint16_t>(number(state, "rows"))},
        static_cast<std::uint16_t>(*column));
    const auto action = number(event, "action");
    if (action == 0 && hit.has_value()) {
      if (hit->kind == render::StatusTargetKind::create_tab) {
        static_cast<void>(
            command(client, R"({"command":"tab.new","session":)" + selector(session) + '}'));
        return;
      }
      preview = member(state, "tabs").array;
      dragged = text(preview.at(hit->tab_position), "id");
      drag_position = hit->tab_position;
      static_cast<void>(command(client, R"({"command":"tab.select","session":)" +
                                            selector(session) + R"(,"tab":)" + selector(dragged) +
                                            '}'));
    } else if (action == 2 && !dragged.empty() && hit.has_value()) {
      drag_position = hit->kind == render::StatusTargetKind::create_tab ? preview.size() - 1U
                                                                        : hit->tab_position;
      const auto source = std::ranges::find_if(
          preview, [&](const auto& tab) { return text(tab, "id") == dragged; });
      if (source != preview.end()) {
        auto tab = std::move(*source);
        preview.erase(source);
        preview.insert(preview.begin() + static_cast<std::ptrdiff_t>(*drag_position),
                       std::move(tab));
        paint();
      }
    } else if (action == 1 && !dragged.empty()) {
      if (drag_position.has_value()) {
        static_cast<void>(command(client, R"({"command":"tab.move","session":)" +
                                              selector(session) + R"(,"tab":)" + selector(dragged) +
                                              R"(,"to_position":)" +
                                              std::to_string(*drag_position + 1U) + '}'));
      }
      dragged.clear();
      drag_position.reset();
      preview.clear();
    }
  }

  // Pane->Tab membership changes only with the Session revision; a signal from an unlisted Pane
  // means the listing is older than that Pane.
  void list_panes() {
    const auto listed =
        command(client, R"({"command":"pane.list","session":)" + selector(session) + '}');
    const auto& values = member(listed, "panes").array;
    std::vector<lemma::user::PaneMember> panes;
    panes.reserve(values.size());
    for (const auto& value : values) {
      panes.push_back({.pane = text(value, "id"),
                       .tab = text(value, "tab"),
                       .signals = lemma::user::decode_signals(member(value, "signals"))});
    }
    attention.list(panes);
  }

  void signal(const JsonValue& document) {
    const auto pane = text(document, "pane");
    const auto signals = lemma::user::decode_signals(member(document, "signals"));
    if (!attention.signal(pane, signals)) {
      list_panes();
      static_cast<void>(attention.signal(pane, signals));
    }
    if (!state.object.empty() && markers() != painted) {
      paint();
    }
  }

  void observe_revision(const JsonValue& document) {
    const auto* const sessions = api::json_member(document, "sessions");
    if (sessions == nullptr || sessions->array.size() != 1U) {
      return;
    }
    const auto current = number(sessions->array.front(), "revision");
    if (revision != current) {
      revision = current;
      list_panes();
    }
  }

  void visit_active() {
    for (const auto& tab : member(state, "tabs").array) {
      if (enabled(tab, "active")) {
        attention.visit(text(tab, "id"));
      }
    }
  }

  // A drag preview survives only while the presented Tabs are the same set.
  void reconcile_preview(const JsonValue& presentation) {
    if (preview.empty()) {
      return;
    }
    const auto& values = member(presentation, "tabs").array;
    if (values.size() != preview.size() || std::ranges::any_of(preview, [&](const auto& tab) {
          return std::ranges::none_of(
              values, [&](const auto& value) { return text(tab, "id") == text(value, "id"); });
        })) {
      dragged.clear();
      drag_position.reset();
      preview.clear();
    } else {
      for (auto& tab : preview) {
        const auto found = std::ranges::find_if(
            values, [&](const auto& value) { return text(tab, "id") == text(value, "id"); });
        tab = *found;
      }
    }
  }

  void event(const JsonValue& document) {
    if (const auto* const presentation = api::json_member(document, "presentation");
        presentation != nullptr) {
      // List before visiting: signals that preceded a Tab switch were seen on the Tab being left.
      observe_revision(document);
      reconcile_preview(*presentation);
      state = *presentation;
      prompt_text = plain(text(member(state, "prompt"), "value"));
      visit_active();
      paint();
    } else if (text(document, "event") == "pane.signal") {
      signal(document);
    } else if (text(document, "event") == "surface.mouse") {
      mouse(document);
    } else if (text(document, "event") == "surface.resized" && !state.object.empty()) {
      paint();
    }
  }
};

// Attention belongs to each live Session rather than its status connection, so a reattached
// statusline marks what happened while detached. Sessions in the first discovery snapshot existed
// before lemma-ui and take their first listing as seen; later Sessions are observed from creation.
using RetainedAttention = std::map<std::string, lemma::user::TabAttention, std::less<>>;

// Discovery observes bounded Session summaries; only attached Sessions own status Surfaces.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void discover_statuses(const std::string_view endpoint, const JsonValue& document,
                       std::vector<std::unique_ptr<Status>>& statuses, RetainedAttention& retained,
                       bool& discovered) {
  const auto* const sessions = api::json_member(document, "sessions");
  if (sessions != nullptr) {
    for (const auto& session : sessions->array) {
      const auto id = text(session, "id");
      if (!retained.contains(id) && std::ranges::none_of(statuses, [&](const auto& status) {
            return status->session == id;
          })) {
        retained.emplace(id, discovered ? lemma::user::TabAttention::since_creation()
                                        : lemma::user::TabAttention{});
      }
    }
    discovered = true;
    std::erase_if(statuses, [&](const auto& status) {
      const bool detached = std::ranges::none_of(sessions->array, [&](const auto& session) {
        return text(session, "id") == status->session && enabled(session, "attached");
      });
      if (detached) {
        retained.insert_or_assign(status->session, std::move(status->attention));
      }
      return detached;
    });
    std::erase_if(retained, [&](const auto& entry) {
      return std::ranges::none_of(
          sessions->array, [&](const auto& session) { return text(session, "id") == entry.first; });
    });
    for (const auto& session : sessions->array) {
      const auto id = text(session, "id");
      if (enabled(session, "attached") && std::ranges::none_of(statuses, [&](const auto& status) {
            return status->session == id;
          })) {
        try {
          auto status = std::make_unique<Status>(endpoint, std::string(id));
          if (const auto found = retained.find(id); found != retained.end()) {
            status->attention = std::move(found->second);
            status->attention.detach();
            retained.erase(found);
          }
          statuses.push_back(std::move(status));
        } catch (const std::exception& error) {
          report_status_error("status admission: ", error);
        }
      }
    }
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto run_status(const std::string_view endpoint) -> int {
  ext::Client observer(
      endpoint,
      R"({"schema":"lemma.extension/v1","name":"lemma-ui-discovery","capabilities":["observe"],"events":{"schema":"lemma.events/v1"}})");
  std::vector<std::unique_ptr<Status>> statuses;
  RetainedAttention retained;
  bool discovered = false;
  while (true) {
    std::vector<pollfd> descriptors{{.fd = observer.descriptor(), .events = POLLIN, .revents = 0}};
    bool ready = observer.ready();
    for (const auto& status : statuses) {
      descriptors.push_back({.fd = status->client.descriptor(), .events = POLLIN, .revents = 0});
      ready = ready || status->client.ready();
    }
    const auto polled =
        ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), ready ? 0 : -1);
    if (polled < 0 && errno == EINTR) {
      continue;
    }
    if (polled < 0) {
      return 1;
    }
    if (observer.ready() || descriptors.front().revents != 0) {
      if (auto record = observer.next(0); record.has_value()) {
        discover_statuses(endpoint, record->document, statuses, retained, discovered);
      }
    }
    // Discovery can replace the vector, so use nonblocking receives rather than stale poll indices.
    std::erase_if(statuses, [&](auto& status) {
      bool failed = false;
      try {
        if (auto record = status->client.next(0); record.has_value()) {
          failed = record->kind == ext::RecordKind::error;
          if (!failed) {
            status->event(record->document);
          }
        }
      } catch (const std::exception& error) {
        report_status_error("status: ", error);
        failed = true;
      }
      if (failed) {
        retained.insert_or_assign(status->session, std::move(status->attention));
      }
      return failed;
    });
  }
}

} // namespace

int main(const int argc, char** argv) {
  try {
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    const char* const endpoint = std::getenv("LEMMA_EXTENSION_ENDPOINT");
    if (arguments.size() == 2 && std::string_view(arguments.back()) == "status" &&
        endpoint != nullptr) {
      return run_status(endpoint);
    }
    const auto* context_text = std::getenv("LEMMA_COMMAND_CONTEXT");
    if (arguments.size() == 2 && std::string_view(arguments.back()) == "sessions" &&
        context_text != nullptr) {
      const auto context = api::parse_json(context_text);
      if (context.value.has_value()) {
        return lemma::user::run_session_manager(*context.value);
      }
    }
    return 2;
  } catch (const std::exception& error) {
    const std::string_view message(error.what());
    static_cast<void>(
        ::write(STDERR_FILENO, message.data(), std::min<std::size_t>(180, message.size())));
    static_cast<void>(::write(STDERR_FILENO, "\n", 1));
    return 1;
  }
}
