#include "extension/runtime.hpp"

#include "api/command.hpp"
#include "api/json.hpp"
#include "extension/protocol.hpp"
#include "frame/assert.hpp"
#include "frame/geometry.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"
#include "render/grid.hpp"
#include "render/scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace frame::extension {

// Bounds and optional engagement are validated before fixed-capacity access; allocating operations
// catch locally.
// NOLINTBEGIN(bugprone-exception-escape,bugprone-unchecked-optional-access)

static_assert(sizeof(Runtime) <= std::size_t{256} * 1'024U);

namespace {

[[nodiscard]] constexpr auto next_generation(const std::uint32_t generation) noexcept
    -> std::uint32_t {
  return generation == std::numeric_limits<std::uint32_t>::max() ? 1U : generation + 1U;
}

template <typename Id>
[[nodiscard]] auto parse_id(const std::string_view value) noexcept -> std::optional<Id> {
  const auto separator = value.find(':');
  if (separator == 0 || separator == std::string_view::npos || separator + 1U == value.size()) {
    return std::nullopt;
  }
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;
  const auto first = value.substr(0, separator);
  const auto second = value.substr(separator + 1U);
  const auto slot_result = std::from_chars(first.begin(), first.end(), slot);
  const auto generation_result = std::from_chars(second.begin(), second.end(), generation);
  return slot_result.ec == std::errc{} && slot_result.ptr == first.end() &&
                 generation_result.ec == std::errc{} && generation_result.ptr == second.end()
             ? Id::try_from_parts(slot, generation)
             : std::nullopt;
}

template <typename Id> [[nodiscard]] auto id_text(const Id id) -> std::string {
  return std::to_string(id.slot()) + ":" + std::to_string(id.generation());
}

[[nodiscard]] auto known_fields(const api::JsonValue& object,
                                const std::initializer_list<std::string_view> fields) noexcept
    -> bool {
  return object.kind == api::JsonKind::object &&
         std::ranges::all_of(object.object, [&](const api::JsonMember& member) {
           return std::ranges::find(fields, std::string_view(member.key)) != fields.end();
         });
}

[[nodiscard]] auto decode_color(const api::JsonValue& object, const std::string_view field) noexcept
    -> std::optional<render::GridColor> {
  const auto value = api::json_string(object, field);
  if (!value.has_value() || value->size() != 7U || value->front() != '#') {
    return std::nullopt;
  }
  std::uint32_t color = 0;
  const auto digits = value->substr(1);
  const auto result = std::from_chars(digits.begin(), digits.end(), color, 16);
  if (result.ec != std::errc{} || result.ptr != digits.end()) {
    return std::nullopt;
  }
  return render::GridColor{.red = static_cast<std::uint8_t>((color >> 16U) & 0xffU),
                           .green = static_cast<std::uint8_t>((color >> 8U) & 0xffU),
                           .blue = static_cast<std::uint8_t>(color & 0xffU)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto decode_styles(const api::JsonValue& document)
    -> std::optional<std::optional<std::vector<render::GridStyle>>> {
  const auto* const encoded = api::json_member(document, "styles");
  if (encoded == nullptr) {
    return std::optional<std::vector<render::GridStyle>>{};
  }
  if (encoded->kind != api::JsonKind::array || encoded->array.empty() ||
      encoded->array.size() > limits::surface_styles_max) {
    return std::nullopt;
  }
  std::vector<render::GridStyle> styles;
  styles.reserve(encoded->array.size());
  for (const auto& value : encoded->array) {
    if (!known_fields(value, {"foreground", "background", "bold", "faint", "italic", "underline",
                              "inverse"})) {
      return std::nullopt;
    }
    render::GridStyle style;
    if (api::json_member(value, "foreground") != nullptr) {
      style.foreground = decode_color(value, "foreground");
      if (!style.foreground.has_value()) {
        return std::nullopt;
      }
    }
    if (api::json_member(value, "background") != nullptr) {
      style.background = decode_color(value, "background");
      if (!style.background.has_value()) {
        return std::nullopt;
      }
    }
    for (const auto [field, destination] :
         std::array{std::pair{std::string_view{"bold"}, &render::GridStyle::bold},
                    std::pair{std::string_view{"faint"}, &render::GridStyle::faint},
                    std::pair{std::string_view{"italic"}, &render::GridStyle::italic},
                    std::pair{std::string_view{"underline"}, &render::GridStyle::underline},
                    std::pair{std::string_view{"inverse"}, &render::GridStyle::inverse}}) {
      if (api::json_member(value, field) == nullptr) {
        continue;
      }
      const auto enabled = api::json_boolean(value, field);
      if (!enabled.has_value()) {
        return std::nullopt;
      }
      style.*destination = *enabled;
    }
    styles.push_back(style);
  }
  return std::optional{std::move(styles)};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto decode_grid_patch(const api::JsonValue& document, SurfaceId& id)
    -> std::optional<render::GridPatch> {
  if (!known_fields(document, {"schema", "surface", "rows", "styles", "cursor"}) ||
      api::json_string(document, "schema") !=
          std::optional<std::string_view>{surface_update_schema}) {
    return std::nullopt;
  }
  const auto surface = api::json_string(document, "surface");
  const auto parsed = surface.has_value() ? parse_id<SurfaceId>(*surface) : std::nullopt;
  const auto* const rows = api::json_member(document, "rows");
  if (!parsed.has_value() ||
      (rows == nullptr && api::json_member(document, "styles") == nullptr &&
       api::json_member(document, "cursor") == nullptr) ||
      (rows != nullptr && (rows->kind != api::JsonKind::array ||
                           rows->array.size() > limits::terminal_rows_hard_max))) {
    return std::nullopt;
  }
  id = *parsed;
  render::GridPatch patch;
  if (rows != nullptr) {
    patch.rows.reserve(rows->array.size());
    for (const auto& encoded_row : rows->array) {
      if (!known_fields(encoded_row, {"row", "runs"})) {
        return std::nullopt;
      }
      const auto row = api::json_unsigned(encoded_row, "row");
      const auto* const runs = api::json_member(encoded_row, "runs");
      if (!row.has_value() || *row > std::numeric_limits<std::uint16_t>::max() || runs == nullptr ||
          runs->kind != api::JsonKind::array ||
          runs->array.size() > limits::surface_runs_per_row_max) {
        return std::nullopt;
      }
      render::GridRowPatch row_patch{.runs = {}, .row = static_cast<std::uint16_t>(*row)};
      row_patch.runs.reserve(runs->array.size());
      for (const auto& encoded_run : runs->array) {
        if (!known_fields(encoded_run, {"column", "text", "style"})) {
          return std::nullopt;
        }
        const auto column = api::json_unsigned(encoded_run, "column");
        const auto text = api::json_string(encoded_run, "text");
        const auto style = api::json_member(encoded_run, "style") == nullptr
                               ? std::optional<std::uint64_t>{0}
                               : api::json_unsigned(encoded_run, "style");
        if (!style.has_value() || !column.has_value() ||
            *column > std::numeric_limits<std::uint16_t>::max() || !text.has_value() ||
            text->empty() || text->size() > limits::surface_text_bytes_per_row_max ||
            *style > std::numeric_limits<std::uint16_t>::max()) {
          return std::nullopt;
        }
        row_patch.runs.push_back({.text = std::string(*text),
                                  .column = static_cast<std::uint16_t>(*column),
                                  .style = static_cast<std::uint16_t>(*style)});
      }
      patch.rows.push_back(std::move(row_patch));
    }
  }
  auto styles = decode_styles(document);
  if (!styles.has_value()) {
    return std::nullopt;
  }
  patch.styles = std::move(*styles);
  if (const auto* const cursor = api::json_member(document, "cursor"); cursor != nullptr) {
    if (!known_fields(*cursor, {"column", "row", "visible"})) {
      return std::nullopt;
    }
    const auto column = api::json_unsigned(*cursor, "column");
    const auto row = api::json_unsigned(*cursor, "row");
    const auto visible = api::json_member(*cursor, "visible") == nullptr
                             ? std::optional<bool>{false}
                             : api::json_boolean(*cursor, "visible");
    if (!visible.has_value() || !column.has_value() || !row.has_value() ||
        *column > std::numeric_limits<std::uint16_t>::max() ||
        *row > std::numeric_limits<std::uint16_t>::max()) {
      return std::nullopt;
    }
    patch.cursor = render::GridCursor{.column = static_cast<std::uint16_t>(*column),
                                      .row = static_cast<std::uint16_t>(*row),
                                      .visible = *visible};
  }
  return patch;
}

[[nodiscard]] constexpr auto dock_placement(const api::SurfacePlacementKind kind) noexcept -> bool {
  return kind == api::SurfacePlacementKind::dock_left ||
         kind == api::SurfacePlacementKind::dock_right ||
         kind == api::SurfacePlacementKind::dock_top ||
         kind == api::SurfacePlacementKind::dock_bottom;
}

[[nodiscard]] constexpr auto surface_operation(const SurfaceOperationStatus status) noexcept
    -> SurfaceOperationResult {
  return {.status = status, .surface = {}, .pane_viewport = {}};
}

[[nodiscard]] constexpr auto surface_update_result(const SurfaceOperationStatus status,
                                                   const SurfaceId id = {}) noexcept
    -> SurfaceUpdateResult {
  return {.status = status, .surface = id, .changed_rows = 0};
}

[[nodiscard]] constexpr auto capability_bit(const std::string_view capability) noexcept
    -> std::uint8_t {
  if (capability == "observe") {
    return capability_observe;
  }
  if (capability == "proc") {
    return capability_proc;
  }
  return capability == "surface" ? capability_surface : std::uint8_t{0};
}

[[nodiscard]] auto valid_native_scope(const Hello& hello, const SessionId session,
                                      const AttachmentId attachment) noexcept -> bool {
  if (attachment.is_valid() && attachment.slot() >= limits::sessions_hard_max) {
    return false;
  }
  return ((hello.capabilities & capability_surface) == 0 &&
          !hello.subscription.session.has_value()) ||
         (session.is_valid() && attachment.is_valid());
}

[[nodiscard]] auto placement_size(const api::SurfacePlacement placement) noexcept -> std::uint16_t {
  return placement.kind == api::SurfacePlacementKind::dock_left ||
                 placement.kind == api::SurfacePlacementKind::dock_right
             ? placement.columns
             : placement.rows;
}

} // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto decode_hello(const api::JsonValue& document) -> std::optional<Hello> {
  if (!known_fields(document, {"schema", "name", "capabilities", "events"}) ||
      api::json_string(document, "schema") != std::optional{protocol_schema}) {
    return std::nullopt;
  }
  const auto name = api::json_string(document, "name");
  const auto* const capabilities = api::json_member(document, "capabilities");
  if (!name.has_value() || name->empty() || name->size() > 64U ||
      !std::ranges::all_of(
          *name,
          [](const unsigned char character) { return character >= 0x20U && character < 0x7fU; }) ||
      capabilities == nullptr || capabilities->kind != api::JsonKind::array ||
      capabilities->array.empty() || capabilities->array.size() > 3U) {
    return std::nullopt;
  }
  Hello result{.name = std::string(*name), .subscription = {}, .capabilities = 0};
  for (const auto& capability : capabilities->array) {
    if (capability.kind != api::JsonKind::string) {
      return std::nullopt;
    }
    const auto bit = capability_bit(capability.string);
    if (bit == 0 || (result.capabilities & bit) != 0) {
      return std::nullopt;
    }
    result.capabilities |= bit;
  }
  const auto* const events = api::json_member(document, "events");
  if (events != nullptr) {
    const auto decoded = api::decode_event_subscription(*events);
    if (!decoded.subscription.has_value()) {
      return std::nullopt;
    }
    result.subscription = *decoded.subscription;
  } else if ((result.capabilities & capability_observe) != 0) {
    return std::nullopt;
  }
  if ((result.capabilities & capability_surface) != 0 && !result.subscription.session.has_value()) {
    return std::nullopt;
  }
  return result;
}

auto append_input_payload(std::string& event, const std::span<const std::byte> bytes,
                          const bool opaque) -> bool {
  // JSON control-byte escaping is the worst expansion (6x); leave bounded room for metadata.
  constexpr std::size_t metadata_bytes_max = 1024;
  static_assert((limits::extension_input_bytes_max * 6U) + metadata_bytes_max <
                limits::extension_record_bytes_max);
  if (bytes.size() > limits::extension_input_bytes_max || event.size() > metadata_bytes_max) {
    return false;
  }
  // Byte and character storage have the same object representation.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!opaque && api::valid_utf8(text)) {
    event += R"(,"text":)";
    return api::append_json_string(event, text, limits::extension_record_bytes_max - 1U);
  }
  constexpr std::string_view digits = "0123456789abcdef";
  event.reserve(event.size() + 16U + (bytes.size() * 2U));
  event += R"(,"bytes_hex":")";
  for (const auto byte : bytes) {
    const auto value = std::to_integer<unsigned char>(byte);
    event += digits.at(value >> 4U);
    event += digits.at(value & 0x0fU);
  }
  event += '"';
  return true;
}

Runtime::Peer* Runtime::peer(const ExtensionGenerationId owner) noexcept {
  if (!owner.is_valid() || owner.slot() >= peers_.size()) {
    return nullptr;
  }
  auto& slot = peers_.at(owner.slot());
  return slot.generation == owner.generation() && slot.peer.has_value() ? &*slot.peer : nullptr;
}

const Runtime::Peer* Runtime::peer(const ExtensionGenerationId owner) const noexcept {
  if (!owner.is_valid() || owner.slot() >= peers_.size()) {
    return nullptr;
  }
  const auto& slot = peers_.at(owner.slot());
  return slot.generation == owner.generation() && slot.peer.has_value() ? &*slot.peer : nullptr;
}

Runtime::Surface* Runtime::surface(const SurfaceId id) noexcept {
  if (!id.is_valid() || id.slot() >= surfaces_.size()) {
    return nullptr;
  }
  auto& slot = surfaces_.at(id.slot());
  return slot.generation == id.generation() && slot.surface.has_value() ? &*slot.surface : nullptr;
}

const Runtime::Surface* Runtime::surface(const SurfaceId id) const noexcept {
  if (!id.is_valid() || id.slot() >= surfaces_.size()) {
    return nullptr;
  }
  const auto& slot = surfaces_.at(id.slot());
  return slot.generation == id.generation() && slot.surface.has_value() ? &*slot.surface : nullptr;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Runtime::admit(FramedPeer transport, Hello hello, const SessionId session_id,
                    const AttachmentId attachment_id, std::string_view snapshot) noexcept
    -> std::optional<ExtensionGenerationId> {
  if (!valid_native_scope(hello, session_id, attachment_id)) {
    return std::nullopt;
  }
  auto* const slot =
      std::ranges::find_if(peers_, [](const PeerSlot& value) { return !value.peer.has_value(); });
  if (slot == peers_.end() || !transport.connected()) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(std::distance(peers_.begin(), slot));
  slot->generation = next_generation(slot->generation);
  const auto owner =
      ExtensionGenerationId::from_parts(static_cast<std::uint32_t>(index), slot->generation);
  try {
    slot->peer.emplace(Peer{.transport = std::move(transport),
                            .hello = std::move(hello),
                            .owner = owner,
                            .session = session_id,
                            .attachment = attachment_id});
    auto welcome =
        std::string{R"({"schema":"frame.extension-welcome/v1","owner":")"} + id_text(owner) + '"';
    if (attachment_id.is_valid()) {
      welcome += R"(,"attachment":")" + id_text(attachment_id) + '"';
    }
    welcome += R"(,"capabilities":[)";
    bool separator = false;
    for (const auto [bit, name] :
         std::array{std::pair{capability_observe, std::string_view{"observe"}},
                    std::pair{capability_proc, std::string_view{"proc"}},
                    std::pair{capability_surface, std::string_view{"surface"}}}) {
      if ((slot->peer->hello.capabilities & bit) == 0) {
        continue;
      }
      welcome += separator ? ",\"" : "\"";
      welcome += name;
      welcome += '"';
      separator = true;
    }
    welcome += R"(],"limits":{"peers":)" + std::to_string(limits::extension_sessions_hard_max) +
               R"(,"surfaces":)" + std::to_string(limits::extension_surfaces_per_owner_max) +
               R"(,"surfaces_aggregate":)" + std::to_string(limits::extension_surfaces_hard_max) +
               R"(,"procs":)" + std::to_string(limits::extension_procs_per_owner_max) +
               R"(,"record_bytes":)" + std::to_string(limits::extension_record_bytes_max) +
               R"(,"input_bytes_aggregate":)" +
               std::to_string(limits::extension_input_bytes_aggregate_max) + R"(,"output_bytes":)" +
               std::to_string(limits::extension_output_bytes_per_owner_max) +
               R"(,"output_bytes_aggregate":)" +
               std::to_string(limits::extension_output_bytes_aggregate_max) +
               R"(,"read_bytes_per_turn":)" + std::to_string(limits::extension_bytes_per_turn_max) +
               R"(,"record_work_bytes_per_turn":)" +
               std::to_string(limits::extension_record_work_bytes_per_turn_max) +
               R"(,"records_per_turn":)" + std::to_string(limits::extension_records_per_turn_max) +
               R"(,"write_bytes_per_turn":)" +
               std::to_string(limits::extension_output_bytes_per_turn_max) +
               R"(,"surface_bytes":)" + std::to_string(limits::surface_retained_bytes_max) +
               R"(,"surface_bytes_aggregate":)" +
               std::to_string(limits::surface_retained_bytes_aggregate_max) + R"(,"styles":)" +
               std::to_string(limits::surface_styles_max) + R"(,"runs_per_row":)" +
               std::to_string(limits::surface_runs_per_row_max) + R"(,"text_bytes_per_row":)" +
               std::to_string(limits::surface_text_bytes_per_row_max) + R"(,"input_bytes":)" +
               std::to_string(limits::extension_input_bytes_max) + R"(,"columns":)" +
               std::to_string(limits::terminal_columns_hard_max) + R"(,"rows":)" +
               std::to_string(limits::terminal_rows_hard_max) + "}}";
    if (!slot->peer->transport.send_json(RecordKind::welcome, 1, welcome)) {
      slot->peer.reset();
      return std::nullopt;
    }
    while (!snapshot.empty() && (snapshot.back() == '\n' || snapshot.back() == '\r')) {
      snapshot.remove_suffix(1);
    }
    if ((slot->peer->hello.capabilities & capability_observe) != 0 &&
        !slot->peer->transport.send_json(RecordKind::event, 1, snapshot)) {
      slot->peer.reset();
      return std::nullopt;
    }
    ++peer_count_;
    return owner;
  } catch (const std::bad_alloc&) {
    slot->peer.reset();
    return std::nullopt;
  }
}

auto Runtime::peer_views(std::array<PeerView, limits::extension_sessions_hard_max>& storage)
    const noexcept -> std::span<const PeerView> {
  if (peer_count_ == 0) {
    return {};
  }
  std::size_t count = 0;
  for (std::size_t slot = 0; slot < peers_.size(); ++slot) {
    const auto& peer_slot = peers_.at(slot);
    if (!peer_slot.peer.has_value() || !peer_slot.peer->transport.connected()) {
      continue;
    }
    storage.at(count++) = {.owner = peer_slot.peer->owner,
                           .descriptor = peer_slot.peer->transport.descriptor(),
                           .events = peer_slot.peer->transport.events(),
                           .slot = slot};
  }
  return std::span(storage).first(count);
}

auto Runtime::read_ready(const std::size_t slot, const std::size_t bytes_max) noexcept
    -> std::size_t {
  return slot < peers_.size() && peers_.at(slot).peer.has_value()
             ? peers_.at(slot).peer->transport.read_ready(bytes_max)
             : 0;
}

auto Runtime::buffered_record_bytes(const std::size_t slot) const noexcept
    -> std::optional<std::size_t> {
  return slot < peers_.size() && peers_.at(slot).peer.has_value()
             ? peers_.at(slot).peer->transport.buffered_record_bytes()
             : std::nullopt;
}

auto Runtime::buffered_work() const noexcept -> bool {
  return peer_count_ > 0 && std::ranges::any_of(peers_, [](const PeerSlot& slot) {
           return slot.peer.has_value() && slot.peer->transport.buffered_record();
         });
}

auto Runtime::write_ready(const std::size_t slot, const std::size_t bytes_max) noexcept
    -> std::size_t {
  if (slot >= peers_.size() || !peers_.at(slot).peer.has_value()) {
    return 0;
  }
  auto& found = *peers_.at(slot).peer;
  const auto written = found.transport.write_ready(bytes_max);
  flush_surface_events(found);
  return written;
}

auto Runtime::receive(const std::size_t slot) noexcept -> std::optional<Record> {
  return slot < peers_.size() && peers_.at(slot).peer.has_value()
             ? peers_.at(slot).peer->transport.receive()
             : std::nullopt;
}

void Runtime::consume(const std::size_t slot) noexcept {
  if (slot < peers_.size() && peers_.at(slot).peer.has_value()) {
    peers_.at(slot).peer->transport.consume();
  }
}

auto Runtime::owner_at(const std::size_t slot) const noexcept -> ExtensionGenerationId {
  return slot < peers_.size() && peers_.at(slot).peer.has_value() ? peers_.at(slot).peer->owner
                                                                  : ExtensionGenerationId{};
}

auto Runtime::connected(const ExtensionGenerationId owner) const noexcept -> bool {
  const auto* const found = peer(owner);
  return found != nullptr && found->transport.connected();
}

auto Runtime::output_bytes(const ExtensionGenerationId owner) const noexcept -> std::size_t {
  const auto* const found = peer(owner);
  return found == nullptr ? 0 : found->transport.output_bytes();
}

auto Runtime::output_accounting(const ExtensionGenerationId owner) const noexcept
    -> OutputAccounting {
  const auto* const found = peer(owner);
  return found == nullptr ? OutputAccounting{} : found->transport.output_accounting();
}

auto Runtime::event_sequence(const ExtensionGenerationId owner) const noexcept -> std::uint32_t {
  const auto* const found = peer(owner);
  return found == nullptr ? 0 : found->next_event_sequence;
}

auto Runtime::has_capability(const ExtensionGenerationId owner,
                             const std::uint8_t capability) const noexcept -> bool {
  const auto* const found = peer(owner);
  return found != nullptr && found->transport.connected() &&
         (found->hello.capabilities & capability) == capability;
}

auto Runtime::subscription(const ExtensionGenerationId owner) const noexcept
    -> const api::EventSubscription* {
  const auto* const found = peer(owner);
  return found == nullptr ? nullptr : &found->hello.subscription;
}

auto Runtime::session(const ExtensionGenerationId owner) const noexcept -> SessionId {
  const auto* const found = peer(owner);
  return found == nullptr ? SessionId{} : found->session;
}

auto Runtime::attachment(const ExtensionGenerationId owner) const noexcept -> AttachmentId {
  const auto* const found = peer(owner);
  return found == nullptr ? AttachmentId{} : found->attachment;
}

auto Runtime::accounting() const noexcept -> RuntimeAccounting {
  RuntimeAccounting result{};
  result.peers = peer_count_;
  result.surfaces = surface_count_;
  result.retained_surface_bytes = retained_surface_bytes_;
  for (const auto& slot : peers_) {
    if (!slot.peer.has_value()) {
      continue;
    }
    const auto& transport = slot.peer->transport;
    const auto output = transport.output_accounting();
    result.input_bytes += transport.input_bytes();
    result.output_bytes += transport.output_bytes();
    result.buffered_records += static_cast<std::size_t>(transport.buffered_record());
    result.output.reserved_bytes += output.reserved_bytes;
    result.output.reserved_records += output.reserved_records;
    result.output.event_bytes += output.event_bytes;
    result.output.event_records += output.event_records;
  }
  FRAME_ASSERT(result.peers <= limits::extension_sessions_hard_max);
  FRAME_ASSERT(result.input_bytes <= limits::extension_input_bytes_aggregate_max);
  FRAME_ASSERT(result.output_bytes + result.output.reserved_bytes <=
               limits::extension_output_bytes_aggregate_max);
  FRAME_ASSERT(result.buffered_records <= result.peers);
  FRAME_ASSERT(result.surfaces <= limits::extension_surfaces_hard_max);
  FRAME_ASSERT(result.retained_surface_bytes <= limits::surface_retained_bytes_aggregate_max);
  return result;
}

auto Runtime::reserve_proc(const ExtensionGenerationId owner,
                           const std::uint32_t request_id) noexcept -> bool {
  auto* const found = peer(owner);
  constexpr auto result_reserve = api::json_bytes_max + protocol_header_bytes;
  if (found == nullptr || !has_capability(owner, capability_proc) || request_id == 0 ||
      std::ranges::find(found->procs, request_id) != found->procs.end()) {
    return false;
  }
  auto* const slot = std::ranges::find(found->procs, 0U);
  if (slot == found->procs.end() || !found->transport.reserve_output(result_reserve)) {
    return false;
  }
  *slot = request_id;
  return true;
}

void Runtime::complete_proc(const ExtensionGenerationId owner, const std::uint32_t request_id,
                            const std::string_view result) noexcept {
  auto* const found = peer(owner);
  if (found == nullptr || !found->transport.connected() || request_id == 0) {
    return;
  }
  auto* const slot = std::ranges::find(found->procs, request_id);
  if (slot == found->procs.end()) {
    return;
  }
  if (result.size() > api::json_bytes_max) {
    found->transport.disconnect();
    return;
  }
  // Single reactor owner: converting reserved storage to queued bytes admits no intervening output.
  found->transport.release_output(api::json_bytes_max + protocol_header_bytes);
  *slot = 0;
  if (!found->transport.send_json(RecordKind::proc_result, request_id, result)) {
    found->transport.disconnect();
    return;
  }
  flush_surface_events(*found);
  for (auto& peer_slot : peers_) {
    if (peer_slot.peer.has_value() && peer_slot.peer->owner != owner) {
      flush_surface_events(*peer_slot.peer);
    }
  }
}

auto Runtime::begin_surface_transaction(const ExtensionGenerationId owner) noexcept -> bool {
  auto* const found = peer(owner);
  const auto attachment_id = attachment(owner);
  if (found == nullptr || !attachment_id.is_valid() || surface_transaction_.has_value()) {
    return false;
  }
  surface_transaction_.emplace();
  auto& transaction = *surface_transaction_;
  transaction.owner = owner;
  transaction.attachment = attachment_id;
  for (std::size_t slot = 0; slot < surfaces_.size(); ++slot) {
    transaction.slot_generations.at(slot) = surfaces_.at(slot).generation;
  }
  for (std::size_t slot = 0; slot < peers_.size(); ++slot) {
    if (peers_.at(slot).peer.has_value()) {
      const auto& current = *peers_.at(slot).peer;
      transaction.peer_owners.at(slot) = current.owner;
      std::ranges::copy(current.surface_events, transaction.surface_events.at(slot).begin());
      transaction.surface_event_counts.at(slot) = current.surface_event_count;
    }
  }
  transaction.focused = focused_surfaces_.at(attachment_id.slot());
  transaction.captured = captured_surfaces_.at(attachment_id.slot());
  transaction.geometry_generation = geometry_generations_.at(attachment_id.slot());
  transaction.retained_surface_bytes = retained_surface_bytes_;
  transaction.surface_count = surface_count_;
  return true;
}

void Runtime::commit_surface_transaction(const ExtensionGenerationId owner) noexcept {
  if (surface_transaction_.has_value() && surface_transaction_->owner == owner) {
    surface_transaction_.reset();
  }
}

auto Runtime::rollback_surface_transaction(const ExtensionGenerationId owner) noexcept -> bool {
  if (!surface_transaction_.has_value() || surface_transaction_->owner != owner) {
    return false;
  }
  auto& transaction = *surface_transaction_;
  for (std::size_t slot = 0; slot < surfaces_.size(); ++slot) {
    if (transaction.touched.at(slot)) {
      surfaces_.at(slot).surface.reset();
      surfaces_.at(slot).surface = std::move(transaction.previous.at(slot));
    }
    surfaces_.at(slot).generation = transaction.slot_generations.at(slot);
  }
  retained_surface_bytes_ = transaction.retained_surface_bytes;
  surface_count_ = transaction.surface_count;
  focused_surfaces_.at(transaction.attachment.slot()) = transaction.focused;
  captured_surfaces_.at(transaction.attachment.slot()) = transaction.captured;
  geometry_generations_.at(transaction.attachment.slot()) = transaction.geometry_generation;
  for (std::size_t slot = 0; slot < peers_.size(); ++slot) {
    if (peers_.at(slot).peer.has_value() &&
        peers_.at(slot).peer->owner == transaction.peer_owners.at(slot)) {
      auto& current = *peers_.at(slot).peer;
      std::ranges::copy(transaction.surface_events.at(slot), current.surface_events.begin());
      current.surface_event_count = transaction.surface_event_counts.at(slot);
    }
  }
  surface_transaction_.reset();
  return true;
}

void Runtime::replace_surface(const std::size_t slot, std::optional<Surface> replacement) noexcept {
  auto& destination = surfaces_.at(slot);
  if (surface_transaction_.has_value() && !surface_transaction_->touched.at(slot)) {
    surface_transaction_->previous.at(slot) = std::move(destination.surface);
    surface_transaction_->touched.at(slot) = true;
  }
  destination.surface = std::move(replacement);
}

auto Runtime::send_event(const ExtensionGenerationId owner, const std::string_view event) noexcept
    -> bool {
  auto* const found = peer(owner);
  if (found == nullptr) {
    return false;
  }
  const auto record_bytes = protocol_header_bytes + event.size();
  const auto accounting = found->transport.output_accounting();
  if (accounting.event_records >= limits::extension_interaction_events_max ||
      record_bytes > limits::extension_interaction_bytes_per_owner_max - accounting.event_bytes) {
    found->transport.disconnect();
    return false;
  }
  // Queue pressure cannot revoke a Proc's reserved result. The caller retains/rejects this Event;
  // transport failure and the Event lane's own overflow policy remain distinct.
  if (!found->transport.send_json(RecordKind::event, found->next_event_sequence, event)) {
    return false;
  }
  found->next_event_sequence =
      found->next_event_sequence == std::numeric_limits<std::uint32_t>::max()
          ? 1U
          : found->next_event_sequence + 1U;
  return true;
}

auto Runtime::send_error(const ExtensionGenerationId owner, const std::uint32_t sequence,
                         const std::string_view reason) noexcept -> bool {
  auto* const found = peer(owner);
  if (found == nullptr) {
    return false;
  }
  try {
    std::string payload = R"({"schema":"frame.extension-error/v1","reason":)";
    if (!api::append_json_string(payload, reason)) {
      return false;
    }
    payload += '}';
    return found->transport.send_json(RecordKind::error, sequence == 0 ? 1U : sequence, payload);
  } catch (const std::bad_alloc&) {
    found->transport.disconnect();
    return false;
  }
}

void Runtime::enqueue_surface_event(const ExtensionGenerationId owner, const SurfaceId id,
                                    const PendingSurfaceEventKind kind, const std::uint16_t columns,
                                    const std::uint16_t rows) noexcept {
  auto* const found = peer(owner);
  if (found == nullptr) {
    return;
  }
  if (kind == PendingSurfaceEventKind::resized) {
    const auto existing = std::ranges::find_if(
        std::span(found->surface_events).first(found->surface_event_count),
        [id](const PendingSurfaceEvent& event) {
          return event.surface == id && event.kind == PendingSurfaceEventKind::resized;
        });
    if (existing != std::span(found->surface_events).first(found->surface_event_count).end()) {
      existing->columns = columns;
      existing->rows = rows;
      return;
    }
  }
  if (found->surface_event_count >= found->surface_events.size()) {
    found->transport.disconnect();
    return;
  }
  found->surface_events.at(found->surface_event_count++) = {
      .surface = id, .columns = columns, .rows = rows, .kind = kind};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Runtime::flush_surface_events(Peer& found) noexcept {
  if (surface_transaction_.has_value() ||
      std::ranges::any_of(found.procs, [](const std::uint32_t id) { return id != 0; })) {
    return;
  }
  std::size_t consumed = 0;
  try {
    for (; consumed < found.surface_event_count; ++consumed) {
      const auto& pending = found.surface_events.at(consumed);
      const char* name = nullptr;
      switch (pending.kind) {
      case PendingSurfaceEventKind::resized:
        name = "surface.resized";
        break;
      case PendingSurfaceEventKind::focused:
        name = "surface.focused";
        break;
      case PendingSurfaceEventKind::blurred:
        name = "surface.blurred";
        break;
      case PendingSurfaceEventKind::closed:
        name = "surface.closed";
        break;
      }
      std::string event = R"({"schema":"frame.event/v1","sequence":)" +
                          std::to_string(found.next_event_sequence) + R"(,"event":")" + name +
                          R"(","surface":")" + id_text(pending.surface) + '"';
      if (pending.kind == PendingSurfaceEventKind::resized) {
        event += R"(,"columns":)" + std::to_string(pending.columns) + R"(,"rows":)" +
                 std::to_string(pending.rows);
      }
      event += '}';
      if (!send_event(found.owner, event)) {
        break;
      }
    }
  } catch (const std::bad_alloc&) {
    found.transport.disconnect();
  }
  if (consumed > 0) {
    std::ranges::move(
        std::span(found.surface_events).subspan(consumed, found.surface_event_count - consumed),
        found.surface_events.begin());
    found.surface_event_count -= consumed;
  }
}

auto Runtime::owned_surface_count(const ExtensionGenerationId owner) const noexcept -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(surfaces_, [&](const SurfaceSlot& slot) {
    return slot.surface.has_value() && slot.surface->owner == owner;
  }));
}

auto Runtime::pane_viewport(const AttachmentId attachment_id,
                            const render::Viewport viewport) const noexcept
    -> std::optional<PaneRectangle> {
  if (surface_count_ == 0) {
    return PaneRectangle{.columns = viewport.columns, .rows = viewport.rows};
  }
  if (viewport.columns == 0 || viewport.rows == 0) {
    return std::nullopt;
  }
  return resolve_layout(attachment_id, viewport).pane;
}

// One projection for geometry, composition, and hit testing. Docks that cannot leave at least one
// Pane cell are suspended in stable slot order; floats outside the viewport are suspended whole.
// Suspension changes neither the declared placement nor the retained Grid.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Runtime::resolve_layout(const AttachmentId attachment_id,
                             const render::Viewport viewport) const noexcept -> SurfaceLayout {
  SurfaceLayout layout{.pane = {.columns = viewport.columns, .rows = viewport.rows}};
  std::uint16_t left = 0;
  std::uint16_t right = 0;
  std::uint16_t top = 0;
  std::uint16_t bottom = 0;
  for (const auto& slot : surfaces_) {
    if (!slot.surface.has_value() || slot.surface->attachment != attachment_id) {
      continue;
    }
    const auto& placement = slot.surface->placement;
    auto& rectangle = layout.rectangles.at(slot.surface->id.slot());
    const auto size = placement_size(placement);
    if (dock_placement(placement.kind)) {
      const bool horizontal = placement.kind == api::SurfacePlacementKind::dock_left ||
                              placement.kind == api::SurfacePlacementKind::dock_right;
      const auto available = horizontal ? layout.pane.columns : layout.pane.rows;
      if (size == 0 || size >= available) {
        continue;
      }
      switch (placement.kind) {
      case api::SurfacePlacementKind::dock_left:
        rectangle = PaneRectangle{.column = left, .columns = size};
        left += size;
        break;
      case api::SurfacePlacementKind::dock_right:
        rectangle = PaneRectangle{.column = right, .columns = size};
        right += size;
        break;
      case api::SurfacePlacementKind::dock_top:
        rectangle = PaneRectangle{.row = top, .columns = viewport.columns, .rows = size};
        top += size;
        break;
      case api::SurfacePlacementKind::dock_bottom:
        rectangle = PaneRectangle{.row = bottom, .columns = viewport.columns, .rows = size};
        bottom += size;
        break;
      case api::SurfacePlacementKind::float_surface:
      case api::SurfacePlacementKind::overlay:
        break;
      }
      layout.pane = {.column = left,
                     .row = top,
                     .columns = static_cast<std::uint16_t>(viewport.columns - left - right),
                     .rows = static_cast<std::uint16_t>(viewport.rows - top - bottom)};
    } else if (placement.columns != 0 && placement.rows != 0 &&
               static_cast<std::uint32_t>(placement.column) + placement.columns <=
                   viewport.columns &&
               static_cast<std::uint32_t>(placement.row) + placement.rows <= viewport.rows) {
      rectangle = PaneRectangle{.column = placement.column,
                                .row = placement.row,
                                .columns = placement.columns,
                                .rows = placement.rows};
    }
  }
  for (const auto& slot : surfaces_) {
    if (!slot.surface.has_value() || slot.surface->attachment != attachment_id) {
      continue;
    }
    auto& rectangle = layout.rectangles.at(slot.surface->id.slot());
    if (!rectangle.has_value()) {
      continue;
    }
    switch (slot.surface->placement.kind) {
    case api::SurfacePlacementKind::dock_right:
      rectangle->column += layout.pane.column + layout.pane.columns;
      [[fallthrough]];
    case api::SurfacePlacementKind::dock_left:
      rectangle->row = top;
      rectangle->rows = layout.pane.rows;
      break;
    case api::SurfacePlacementKind::dock_bottom:
      rectangle->row += layout.pane.row + layout.pane.rows;
      break;
    case api::SurfacePlacementKind::dock_top:
    case api::SurfacePlacementKind::float_surface:
    case api::SurfacePlacementKind::overlay:
      break;
    }
  }
  return layout;
}

auto Runtime::resolved_rectangle(const Surface& target,
                                 const render::Viewport viewport) const noexcept
    -> std::optional<PaneRectangle> {
  return resolve_layout(target.attachment, viewport).rectangles.at(target.id.slot());
}

auto Runtime::create_surface(const ExtensionGenerationId owner,
                             const api::SurfacePlacement placement, const bool focusable,
                             const bool opaque, const render::Viewport viewport) noexcept
    -> SurfaceOperationResult {
  if (!has_capability(owner, capability_surface)) {
    return surface_operation(SurfaceOperationStatus::unavailable);
  }
  if (!opaque && dock_placement(placement.kind)) {
    return surface_operation(SurfaceOperationStatus::invalid);
  }
  if (owned_surface_count(owner) >= limits::extension_surfaces_per_owner_max) {
    return surface_operation(SurfaceOperationStatus::capacity);
  }
  auto* const destination = std::ranges::find_if(
      surfaces_, [](const SurfaceSlot& slot) { return !slot.surface.has_value(); });
  if (destination == surfaces_.end()) {
    return surface_operation(SurfaceOperationStatus::capacity);
  }
  const auto slot = static_cast<std::size_t>(std::distance(surfaces_.begin(), destination));
  destination->generation = next_generation(destination->generation);
  const auto id = SurfaceId::from_parts(static_cast<std::uint32_t>(slot), destination->generation);
  const auto attachment_id = attachment(owner);
  auto placeholder = render::Grid::create(1, 1);
  if (!placeholder.has_value()) {
    return surface_operation(SurfaceOperationStatus::capacity);
  }
  try {
    replace_surface(slot, Surface{.id = id,
                                  .owner = owner,
                                  .attachment = attachment_id,
                                  .placement = placement,
                                  .grid = std::move(*placeholder),
                                  .focusable = focusable,
                                  .opaque = opaque});
  } catch (const std::bad_alloc&) {
    replace_surface(slot, std::nullopt);
    return surface_operation(SurfaceOperationStatus::capacity);
  }
  const auto rectangle = resolved_rectangle(*destination->surface, viewport);
  if (!rectangle.has_value()) {
    destination->surface.reset();
    return surface_operation(SurfaceOperationStatus::invalid);
  }
  auto grid = render::Grid::create(rectangle->columns, rectangle->rows);
  if (!grid.has_value() || retained_surface_bytes_ + grid->retained_bytes() >
                               limits::surface_retained_bytes_aggregate_max) {
    destination->surface.reset();
    return surface_operation(SurfaceOperationStatus::capacity);
  }
  destination->surface->grid = std::move(*grid);
  retained_surface_bytes_ += destination->surface->grid.retained_bytes();
  ++surface_count_;
  auto& geometry = geometry_generations_.at(attachment_id.slot());
  geometry = geometry == std::numeric_limits<std::uint64_t>::max() ? 1U : geometry + 1U;
  enqueue_surface_event(owner, id, PendingSurfaceEventKind::resized, rectangle->columns,
                        rectangle->rows);
  return {.status = SurfaceOperationStatus::applied,
          .surface = id,
          .pane_viewport = pane_viewport(attachment_id, viewport).value_or(PaneRectangle{})};
}

auto Runtime::configure_surface(const ExtensionGenerationId owner, const SurfaceId id,
                                const api::SurfacePlacement placement,
                                const render::Viewport viewport,
                                const std::optional<bool> configured_focusable) noexcept
    -> SurfaceOperationResult {
  auto* const found = surface(id);
  if (found == nullptr) {
    return surface_operation(SurfaceOperationStatus::stale);
  }
  if (found->owner != owner) {
    return surface_operation(SurfaceOperationStatus::wrong_owner);
  }
  if (!found->opaque && dock_placement(placement.kind)) {
    return surface_operation(SurfaceOperationStatus::invalid);
  }
  if (found->placement.kind == placement.kind && found->placement.column == placement.column &&
      found->placement.row == placement.row && found->placement.columns == placement.columns &&
      found->placement.rows == placement.rows &&
      configured_focusable.value_or(found->focusable) == found->focusable) {
    return {.status = SurfaceOperationStatus::no_effect,
            .surface = id,
            .pane_viewport = pane_viewport(found->attachment, viewport).value_or(PaneRectangle{})};
  }
  const auto previous = found->placement;
  found->placement = placement;
  const auto rectangle = resolved_rectangle(*found, viewport);
  if (!rectangle.has_value()) {
    found->placement = previous;
    return surface_operation(SurfaceOperationStatus::invalid);
  }
  auto grid = found->grid.resized(rectangle->columns, rectangle->rows);
  if (!grid.has_value() ||
      grid->retained_bytes() > limits::surface_retained_bytes_aggregate_max -
                                   (retained_surface_bytes_ - found->grid.retained_bytes())) {
    found->placement = previous;
    return surface_operation(SurfaceOperationStatus::capacity);
  }
  const auto attachment_id = found->attachment;
  const auto focusable = configured_focusable.value_or(found->focusable);
  const auto opaque = found->opaque;
  const auto previous_bytes = found->grid.retained_bytes();
  found->placement = previous;
  replace_surface(id.slot(), Surface{.id = id,
                                     .owner = owner,
                                     .attachment = attachment_id,
                                     .placement = placement,
                                     .grid = std::move(*grid),
                                     .focusable = focusable,
                                     .opaque = opaque});
  retained_surface_bytes_ = retained_surface_bytes_ - previous_bytes +
                            surfaces_.at(id.slot()).surface->grid.retained_bytes();
  if (!focusable && focused_surface(attachment_id) == id) {
    static_cast<void>(focus_pane(attachment_id));
  }
  auto& geometry = geometry_generations_.at(attachment_id.slot());
  geometry = geometry == std::numeric_limits<std::uint64_t>::max() ? 1U : geometry + 1U;
  enqueue_surface_event(owner, id, PendingSurfaceEventKind::resized, rectangle->columns,
                        rectangle->rows);
  return {.status = SurfaceOperationStatus::applied,
          .surface = id,
          .pane_viewport = pane_viewport(found->attachment, viewport).value_or(PaneRectangle{})};
}

auto Runtime::close_surface(const ExtensionGenerationId owner, const SurfaceId id,
                            const render::Viewport viewport) noexcept -> SurfaceOperationResult {
  auto* const found = surface(id);
  if (found == nullptr) {
    return surface_operation(SurfaceOperationStatus::stale);
  }
  if (found->owner != owner) {
    return surface_operation(SurfaceOperationStatus::wrong_owner);
  }
  const auto attachment_id = found->attachment;
  enqueue_surface_event(owner, id, PendingSurfaceEventKind::closed);
  retained_surface_bytes_ -= found->grid.retained_bytes();
  replace_surface(id.slot(), std::nullopt);
  --surface_count_;
  if (focused_surfaces_.at(attachment_id.slot()) == id) {
    focused_surfaces_.at(attachment_id.slot()) = {};
  }
  if (captured_surfaces_.at(attachment_id.slot()) == id) {
    captured_surfaces_.at(attachment_id.slot()) = {};
  }
  auto& geometry = geometry_generations_.at(attachment_id.slot());
  geometry = geometry == std::numeric_limits<std::uint64_t>::max() ? 1U : geometry + 1U;
  return {.status = SurfaceOperationStatus::applied,
          .surface = id,
          .pane_viewport = pane_viewport(attachment_id, viewport).value_or(PaneRectangle{})};
}

auto Runtime::focus_surface(const ExtensionGenerationId owner, const SurfaceId id,
                            const render::Viewport viewport) noexcept -> SurfaceOperationResult {
  auto* const found = surface(id);
  if (found == nullptr) {
    return surface_operation(SurfaceOperationStatus::stale);
  }
  if (found->owner != owner) {
    return surface_operation(SurfaceOperationStatus::wrong_owner);
  }
  if (!found->focusable || !resolved_rectangle(*found, viewport).has_value()) {
    return surface_operation(SurfaceOperationStatus::unavailable);
  }
  auto& focused = focused_surfaces_.at(found->attachment.slot());
  if (focused == id) {
    return {.status = SurfaceOperationStatus::no_effect,
            .surface = id,
            .pane_viewport = pane_viewport(found->attachment, viewport).value_or(PaneRectangle{})};
  }
  ExtensionGenerationId blurred_owner;
  if (focused.is_valid()) {
    blurred_owner = surface_owner(focused);
    enqueue_surface_event(blurred_owner, focused, PendingSurfaceEventKind::blurred);
  }
  focused = id;
  enqueue_surface_event(owner, id, PendingSurfaceEventKind::focused);
  if (auto* const destination = peer(blurred_owner); destination != nullptr) {
    flush_surface_events(*destination);
  }
  if (blurred_owner != owner) {
    if (auto* const destination = peer(owner); destination != nullptr) {
      flush_surface_events(*destination);
    }
  }
  return {.status = SurfaceOperationStatus::applied,
          .surface = id,
          .pane_viewport = pane_viewport(found->attachment, viewport).value_or(PaneRectangle{})};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Runtime::apply_surface_update(const ExtensionGenerationId owner,
                                   const std::span<const std::byte> payload) noexcept
    -> SurfaceUpdateResult {
  if (!has_capability(owner, capability_surface)) {
    return {.status = SurfaceOperationStatus::unavailable, .surface = {}, .changed_rows = 0};
  }
  try {
    // Protocol bytes and character storage have the same object representation.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view json(reinterpret_cast<const char*>(payload.data()), payload.size());
    auto parsed = api::parse_json(json);
    SurfaceId id;
    auto patch = parsed.value.has_value() ? decode_grid_patch(*parsed.value, id) : std::nullopt;
    auto* const found = patch.has_value() ? surface(id) : nullptr;
    if (!patch.has_value() || found == nullptr) {
      return surface_update_result(found == nullptr && id.is_valid()
                                       ? SurfaceOperationStatus::stale
                                       : SurfaceOperationStatus::invalid,
                                   id);
    }
    if (found->owner != owner) {
      return surface_update_result(SurfaceOperationStatus::wrong_owner, id);
    }
    const auto previous_retained_bytes = found->grid.retained_bytes();
    const auto aggregate_available = limits::surface_retained_bytes_aggregate_max -
                                     (retained_surface_bytes_ - previous_retained_bytes);
    const auto applied = found->grid.apply(
        std::move(*patch), std::min(limits::surface_retained_bytes_max, aggregate_available));
    if (!applied.has_value()) {
      return surface_update_result(applied.error() == render::GridError::resource_limit ||
                                           applied.error() == render::GridError::out_of_memory
                                       ? SurfaceOperationStatus::capacity
                                       : SurfaceOperationStatus::invalid,
                                   id);
    }
    retained_surface_bytes_ =
        retained_surface_bytes_ - previous_retained_bytes + applied->retained_bytes;
    return {.status =
                applied->changed_rows == 0 && !applied->cursor_changed && !applied->styles_changed
                    ? SurfaceOperationStatus::no_effect
                    : SurfaceOperationStatus::applied,
            .surface = id,
            .changed_rows = applied->changed_rows};
  } catch (const std::bad_alloc&) {
    return {.status = SurfaceOperationStatus::capacity, .surface = {}, .changed_rows = 0};
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Runtime::resize_surfaces(const AttachmentId attachment_id,
                              const render::Viewport viewport) noexcept -> bool {
  if (surface_count_ == 0) {
    return true;
  }
  const auto layout = resolve_layout(attachment_id, viewport);
  std::array<std::optional<render::Grid>, limits::extension_surfaces_hard_max> replacements{};
  auto proposed_bytes = retained_surface_bytes_;
  for (std::size_t index = 0; index < surfaces_.size(); ++index) {
    const auto& slot = surfaces_.at(index);
    if (!slot.surface.has_value() || slot.surface->attachment != attachment_id) {
      continue;
    }
    const auto rectangle = layout.rectangles.at(index);
    if (!rectangle.has_value()) {
      continue;
    }
    if (slot.surface->grid.columns() == rectangle->columns &&
        slot.surface->grid.rows() == rectangle->rows) {
      continue;
    }
    auto replacement = slot.surface->grid.resized(rectangle->columns, rectangle->rows);
    if (!replacement.has_value() ||
        replacement->retained_bytes() >
            limits::surface_retained_bytes_aggregate_max -
                (proposed_bytes - slot.surface->grid.retained_bytes())) {
      return false;
    }
    proposed_bytes =
        proposed_bytes - slot.surface->grid.retained_bytes() + replacement->retained_bytes();
    replacements.at(index) = std::move(*replacement);
  }
  for (std::size_t index = 0; index < surfaces_.size(); ++index) {
    if (replacements.at(index).has_value()) {
      const auto& current = *surfaces_.at(index).surface;
      const auto id = current.id;
      const auto owner = current.owner;
      replace_surface(index, Surface{.id = id,
                                     .owner = owner,
                                     .attachment = current.attachment,
                                     .placement = current.placement,
                                     .grid = std::move(*replacements.at(index)),
                                     .focusable = current.focusable,
                                     .opaque = current.opaque});
      const auto& resized = *surfaces_.at(index).surface;
      enqueue_surface_event(owner, id, PendingSurfaceEventKind::resized, resized.grid.columns(),
                            resized.grid.rows());
    }
  }
  retained_surface_bytes_ = proposed_bytes;
  const auto focused = focused_surface(attachment_id);
  if (focused.is_valid() && !layout.rectangles.at(focused.slot()).has_value()) {
    static_cast<void>(focus_pane(attachment_id));
  }
  const auto captured = captured_surface_pointer(attachment_id);
  if (captured.is_valid() && !layout.rectangles.at(captured.slot()).has_value()) {
    release_surface_pointer(attachment_id);
  }
  for (auto& slot : peers_) {
    if (slot.peer.has_value()) {
      flush_surface_events(*slot.peer);
    }
  }
  return true;
}

auto Runtime::collect_surfaces(
    const AttachmentId attachment_id, const render::Viewport viewport,
    std::array<render::GridSurface, limits::extension_surfaces_hard_max>& storage) noexcept
    -> std::span<const render::GridSurface> {
  if (surface_count_ == 0) {
    return {};
  }
  const auto layout = resolve_layout(attachment_id, viewport);
  std::size_t count = 0;
  std::optional<std::size_t> passive_cursor;
  const auto focused = focused_surface(attachment_id);
  for (auto& slot : surfaces_) {
    if (!slot.surface.has_value() || slot.surface->attachment != attachment_id) {
      continue;
    }
    const auto rectangle = layout.rectangles.at(slot.surface->id.slot());
    if (!rectangle.has_value() || slot.surface->grid.columns() != rectangle->columns ||
        slot.surface->grid.rows() != rectangle->rows) {
      continue;
    }
    if (!slot.surface->focusable && slot.surface->grid.cursor().visible) {
      passive_cursor = count;
    }
    storage.at(count++) = {.grid = &slot.surface->grid,
                           .rectangle = *rectangle,
                           .focused = focused == slot.surface->id,
                           .opaque = slot.surface->opaque};
  }
  // Cursor display is independent of keyboard ownership for nonfocusable Surfaces. A focused
  // extension always wins; otherwise the highest passive cursor is projected over the Pane.
  if (!focused.is_valid() && passive_cursor.has_value()) {
    storage.at(*passive_cursor).focused = true;
  }
  return std::span(storage).first(count);
}

auto Runtime::focused_surface(const AttachmentId attachment_id) const noexcept -> SurfaceId {
  if (surface_count_ == 0 || !attachment_id.is_valid() ||
      attachment_id.slot() >= focused_surfaces_.size()) {
    return {};
  }
  const auto id = focused_surfaces_.at(attachment_id.slot());
  const auto* const found = surface(id);
  return found != nullptr && found->attachment == attachment_id && connected(found->owner)
             ? id
             : SurfaceId{};
}

auto Runtime::focus_pane(const AttachmentId attachment_id) noexcept -> bool {
  const auto focused = focused_surface(attachment_id);
  if (!focused.is_valid()) {
    return false;
  }
  const auto owner = surface_owner(focused);
  focused_surfaces_.at(attachment_id.slot()) = {};
  enqueue_surface_event(owner, focused, PendingSurfaceEventKind::blurred);
  if (auto* const destination = peer(owner); destination != nullptr) {
    flush_surface_events(*destination);
  }
  return true;
}

auto Runtime::geometry_generation(const AttachmentId attachment_id) const noexcept
    -> std::uint64_t {
  return attachment_id.is_valid() && attachment_id.slot() < geometry_generations_.size()
             ? geometry_generations_.at(attachment_id.slot())
             : 0;
}

auto Runtime::surface_at(const AttachmentId attachment_id, const render::Viewport viewport,
                         const std::uint16_t column, const std::uint16_t row) const noexcept
    -> SurfaceId {
  if (surface_count_ == 0) {
    return {};
  }
  for (const auto& slot : surfaces_ | std::views::reverse) {
    if (!slot.surface.has_value() || slot.surface->attachment != attachment_id ||
        !connected(slot.surface->owner)) {
      continue;
    }
    const auto rectangle = resolved_rectangle(*slot.surface, viewport);
    if (rectangle.has_value() && column >= rectangle->column && row >= rectangle->row &&
        column < rectangle->column + rectangle->columns && row < rectangle->row + rectangle->rows) {
      return slot.surface->id;
    }
  }
  return {};
}

auto Runtime::surface_rectangle(const SurfaceId id, const render::Viewport viewport) const noexcept
    -> std::optional<PaneRectangle> {
  const auto* const found = surface(id);
  return found == nullptr ? std::nullopt : resolved_rectangle(*found, viewport);
}

auto Runtime::surface_owner(const SurfaceId id) const noexcept -> ExtensionGenerationId {
  const auto* const found = surface(id);
  return found == nullptr ? ExtensionGenerationId{} : found->owner;
}

auto Runtime::surface_focusable(const SurfaceId id) const noexcept -> bool {
  const auto* const found = surface(id);
  return found != nullptr && found->focusable;
}

void Runtime::capture_surface_pointer(const AttachmentId attachment_id,
                                      const SurfaceId id) noexcept {
  const auto* const found = surface(id);
  if (attachment_id.is_valid() && attachment_id.slot() < captured_surfaces_.size() &&
      found != nullptr && found->attachment == attachment_id && connected(found->owner)) {
    captured_surfaces_.at(attachment_id.slot()) = id;
  }
}

auto Runtime::captured_surface_pointer(const AttachmentId attachment_id) const noexcept
    -> SurfaceId {
  if (!attachment_id.is_valid() || attachment_id.slot() >= captured_surfaces_.size()) {
    return {};
  }
  const auto id = captured_surfaces_.at(attachment_id.slot());
  const auto* const found = surface(id);
  return found != nullptr && found->attachment == attachment_id && connected(found->owner)
             ? id
             : SurfaceId{};
}

void Runtime::release_surface_pointer(const AttachmentId attachment_id) noexcept {
  if (captured_surface_pointer(attachment_id).is_valid()) {
    captured_surfaces_.at(attachment_id.slot()) = {};
  }
}

void Runtime::revoke_session(const SessionId session_id) noexcept {
  for (auto& slot : peers_) {
    if (slot.peer.has_value() && slot.peer->session == session_id) {
      static_cast<void>(disconnect(slot.peer->owner));
    }
  }
}

auto Runtime::disconnect(const ExtensionGenerationId owner) noexcept -> AttachmentId {
  auto* const found = peer(owner);
  if (found == nullptr) {
    return {};
  }
  const auto attachment_id = found->attachment;
  for (auto& slot : surfaces_) {
    if (slot.surface.has_value() && slot.surface->owner == owner) {
      retained_surface_bytes_ -= slot.surface->grid.retained_bytes();
      slot.surface.reset();
      --surface_count_;
    }
  }
  if (attachment_id.is_valid() && attachment_id.slot() < focused_surfaces_.size()) {
    const auto focused = focused_surfaces_.at(attachment_id.slot());
    if (surface(focused) == nullptr) {
      focused_surfaces_.at(attachment_id.slot()) = {};
    }
    const auto captured = captured_surfaces_.at(attachment_id.slot());
    if (surface(captured) == nullptr) {
      captured_surfaces_.at(attachment_id.slot()) = {};
    }
  }
  if (attachment_id.is_valid()) {
    auto& geometry = geometry_generations_.at(attachment_id.slot());
    geometry = geometry == std::numeric_limits<std::uint64_t>::max() ? 1U : geometry + 1U;
  }
  peers_.at(owner.slot()).peer.reset();
  --peer_count_;
  return attachment_id;
}

auto Runtime::reap_disconnected(
    std::array<AttachmentId, limits::extension_sessions_hard_max>& affected) noexcept
    -> std::span<const AttachmentId> {
  std::size_t count = 0;
  for (auto& slot : peers_) {
    if (!slot.peer.has_value() || slot.peer->transport.connected()) {
      continue;
    }
    const auto owner = slot.peer->owner;
    const auto attachment_id = disconnect(owner);
    if (attachment_id.is_valid() &&
        std::ranges::find(std::span(affected).first(count), attachment_id) ==
            std::span(affected).first(count).end()) {
      affected.at(count++) = attachment_id;
    }
  }
  return std::span(affected).first(count);
}

auto surface_operation_status_name(const SurfaceOperationStatus status) noexcept
    -> std::string_view {
  switch (status) {
  case SurfaceOperationStatus::applied:
    return "applied";
  case SurfaceOperationStatus::no_effect:
    return "no_effect";
  case SurfaceOperationStatus::stale:
    return "stale";
  case SurfaceOperationStatus::wrong_owner:
    return "wrong_owner";
  case SurfaceOperationStatus::invalid:
    return "invalid";
  case SurfaceOperationStatus::capacity:
    return "capacity";
  case SurfaceOperationStatus::unavailable:
    return "unavailable";
  }
  return "invalid";
}

// NOLINTEND(bugprone-exception-escape,bugprone-unchecked-optional-access)

} // namespace frame::extension
