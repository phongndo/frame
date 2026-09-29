#include "frame/terminal/terminal.hpp"
#include "render/frame_buffer.hpp"

#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>

namespace {

void print_stats(const char* const label, const frame::vt::AllocationStats stats) {
  std::cout << '"' << label << R"(":{"bytes_current":)" << stats.bytes_current
            << R"(,"bytes_peak":)" << stats.bytes_peak << R"(,"allocations_current":)"
            << stats.allocations_current << R"(,"allocations_total":)" << stats.allocations_total
            << R"(,"failures_total":)" << stats.failures_total << '}';
}

} // namespace

int main() {
  frame::vt::TerminalOptions options;
  auto terminal_result = frame::vt::Terminal::create(options);
  if (!terminal_result.has_value()) {
    return 1;
  }
  auto terminal = std::move(*terminal_result);
  const auto created = terminal.allocation_stats();

  frame::render::FrameBuffer frame;
  if (!frame.prepare({.columns = options.size.columns, .rows = options.size.rows})) {
    return 1;
  }
  const auto rendered = frame::render::compose_retained_single_pane(terminal, frame, true);
  if (!rendered.has_value()) {
    return 1;
  }
  const auto after_render = terminal.allocation_stats();

  constexpr std::string_view line = "history history history\r\n";
  constexpr std::size_t history_input_rows = 25'000;
  for (std::size_t row = 0; row < history_input_rows; ++row) {
    terminal.write(std::as_bytes(std::span(line.data(), line.size())));
  }
  const auto after_history = terminal.allocation_stats();
  const auto scrollback_rows = terminal.scrollback_rows();
  if (!scrollback_rows.has_value()) {
    return 1;
  }

  std::cout << R"({"schema":1,)";
  print_stats("created", created);
  std::cout << ',';
  print_stats("after_initial_render", after_render);
  std::cout << ',';
  print_stats("after_history", after_history);
  std::cout << R"(,"frame_capacity_bytes":)" << frame.capacity() << R"(,"initial_frame_bytes":)"
            << rendered->bytes << R"(,"history_input_rows":)" << history_input_rows
            << R"(,"scrollback_rows":)" << *scrollback_rows << R"(,"scrollback_quota_bytes":)"
            << options.scrollback_bytes_max << "}\n";
  return 0;
}
