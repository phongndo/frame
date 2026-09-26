#ifndef LEMMA_CORE_ENGINE_PROJECTION_HPP
#define LEMMA_CORE_ENGINE_PROJECTION_HPP

#include "api/command.hpp"
#include "core/client_frame_output.hpp"
#include "core/connection_output.hpp"
#include "core/engine_connection_state.hpp"
#include "core/engine_state.hpp"
#include "core/session.hpp"
#include "extension/runtime.hpp"
#include "lemma/command.hpp"
#include "lemma/terminal/terminal.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lemma::core::engine_detail {

[[nodiscard]] constexpr auto clipboard_base64_bytes(const std::size_t bytes) noexcept
    -> std::size_t {
  return ((bytes + 2U) / 3U) * 4U;
}

struct PublicCaptureFormatting final {
  std::expected<std::size_t, vt::Error> result{std::unexpected(vt::Error::invalid_state)};
  bool truncated{false};
};

[[nodiscard]] auto refresh_process_name(PaneRuntime& runtime) noexcept -> bool;
[[nodiscard]] auto pane_presented(const Tab& active, const Pane& pane) noexcept -> bool;
[[nodiscard]] auto current_status_signature(const SessionRecord& session,
                                            const PaneRuntimeStore& runtimes) noexcept
    -> std::uint64_t;
[[nodiscard]] auto tab_title(const SessionRecord& session, const Tab& tab,
                             const PaneRuntimeStore& runtimes) noexcept -> std::string_view;
[[nodiscard]] auto compose_session_frame(SessionRecord& session, PaneRuntimeStore& runtimes,
                                         extension::Runtime& extensions, bool force_full,
                                         ClientFrameOutput::TimePoint now) noexcept -> bool;

[[nodiscard]] auto append_listing(ConnectionOutput& output, const SessionRecord& session,
                                  const PaneRuntimeStore& runtimes) noexcept -> bool;
[[nodiscard]] auto append_tab_listings(ConnectionOutput& output, const SessionRecord& session,
                                       const PaneRuntimeStore& runtimes) noexcept -> bool;
[[nodiscard]] auto append_pane_listings(ConnectionOutput& output, const SessionRecord& session,
                                        const PaneRuntimeStore& runtimes) noexcept -> bool;
[[nodiscard]] auto append_structured_session(ConnectionOutput& output,
                                             const SessionRecord& session) noexcept -> bool;
[[nodiscard]] auto append_structured_tabs(ConnectionOutput& output, const SessionRecord& session,
                                          const PaneRuntimeStore& runtimes) noexcept -> bool;
[[nodiscard]] auto append_structured_panes(ConnectionOutput& output, const SessionRecord& session,
                                           const PaneRuntimeStore& runtimes) noexcept -> bool;
[[nodiscard]] auto append_structured_sessions(ConnectionOutput& output,
                                              const Sessions& sessions) noexcept -> bool;
[[nodiscard]] auto append_all_listings(ConnectionOutput& output, const Sessions& sessions,
                                       const PaneRuntimeStore& runtimes) noexcept -> bool;

[[nodiscard]] auto append_public(std::string& output, std::string_view text) -> bool;
[[nodiscard]] auto append_public_number(std::string& output, std::uint64_t value) -> bool;
template <typename Id>
[[nodiscard]] auto append_public_id(std::string& output, const Id id) -> bool {
  return id.is_valid() && append_public(output, "\"") && append_public_number(output, id.slot()) &&
         append_public(output, ":") && append_public_number(output, id.generation()) &&
         append_public(output, "\"");
}
[[nodiscard]] constexpr auto public_status_name(CommandStatus status) noexcept -> std::string_view;
[[nodiscard]] auto public_session(Sessions& sessions, const api::SessionSelector& selector) noexcept
    -> SessionRecord*;
[[nodiscard]] auto public_tab(SessionRecord& session, const api::TabSelector& selector) noexcept
    -> Tab*;
[[nodiscard]] auto valid_environment(std::span<const std::byte> environment) noexcept -> bool;
[[nodiscard]] auto valid_launch_command(std::span<const std::byte> command) noexcept -> bool;
[[nodiscard]] auto public_launch_command(const api::Command& request,
                                         std::vector<std::byte>& output) -> bool;
[[nodiscard]] auto public_environment(const api::Command& request, std::vector<std::byte>& output)
    -> bool;
[[nodiscard]] auto copy_connection_json(const ConnectionOutput& source, std::string& output)
    -> bool;
[[nodiscard]] auto format_public_visible(vt::Terminal& terminal, vt::ScreenFormat format,
                                         api::CaptureWrap wrap, std::uint16_t lines,
                                         std::span<std::byte> output) noexcept
    -> PublicCaptureFormatting;
[[nodiscard]] auto daemon_inspection(const Sessions& sessions, const PaneRuntimeStore& runtimes)
    -> std::string;
[[nodiscard]] auto session_inspection(const SessionRecord& session) -> std::string;
[[nodiscard]] auto tab_inspection(const SessionRecord& session, const Tab& tab,
                                  const PaneRuntimeStore& runtimes) -> std::string;
[[nodiscard]] auto pane_inspection(const Tab& tab, const Pane& pane, const PaneRuntime& runtime)
    -> std::string;
[[nodiscard]] auto encode_public_error(api::CommandDecodeError error,
                                       std::optional<std::size_t> byte = std::nullopt)
    -> std::string;
[[nodiscard]] auto encode_command_result(const api::Command& request,
                                         const PublicCommandExecution& result, bool target_resolved)
    -> std::string;

[[nodiscard]] auto semantic_hash(const Sessions& sessions,
                                 const std::optional<api::SessionSelector>& filter) noexcept
    -> std::uint64_t;
[[nodiscard]] auto encode_initial_snapshot(PendingConnection& pending, Sessions& sessions,
                                           PaneRuntimeStore& runtimes, std::span<std::byte> scratch,
                                           std::uint64_t sequence = 0) -> std::string;
[[nodiscard]] auto service_public_observers(PendingConnections& connections, Sessions& sessions,
                                            PaneRuntimeStore& runtimes,
                                            PublicScratch& scratch_owner,
                                            std::size_t& cursor) noexcept -> bool;
void service_extension_observers(extension::Runtime& extensions, Sessions& sessions,
                                 PaneRuntimeStore& runtimes, PublicScratch& scratch_owner,
                                 ExtensionObservations& observations, std::size_t& cursor) noexcept;

} // namespace lemma::core::engine_detail

#endif // LEMMA_CORE_ENGINE_PROJECTION_HPP
