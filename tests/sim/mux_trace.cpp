#include "mux_trace.hpp"

#include <array>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace lemma::test::sim {
using namespace std::string_view_literals;
namespace {

using core::RuntimeEffectStatus;

template <typename Value>
[[nodiscard]] auto parse_unsigned(const std::string_view text, Value& value,
                                  const int base = 10) noexcept -> bool {
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(text.begin(), text.end(), parsed, base);
  if (result.ec != std::errc{} || result.ptr != text.end() ||
      parsed > std::numeric_limits<Value>::max()) {
    return false;
  }
  value = static_cast<Value>(parsed);
  return true;
}

template <typename Id> [[nodiscard]] auto parse_id(const std::string_view text, Id& id) {
  if (text == "-") {
    id = {};
    return true;
  }
  const auto separator = text.find(':');
  if (separator == std::string_view::npos ||
      text.find(':', separator + 1U) != std::string_view::npos) {
    return false;
  }
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;
  if (!parse_unsigned(text.substr(0, separator), slot) ||
      !parse_unsigned(text.substr(separator + 1U), generation)) {
    return false;
  }
  const auto parsed = Id::try_from_parts(slot, generation);
  if (!parsed.has_value()) {
    return false;
  }
  id = *parsed;
  return true;
}

template <typename Id> void write_id(std::ostream& stream, const Id id) {
  if (id.is_valid()) {
    stream << id.slot() << ':' << id.generation();
  } else {
    stream << '-';
  }
}

[[nodiscard]] auto parse_operation_kind(const std::string_view text,
                                        MuxOperationKind& kind) noexcept -> bool {
  constexpr std::array values{
      std::pair{"split"sv, MuxOperationKind::split},
      std::pair{"close-pane"sv, MuxOperationKind::close_pane},
      std::pair{"focus"sv, MuxOperationKind::focus},
      std::pair{"zoom"sv, MuxOperationKind::zoom},
      std::pair{"create-tab"sv, MuxOperationKind::create_tab},
      std::pair{"close-tab"sv, MuxOperationKind::close_tab},
      std::pair{"select-tab"sv, MuxOperationKind::select_tab},
      std::pair{"place-tab"sv, MuxOperationKind::place_tab},
      std::pair{"swap"sv, MuxOperationKind::swap},
      std::pair{"resize"sv, MuxOperationKind::resize},
      std::pair{"stale-focus"sv, MuxOperationKind::stale_focus},
      std::pair{"spawn-failure"sv, MuxOperationKind::spawn_failure},
      std::pair{"resize-failure"sv, MuxOperationKind::resize_failure},
      std::pair{"child-exit"sv, MuxOperationKind::child_exit},
      std::pair{"runtime-error"sv, MuxOperationKind::runtime_error},
      std::pair{"attachment-resize"sv, MuxOperationKind::attachment_resize},
      std::pair{"float-create"sv, MuxOperationKind::float_create},
      std::pair{"float-place"sv, MuxOperationKind::float_place},
      std::pair{"float-visibility"sv, MuxOperationKind::float_visibility},
      std::pair{"idle"sv, MuxOperationKind::idle},
  };
  for (const auto& [name, candidate] : values) {
    if (text == name) {
      kind = candidate;
      return true;
    }
  }
  return false;
}

[[nodiscard]] auto effect_status_name(const RuntimeEffectStatus status) noexcept
    -> std::string_view {
  switch (status) {
  case RuntimeEffectStatus::applied:
    return "applied";
  case RuntimeEffectStatus::rejected:
    return "rejected";
  case RuntimeEffectStatus::consistency_lost:
    return "consistency-lost";
  }
  return "unknown";
}

[[nodiscard]] auto parse_effect_status(const std::string_view text,
                                       RuntimeEffectStatus& status) noexcept -> bool {
  if (text == "applied") {
    status = RuntimeEffectStatus::applied;
    return true;
  }
  if (text == "rejected") {
    status = RuntimeEffectStatus::rejected;
    return true;
  }
  if (text == "consistency-lost") {
    status = RuntimeEffectStatus::consistency_lost;
    return true;
  }
  return false;
}

[[nodiscard]] auto command_status_name(const CommandStatus status) noexcept -> std::string_view {
  switch (status) {
  case CommandStatus::applied:
    return "applied";
  case CommandStatus::no_effect:
    return "no-effect";
  case CommandStatus::detach_requested:
    return "detach-requested";
  case CommandStatus::invalid_command:
    return "invalid-command";
  case CommandStatus::invalid_target:
    return "invalid-target";
  case CommandStatus::stale_target:
    return "stale-target";
  case CommandStatus::wrong_owner:
    return "wrong-owner";
  case CommandStatus::conflict:
    return "conflict";
  case CommandStatus::capacity:
    return "capacity";
  case CommandStatus::unavailable:
    return "unavailable";
  case CommandStatus::failed:
    return "failed";
  }
  return "unknown";
}

[[nodiscard]] auto parse_command_status(const std::string_view text, CommandStatus& status) noexcept
    -> bool {
  constexpr std::array values{
      std::pair{"applied"sv, CommandStatus::applied},
      std::pair{"no-effect"sv, CommandStatus::no_effect},
      std::pair{"detach-requested"sv, CommandStatus::detach_requested},
      std::pair{"invalid-command"sv, CommandStatus::invalid_command},
      std::pair{"invalid-target"sv, CommandStatus::invalid_target},
      std::pair{"stale-target"sv, CommandStatus::stale_target},
      std::pair{"wrong-owner"sv, CommandStatus::wrong_owner},
      std::pair{"conflict"sv, CommandStatus::conflict},
      std::pair{"capacity"sv, CommandStatus::capacity},
      std::pair{"unavailable"sv, CommandStatus::unavailable},
      std::pair{"failed"sv, CommandStatus::failed},
  };
  for (const auto& [name, candidate] : values) {
    if (text == name) {
      status = candidate;
      return true;
    }
  }
  return false;
}

[[nodiscard]] auto trailing_token(std::istringstream& stream) -> bool {
  stream >> std::ws;
  return !stream.eof();
}

[[nodiscard]] auto valid_operation_arguments(const MuxOperation& operation) noexcept -> bool {
  switch (operation.kind) {
  case MuxOperationKind::split:
    return operation.argument_0 <= static_cast<std::uint16_t>(core::SplitAxis::top_bottom) &&
           operation.argument_1 <= static_cast<std::uint16_t>(core::PaneExitPolicy::hold);
  case MuxOperationKind::create_tab:
    return operation.argument_0 <= static_cast<std::uint16_t>(core::PaneExitPolicy::hold);
  case MuxOperationKind::float_create:
    return operation.argument_0 <= (mux_float_focus | mux_float_hold) &&
           operation.argument_1 < mux_float_placements;
  case MuxOperationKind::float_place:
    return operation.argument_1 < mux_float_placements;
  case MuxOperationKind::float_visibility:
    return operation.argument_0 <= 1U;
  case MuxOperationKind::resize:
    return operation.argument_0 >= static_cast<std::uint16_t>(CommandKind::resize_left) &&
           operation.argument_0 <= static_cast<std::uint16_t>(CommandKind::resize_down) &&
           operation.argument_1 <= command_resize_amount_max;
  case MuxOperationKind::close_pane:
  case MuxOperationKind::focus:
  case MuxOperationKind::zoom:
  case MuxOperationKind::close_tab:
  case MuxOperationKind::select_tab:
  case MuxOperationKind::place_tab:
  case MuxOperationKind::swap:
  case MuxOperationKind::stale_focus:
  case MuxOperationKind::spawn_failure:
  case MuxOperationKind::resize_failure:
  case MuxOperationKind::child_exit:
  case MuxOperationKind::runtime_error:
  case MuxOperationKind::attachment_resize:
  case MuxOperationKind::idle:
    return true;
  }
  return false;
}

[[nodiscard]] auto parse_operation_line(std::istringstream& stream, MuxOperation& operation)
    -> bool {
  std::string kind;
  std::string tab;
  std::string peer_tab;
  std::string pane;
  std::string peer_pane;
  std::string argument_0;
  std::string argument_1;
  std::string spawn;
  std::string resize;
  if (!(stream >> kind >> tab >> peer_tab >> pane >> peer_pane >> argument_0 >> argument_1 >>
        spawn >> resize) ||
      trailing_token(stream)) {
    return false;
  }
  return parse_operation_kind(kind, operation.kind) && parse_id(tab, operation.tab) &&
         parse_id(peer_tab, operation.peer_tab) && parse_id(pane, operation.pane) &&
         parse_id(peer_pane, operation.peer_pane) &&
         parse_unsigned(argument_0, operation.argument_0) &&
         parse_unsigned(argument_1, operation.argument_1) &&
         parse_effect_status(spawn, operation.spawn_outcome) &&
         parse_effect_status(resize, operation.resize_outcome) &&
         valid_operation_arguments(operation);
}

[[nodiscard]] auto parse_checkpoint_line(std::istringstream& stream, MuxCheckpoint& checkpoint)
    -> bool {
  std::string status;
  std::string mutated;
  std::string state_hash;
  std::uint8_t mutated_value = 0;
  if (!(stream >> status >> mutated >> state_hash) || trailing_token(stream) ||
      !parse_command_status(status, checkpoint.status) || !parse_unsigned(mutated, mutated_value) ||
      mutated_value > 1U || !parse_unsigned(state_hash, checkpoint.state_hash, 16)) {
    return false;
  }
  checkpoint.mutated = mutated_value != 0;
  return true;
}

} // namespace

[[nodiscard]] auto mux_operation_name(const MuxOperationKind kind) noexcept -> std::string_view {
  switch (kind) {
  case MuxOperationKind::split:
    return "split";
  case MuxOperationKind::close_pane:
    return "close-pane";
  case MuxOperationKind::focus:
    return "focus";
  case MuxOperationKind::zoom:
    return "zoom";
  case MuxOperationKind::create_tab:
    return "create-tab";
  case MuxOperationKind::close_tab:
    return "close-tab";
  case MuxOperationKind::select_tab:
    return "select-tab";
  case MuxOperationKind::place_tab:
    return "place-tab";
  case MuxOperationKind::swap:
    return "swap";
  case MuxOperationKind::resize:
    return "resize";
  case MuxOperationKind::stale_focus:
    return "stale-focus";
  case MuxOperationKind::spawn_failure:
    return "spawn-failure";
  case MuxOperationKind::resize_failure:
    return "resize-failure";
  case MuxOperationKind::child_exit:
    return "child-exit";
  case MuxOperationKind::runtime_error:
    return "runtime-error";
  case MuxOperationKind::attachment_resize:
    return "attachment-resize";
  case MuxOperationKind::float_create:
    return "float-create";
  case MuxOperationKind::float_place:
    return "float-place";
  case MuxOperationKind::float_visibility:
    return "float-visibility";
  case MuxOperationKind::idle:
    return "idle";
  }
  return "unknown";
}

void write_mux_trace_header(std::ostream& stream) { stream << mux_trace_header << '\n'; }

void write_mux_operation(std::ostream& stream, const MuxOperation& operation) {
  stream << "op " << mux_operation_name(operation.kind) << ' ';
  write_id(stream, operation.tab);
  stream << ' ';
  write_id(stream, operation.peer_tab);
  stream << ' ';
  write_id(stream, operation.pane);
  stream << ' ';
  write_id(stream, operation.peer_pane);
  stream << ' ' << operation.argument_0 << ' ' << operation.argument_1 << ' '
         << effect_status_name(operation.spawn_outcome) << ' '
         << effect_status_name(operation.resize_outcome) << '\n';
}

void write_mux_checkpoint(std::ostream& stream, const MuxCheckpoint& checkpoint) {
  stream << "check " << command_status_name(checkpoint.status) << ' '
         << static_cast<unsigned int>(checkpoint.mutated) << ' ' << std::hex << std::setw(16)
         << std::setfill('0') << checkpoint.state_hash << std::dec << std::setfill(' ') << '\n';
}

// Parsing dispatches a bounded versioned record grammar.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] auto read_mux_trace(std::istream& stream, std::vector<MuxTraceEntry>& entries,
                                  std::string& error) -> bool {
  entries.clear();
  error.clear();
  std::string line;
  std::size_t line_number = 0;
  bool header_read = false;
  while (std::getline(stream, line)) {
    ++line_number;
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    if (!header_read) {
      if (line != mux_trace_header) {
        error = "line " + std::to_string(line_number) + ": unsupported mux trace header";
        return false;
      }
      header_read = true;
      continue;
    }
    std::istringstream line_stream(line);
    std::string record;
    line_stream >> record;
    if (record == "op") {
      if (entries.size() == mux_trace_operations_max) {
        error = "line " + std::to_string(line_number) + ": mux trace capacity exceeded";
        return false;
      }
      MuxTraceEntry entry;
      if (!parse_operation_line(line_stream, entry.operation)) {
        error = "line " + std::to_string(line_number) + ": invalid mux operation";
        return false;
      }
      entries.push_back(entry);
      continue;
    }
    if (record == "check") {
      if (entries.empty() || entries.back().checkpoint.has_value()) {
        error = "line " + std::to_string(line_number) + ": misplaced mux checkpoint";
        return false;
      }
      MuxCheckpoint checkpoint;
      if (!parse_checkpoint_line(line_stream, checkpoint)) {
        error = "line " + std::to_string(line_number) + ": invalid mux checkpoint";
        return false;
      }
      entries.back().checkpoint = checkpoint;
      continue;
    }
    error = "line " + std::to_string(line_number) + ": unknown mux trace record";
    return false;
  }
  if (!header_read) {
    error = "missing mux trace header";
    return false;
  }
  return true;
}

[[nodiscard]] auto read_mux_trace_file(const std::filesystem::path& path,
                                       std::vector<MuxTraceEntry>& entries, std::string& error)
    -> bool {
  std::ifstream stream(path);
  if (!stream.is_open()) {
    error = "could not open " + path.string();
    return false;
  }
  if (!read_mux_trace(stream, entries, error)) {
    error = path.string() + ": " + error;
    return false;
  }
  return true;
}

[[nodiscard]] auto write_mux_trace_file(const std::filesystem::path& path,
                                        const std::span<const MuxTraceEntry> entries,
                                        std::string& error) -> bool {
  error.clear();
  std::error_code directory_error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), directory_error);
  }
  if (directory_error) {
    error = "could not create mux trace directory " + path.parent_path().string() + ": " +
            directory_error.message();
    return false;
  }
  std::ofstream stream(path, std::ios::trunc);
  if (!stream.is_open()) {
    error = "could not create " + path.string();
    return false;
  }
  write_mux_trace_header(stream);
  for (const auto& entry : entries) {
    write_mux_operation(stream, entry.operation);
    if (entry.checkpoint.has_value()) {
      write_mux_checkpoint(stream, *entry.checkpoint);
    }
  }
  stream.flush();
  if (!stream.good()) {
    error = "could not write " + path.string();
    return false;
  }
  return true;
}

} // namespace lemma::test::sim
