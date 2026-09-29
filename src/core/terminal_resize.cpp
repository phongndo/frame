#include "core/terminal_resize.hpp"

#include "frame/terminal/terminal.hpp"

namespace frame::core {

[[nodiscard]] auto
resize_terminal_transaction(vt::Terminal& terminal, const vt::TerminalSize& requested,
                            const PtyResizeOperation resize_pty, void* const context) noexcept
    -> TerminalResizeStatus {
  const auto previous = terminal.size();
  if (requested == previous) {
    return TerminalResizeStatus::unchanged;
  }
  if (resize_pty == nullptr) {
    return TerminalResizeStatus::rejected;
  }
  if (!resize_pty(context, requested)) {
    return TerminalResizeStatus::rejected;
  }
  if (terminal.resize(requested).has_value()) {
    return TerminalResizeStatus::applied;
  }
  return resize_pty(context, previous) ? TerminalResizeStatus::rolled_back
                                       : TerminalResizeStatus::consistency_lost;
}

} // namespace frame::core
