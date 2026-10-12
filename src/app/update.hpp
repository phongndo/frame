#ifndef FRAME_APP_UPDATE_HPP
#define FRAME_APP_UPDATE_HPP

#include <span>

namespace frame::app {

// Replaces this process with the update command of the tool that installed this executable: the
// install script shipped in its release, Homebrew, mise, or a Nix profile.
[[nodiscard]] auto run_update(std::span<char*> arguments) -> int;

} // namespace frame::app

#endif // FRAME_APP_UPDATE_HPP
