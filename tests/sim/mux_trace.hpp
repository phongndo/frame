#ifndef LEMMA_TESTS_SIM_MUX_TRACE_HPP
#define LEMMA_TESTS_SIM_MUX_TRACE_HPP

#include "core/session_machine.hpp"
#include "lemma/command.hpp"
#include "lemma/id.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lemma::test::sim {

inline constexpr std::size_t mux_trace_operations_max = 16'384;
inline constexpr std::string_view mux_trace_header = "lemma-mux-trace-v1";

enum class MuxOperationKind : std::uint8_t {
  split,
  close_pane,
  focus,
  zoom,
  create_tab,
  close_tab,
  select_tab,
  place_tab,
  swap,
  resize,
  stale_focus,
  spawn_failure,
  resize_failure,
  child_exit,
  runtime_error,
  attachment_resize,
  // argument_0 bit 0 focuses the float and bit 1 holds its exit; argument_1 selects a placement.
  float_create,
  // argument_1 selects a placement.
  float_place,
  // argument_0 is the requested visibility.
  float_visibility,
  idle,
};

inline constexpr std::uint16_t mux_float_placements = 6;
inline constexpr std::uint16_t mux_float_focus = 1U << 0U;
inline constexpr std::uint16_t mux_float_hold = 1U << 1U;

// Every target, argument, and Runtime outcome is concrete. Replaying this value does not invoke the
// generator, so traces remain meaningful when generator probabilities or draw order change.
struct MuxOperation final {
  MuxOperationKind kind{MuxOperationKind::idle};
  TabId tab;
  TabId peer_tab;
  PaneId pane;
  PaneId peer_pane;
  std::uint16_t argument_0{0};
  std::uint16_t argument_1{0};
  core::RuntimeEffectStatus spawn_outcome{core::RuntimeEffectStatus::applied};
  core::RuntimeEffectStatus resize_outcome{core::RuntimeEffectStatus::applied};

  [[nodiscard]] constexpr auto operator==(const MuxOperation&) const noexcept -> bool = default;
};

struct MuxCheckpoint final {
  CommandStatus status{CommandStatus::failed};
  bool mutated{false};
  std::uint64_t state_hash{0};

  [[nodiscard]] constexpr auto operator==(const MuxCheckpoint&) const noexcept -> bool = default;
};

struct MuxTraceEntry final {
  MuxOperation operation;
  std::optional<MuxCheckpoint> checkpoint;

  [[nodiscard]] constexpr auto operator==(const MuxTraceEntry&) const noexcept -> bool = default;
};

[[nodiscard]] auto mux_operation_name(MuxOperationKind kind) noexcept -> std::string_view;
void write_mux_trace_header(std::ostream& stream);
void write_mux_operation(std::ostream& stream, const MuxOperation& operation);
void write_mux_checkpoint(std::ostream& stream, const MuxCheckpoint& checkpoint);

[[nodiscard]] auto read_mux_trace(std::istream& stream, std::vector<MuxTraceEntry>& entries,
                                  std::string& error) -> bool;
[[nodiscard]] auto read_mux_trace_file(const std::filesystem::path& path,
                                       std::vector<MuxTraceEntry>& entries, std::string& error)
    -> bool;
[[nodiscard]] auto write_mux_trace_file(const std::filesystem::path& path,
                                        std::span<const MuxTraceEntry> entries, std::string& error)
    -> bool;

} // namespace lemma::test::sim

#endif // LEMMA_TESTS_SIM_MUX_TRACE_HPP
