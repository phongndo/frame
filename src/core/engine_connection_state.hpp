#ifndef LEMMA_CORE_ENGINE_CONNECTION_STATE_HPP
#define LEMMA_CORE_ENGINE_CONNECTION_STATE_HPP

#include "api/command.hpp"
#include "api/json.hpp"
#include "core/connection_output.hpp"
#include "core/engine_state.hpp"
#include "core/session.hpp"
#include "extension/protocol.hpp"
#include "lemma/command.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"
#include "protocol/attachment.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lemma::core::engine_detail {

inline constexpr auto setup_progress_timeout = std::chrono::seconds(5);
inline constexpr auto setup_total_timeout = std::chrono::seconds(10);

enum class PendingState : std::uint8_t {
  unused,
  read_command,
  read_attach,
  read_name_size,
  read_name,
  read_mutation_position,
  read_mutation_size,
  read_mutation,
  read_create_flags,
  read_working_directory_size,
  read_working_directory,
  read_environment_size,
  read_environment,
  read_launch_command_size,
  read_launch_command,
  read_control_payload_size,
  read_control_payload,
  read_public_json,
  read_extension,
  execute_public_proc,
  prepare_public_observer,
  observe,
  flush_response,
};

enum class PendingDisposition : std::uint8_t {
  close,
  attach,
  keep_proc,
  keep_observe,
  shutdown,
};

struct PublicProcId final {
  std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()};
  std::uint32_t generation{0};

  [[nodiscard]] auto valid() const noexcept -> bool {
    return slot < limits::pending_connections_hard_max && generation > 0;
  }
  [[nodiscard]] auto operator==(const PublicProcId&) const noexcept -> bool = default;
};

struct ProcCommandWait final {
  api::Command request;
  std::chrono::steady_clock::time_point deadline;
  std::uint64_t observed_terminal_generation{0};
  // until_command completes once the Pane's OSC 133;D count exceeds this baseline.
  std::uint64_t command_baseline{0};
  // A close-on-exit Pane disappears in the transition that publishes its exit.
  std::optional<ProcessExit> exit;
  bool observed{false};
  bool pane_was_present{false};
};

struct PublicObservedPaneState final {
  std::uint64_t terminal_generation{0};
  ProcessExit process{};
  bool present{false};
  bool process_exited{false};
};

struct ExtensionObservation final {
  ExtensionGenerationId owner;
  std::array<PublicObservedPaneState, api::event_panes_max> panes{};
  std::uint64_t semantic_hash{0};
  std::uint64_t presentation_hash{0};
  std::uint64_t signal_stamp{0};
  std::size_t pane_cursor{0};
};
using ExtensionObservations = std::array<ExtensionObservation, limits::extension_sessions_hard_max>;

struct PendingConnection final {
  PendingConnection() = default;
  PendingConnection(const PendingConnection&) = delete;
  auto operator=(const PendingConnection&) -> PendingConnection& = delete;
  PendingConnection(PendingConnection&&) = delete;
  auto operator=(PendingConnection&&) -> PendingConnection& = delete;
  ~PendingConnection() = default;
  [[nodiscard]] auto active() const noexcept -> bool { return state != PendingState::unused; }

  int descriptor{-1};
  std::uint32_t generation{0};
  PendingState state{PendingState::unused};
  PendingDisposition disposition{PendingDisposition::close};
  std::byte command{};
  SessionName session;
  WorkingDirectory working_directory;
  std::vector<std::byte> environment;
  std::size_t environment_size{0};
  std::vector<std::byte> launch_command;
  std::size_t launch_command_size{0};
  PaneExitPolicy exit_policy{PaneExitPolicy::close};
  std::array<char, protocol::tab_title_bytes_max> mutation_text{};
  std::size_t mutation_text_size{0};
  std::uint8_t mutation_position{0};
  std::array<std::byte, protocol::environment_bytes_max> field{};
  std::size_t field_size{0};
  std::size_t field_target{0};
  ConnectionOutput output;
  std::string public_input;
  std::string public_output;
  std::size_t public_output_offset{0};
  PublicProcId proc;
  api::EventSubscription subscription;
  std::uint64_t event_sequence{0};
  std::uint64_t observed_semantic_hash{0};
  std::uint64_t observed_presentation_hash{0};
  std::uint64_t observed_signal_stamp{0};
  std::array<PublicObservedPaneState, api::event_panes_max> observed_panes{};
  std::size_t observed_pane_cursor{0};
  bool public_connection{false};
  std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()};
  protocol::ClientDecoder attach_decoder;
  std::unique_ptr<extension::FramedPeer> extension_peer;
  protocol::Dimensions attach_dimensions{};
  std::optional<protocol::HostTerminalTheme> attach_host_theme;
  SessionId attach_session;
  std::chrono::steady_clock::time_point deadline;
  std::chrono::steady_clock::time_point setup_deadline;
};

using PendingConnections =
    std::array<std::unique_ptr<PendingConnection>, limits::pending_connections_hard_max>;
using PendingConnectionGenerations =
    std::array<std::uint32_t, limits::pending_connections_hard_max>;
using PublicScratchStorage = std::array<std::byte, api::json_bytes_max>;
using PublicScratch = std::unique_ptr<PublicScratchStorage>;
inline constexpr std::size_t public_capture_bytes_max =
    (api::json_bytes_max - (std::size_t{4} * 1'024U)) / 6U;

struct ObservedPane final {
  SessionRecord* session{nullptr};
  Pane* pane{nullptr};
  PaneRuntime* runtime{nullptr};
};

struct PublicCommandExecution final {
  CommandStatus status{CommandStatus::failed};
  std::string session_name;
  SessionId session;
  TabId tab;
  PaneId pane;
  std::string value_field;
  std::string value_json;
  std::string text;
  std::string error_reason;
  std::optional<ProcessExit> process;
  std::uint64_t session_revision{0};
  std::uint64_t terminal_generation{0};
  api::CaptureSource capture_source{api::CaptureSource::visible};
  api::CaptureFormat capture_format{api::CaptureFormat::plain};
  api::CaptureWrap capture_wrap{api::CaptureWrap::rendered};
  bool has_text{false};
  bool has_capture{false};
  bool capture_truncated{false};
  bool retryable{false};
};

[[nodiscard]] auto acquire_public_scratch(PublicScratch& owner) noexcept -> std::span<std::byte>;
void finish_public_output(PendingConnection& pending, std::string output,
                          PendingDisposition disposition) noexcept;

static_assert(sizeof(PendingConnection) <= std::size_t{160} * 1'024U);
static_assert(sizeof(PendingConnections) ==
              limits::pending_connections_hard_max * sizeof(std::unique_ptr<PendingConnection>));
static_assert(sizeof(PendingConnectionGenerations) ==
              limits::pending_connections_hard_max * sizeof(std::uint32_t));

} // namespace lemma::core::engine_detail

#endif // LEMMA_CORE_ENGINE_CONNECTION_STATE_HPP
