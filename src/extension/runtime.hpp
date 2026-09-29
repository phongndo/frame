#ifndef FRAME_EXTENSION_RUNTIME_HPP
#define FRAME_EXTENSION_RUNTIME_HPP

#include "api/command.hpp"
#include "api/json.hpp"
#include "extension/protocol.hpp"
#include "frame/geometry.hpp"
#include "frame/id.hpp"
#include "frame/limits.hpp"
#include "render/grid.hpp"
#include "render/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace frame::extension {

inline constexpr std::uint8_t capability_observe = 1U << 0U;
inline constexpr std::uint8_t capability_proc = 1U << 1U;
inline constexpr std::uint8_t capability_surface = 1U << 2U;

struct Hello final {
  std::string name;
  api::EventSubscription subscription;
  std::uint8_t capabilities{0};
};

[[nodiscard]] auto decode_hello(const api::JsonValue& document) -> std::optional<Hello>;

// Append one lossless text or opaque-byte member; transport chunks need not end on UTF-8
// boundaries.
[[nodiscard]] auto append_input_payload(std::string& event, std::span<const std::byte> bytes,
                                        bool opaque) -> bool;

enum class SurfaceOperationStatus : std::uint8_t {
  applied,
  no_effect,
  stale,
  wrong_owner,
  invalid,
  capacity,
  unavailable,
};

struct SurfaceOperationResult final {
  SurfaceOperationStatus status{SurfaceOperationStatus::invalid};
  SurfaceId surface;
  PaneRectangle pane_viewport{};
};

struct SurfaceUpdateResult final {
  SurfaceOperationStatus status{SurfaceOperationStatus::invalid};
  SurfaceId surface;
  std::size_t changed_rows{0};
};

struct PeerView final {
  ExtensionGenerationId owner;
  int descriptor{-1};
  short events{0};
  std::size_t slot{0};
};

struct RuntimeAccounting final {
  OutputAccounting output;
  std::size_t input_bytes{0};
  std::size_t output_bytes{0};
  std::size_t buffered_records{0};
  std::size_t peers{0};
  std::size_t surfaces{0};
  std::size_t retained_surface_bytes{0};

  auto operator==(const RuntimeAccounting&) const -> bool = default;
};

// Reactor-owned authority for language-neutral extension sessions and attachment-local Surfaces.
// The reactor remains the only caller; no synchronization or extension callback enters this module.
class Runtime final {
public:
  Runtime() noexcept = default;
  Runtime(const Runtime&) = delete;
  auto operator=(const Runtime&) -> Runtime& = delete;
  Runtime(Runtime&&) = delete;
  auto operator=(Runtime&&) -> Runtime& = delete;
  ~Runtime() = default;

  [[nodiscard]] auto admit(FramedPeer transport, Hello hello, SessionId session_id,
                           AttachmentId attachment_id, std::string_view snapshot) noexcept
      -> std::optional<ExtensionGenerationId>;
  [[nodiscard]] auto
  peer_views(std::array<PeerView, limits::extension_sessions_hard_max>& storage) const noexcept
      -> std::span<const PeerView>;
  [[nodiscard]] auto
  read_ready(std::size_t slot,
             std::size_t bytes_max = limits::extension_io_bytes_per_turn_max) noexcept
      -> std::size_t;
  [[nodiscard]] auto buffered_record_bytes(std::size_t slot) const noexcept
      -> std::optional<std::size_t>;
  [[nodiscard]] auto buffered_work() const noexcept -> bool;
  [[nodiscard]] auto
  write_ready(std::size_t slot,
              std::size_t bytes_max = limits::extension_io_bytes_per_turn_max) noexcept
      -> std::size_t;
  [[nodiscard]] auto receive(std::size_t slot) noexcept -> std::optional<Record>;
  void consume(std::size_t slot) noexcept;
  [[nodiscard]] auto owner_at(std::size_t slot) const noexcept -> ExtensionGenerationId;
  [[nodiscard]] auto connected(ExtensionGenerationId owner) const noexcept -> bool;
  [[nodiscard]] auto output_bytes(ExtensionGenerationId owner) const noexcept -> std::size_t;
  [[nodiscard]] auto output_accounting(ExtensionGenerationId owner) const noexcept
      -> OutputAccounting;
  [[nodiscard]] auto event_sequence(ExtensionGenerationId owner) const noexcept -> std::uint32_t;
  [[nodiscard]] auto has_capability(ExtensionGenerationId owner,
                                    std::uint8_t capability) const noexcept -> bool;
  [[nodiscard]] auto subscription(ExtensionGenerationId owner) const noexcept
      -> const api::EventSubscription*;
  [[nodiscard]] auto session(ExtensionGenerationId owner) const noexcept -> SessionId;
  [[nodiscard]] auto attachment(ExtensionGenerationId owner) const noexcept -> AttachmentId;
  [[nodiscard]] auto accounting() const noexcept -> RuntimeAccounting;

  [[nodiscard]] auto reserve_proc(ExtensionGenerationId owner, std::uint32_t request_id) noexcept
      -> bool;
  void complete_proc(ExtensionGenerationId owner, std::uint32_t request_id,
                     std::string_view result) noexcept;

  [[nodiscard]] auto begin_surface_transaction(ExtensionGenerationId owner) noexcept -> bool;
  void commit_surface_transaction(ExtensionGenerationId owner) noexcept;
  [[nodiscard]] auto rollback_surface_transaction(ExtensionGenerationId owner) noexcept -> bool;
  [[nodiscard]] auto send_event(ExtensionGenerationId owner, std::string_view event) noexcept
      -> bool;
  [[nodiscard]] auto send_error(ExtensionGenerationId owner, std::uint32_t sequence,
                                std::string_view reason) noexcept -> bool;

  [[nodiscard]] auto create_surface(ExtensionGenerationId owner, api::SurfacePlacement placement,
                                    bool focusable, bool opaque, render::Viewport viewport) noexcept
      -> SurfaceOperationResult;
  [[nodiscard]] auto configure_surface(ExtensionGenerationId owner, SurfaceId id,
                                       api::SurfacePlacement placement, render::Viewport viewport,
                                       std::optional<bool> focusable = std::nullopt) noexcept
      -> SurfaceOperationResult;
  [[nodiscard]] auto close_surface(ExtensionGenerationId owner, SurfaceId id,
                                   render::Viewport viewport) noexcept -> SurfaceOperationResult;
  [[nodiscard]] auto focus_surface(ExtensionGenerationId owner, SurfaceId id,
                                   render::Viewport viewport) noexcept -> SurfaceOperationResult;
  [[nodiscard]] auto apply_surface_update(ExtensionGenerationId owner,
                                          std::span<const std::byte> payload) noexcept
      -> SurfaceUpdateResult;

  [[nodiscard]] auto pane_viewport(AttachmentId attachment,
                                   render::Viewport viewport) const noexcept
      -> std::optional<PaneRectangle>;
  [[nodiscard]] auto resize_surfaces(AttachmentId attachment, render::Viewport viewport) noexcept
      -> bool;
  [[nodiscard]] auto collect_surfaces(
      AttachmentId attachment, render::Viewport viewport,
      std::array<render::GridSurface, limits::extension_surfaces_hard_max>& storage) noexcept
      -> std::span<const render::GridSurface>;
  [[nodiscard]] auto focused_surface(AttachmentId attachment) const noexcept -> SurfaceId;
  [[nodiscard]] auto focus_pane(AttachmentId attachment) noexcept -> bool;
  [[nodiscard]] auto geometry_generation(AttachmentId attachment) const noexcept -> std::uint64_t;
  [[nodiscard]] auto surface_at(AttachmentId attachment, render::Viewport viewport,
                                std::uint16_t column, std::uint16_t row) const noexcept
      -> SurfaceId;
  [[nodiscard]] auto surface_rectangle(SurfaceId id, render::Viewport viewport) const noexcept
      -> std::optional<PaneRectangle>;
  [[nodiscard]] auto surface_owner(SurfaceId id) const noexcept -> ExtensionGenerationId;
  [[nodiscard]] auto surface_focusable(SurfaceId id) const noexcept -> bool;
  void capture_surface_pointer(AttachmentId attachment_id, SurfaceId id) noexcept;
  [[nodiscard]] auto captured_surface_pointer(AttachmentId attachment) const noexcept -> SurfaceId;
  void release_surface_pointer(AttachmentId attachment) noexcept;

  // Disconnect invalidates the generation and removes all owned projection state without invoking
  // extension code. The returned Attachment identifies geometry/focus that the reactor must repair.
  [[nodiscard]] auto disconnect(ExtensionGenerationId owner) noexcept -> AttachmentId;
  // Semantic scope destruction, not client detach. Revokes peers regardless of capabilities.
  void revoke_session(SessionId session) noexcept;
  [[nodiscard]] auto reap_disconnected(
      std::array<AttachmentId, limits::extension_sessions_hard_max>& affected) noexcept
      -> std::span<const AttachmentId>;

  [[nodiscard]] auto retained_surface_bytes() const noexcept -> std::size_t {
    return retained_surface_bytes_;
  }

private:
  enum class PendingSurfaceEventKind : std::uint8_t {
    resized,
    focused,
    blurred,
    closed,
  };

  struct PendingSurfaceEvent final {
    SurfaceId surface;
    std::uint16_t columns{0};
    std::uint16_t rows{0};
    PendingSurfaceEventKind kind{PendingSurfaceEventKind::resized};
  };

  struct Peer final {
    FramedPeer transport;
    Hello hello;
    ExtensionGenerationId owner;
    SessionId session;
    AttachmentId attachment;
    std::array<std::uint32_t, limits::extension_procs_per_owner_max> procs{};
    std::array<PendingSurfaceEvent, limits::extension_surfaces_per_owner_max * 2U> surface_events{};
    std::size_t surface_event_count{0};
    std::uint32_t next_event_sequence{2};
  };

  struct PeerSlot final {
    std::optional<Peer> peer;
    std::uint32_t generation{0};
  };

  struct Surface final {
    SurfaceId id;
    ExtensionGenerationId owner;
    AttachmentId attachment;
    api::SurfacePlacement placement;
    render::Grid grid;
    bool focusable{true};
    bool opaque{true};
  };

  struct SurfaceSlot final {
    std::optional<Surface> surface;
    std::uint32_t generation{0};
  };

  struct SurfaceTransaction final {
    ExtensionGenerationId owner;
    AttachmentId attachment;
    std::array<std::optional<Surface>, limits::extension_surfaces_hard_max> previous{};
    std::array<bool, limits::extension_surfaces_hard_max> touched{};
    std::array<std::uint32_t, limits::extension_surfaces_hard_max> slot_generations{};
    std::array<std::array<PendingSurfaceEvent, limits::extension_surfaces_per_owner_max * 2U>,
               limits::extension_sessions_hard_max>
        surface_events{};
    std::array<ExtensionGenerationId, limits::extension_sessions_hard_max> peer_owners{};
    std::array<std::size_t, limits::extension_sessions_hard_max> surface_event_counts{};
    SurfaceId focused;
    SurfaceId captured;
    std::uint64_t geometry_generation{0};
    std::size_t retained_surface_bytes{0};
    std::size_t surface_count{0};
  };

  [[nodiscard]] auto peer(ExtensionGenerationId owner) noexcept -> Peer*;
  [[nodiscard]] auto peer(ExtensionGenerationId owner) const noexcept -> const Peer*;
  [[nodiscard]] auto surface(SurfaceId id) noexcept -> Surface*;
  [[nodiscard]] auto surface(SurfaceId id) const noexcept -> const Surface*;
  struct SurfaceLayout final {
    PaneRectangle pane;
    std::array<std::optional<PaneRectangle>, limits::extension_surfaces_hard_max> rectangles{};
  };

  [[nodiscard]] auto resolve_layout(AttachmentId attachment,
                                    render::Viewport viewport) const noexcept -> SurfaceLayout;
  [[nodiscard]] auto resolved_rectangle(const Surface& target,
                                        render::Viewport viewport) const noexcept
      -> std::optional<PaneRectangle>;
  [[nodiscard]] auto owned_surface_count(ExtensionGenerationId owner) const noexcept -> std::size_t;
  void replace_surface(std::size_t slot, std::optional<Surface> replacement) noexcept;
  void enqueue_surface_event(ExtensionGenerationId owner, SurfaceId id,
                             PendingSurfaceEventKind kind, std::uint16_t columns = 0,
                             std::uint16_t rows = 0) noexcept;
  void flush_surface_events(Peer& found) noexcept;

  std::array<PeerSlot, limits::extension_sessions_hard_max> peers_{};
  std::array<SurfaceSlot, limits::extension_surfaces_hard_max> surfaces_{};
  std::array<SurfaceId, limits::sessions_hard_max> focused_surfaces_{};
  std::array<SurfaceId, limits::sessions_hard_max> captured_surfaces_{};
  std::array<std::uint64_t, limits::sessions_hard_max> geometry_generations_{};
  std::optional<SurfaceTransaction> surface_transaction_;
  std::size_t peer_count_{0};
  std::size_t surface_count_{0};
  std::size_t retained_surface_bytes_{0};
};

[[nodiscard]] auto surface_operation_status_name(SurfaceOperationStatus status) noexcept
    -> std::string_view;

} // namespace frame::extension

#endif // FRAME_EXTENSION_RUNTIME_HPP
