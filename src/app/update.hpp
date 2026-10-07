#ifndef FRAME_APP_UPDATE_HPP
#define FRAME_APP_UPDATE_HPP

#include <span>

namespace frame::app {

// Reruns the portable installer that owns this executable, replacing the process. Installations
// owned by a package manager are reported with that manager's update command instead.
[[nodiscard]] auto run_update(std::span<char*> arguments) -> int;

} // namespace frame::app

#endif // FRAME_APP_UPDATE_HPP
