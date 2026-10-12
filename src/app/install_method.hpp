#ifndef FRAME_APP_INSTALL_METHOD_HPP
#define FRAME_APP_INSTALL_METHOD_HPP

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace frame::app {

// The owner of an installed Frame, which `frame update` asks to update it. Each alternative holds
// only what that owner's update command needs.
struct InstallScript final {
  auto operator==(const InstallScript&) const -> bool = default;
};
struct Homebrew final {
  std::string prefix; // The directory holding Cellar; its bin/brew normally owns the keg.
  std::string formula;
  auto operator==(const Homebrew&) const -> bool = default;
};
struct Mise final {
  auto operator==(const Mise&) const -> bool = default;
};
struct NixStore final {
  std::string store_path; // /nix/store/<hash>-<name>, which a profile element may reference.
  auto operator==(const NixStore&) const -> bool = default;
};
struct Unmanaged final {
  auto operator==(const Unmanaged&) const -> bool = default;
};
using InstallMethod = std::variant<InstallScript, Homebrew, Mise, NixStore, Unmanaged>;

// Classifies a canonical executable path. A valid install script receipt wins, then a Homebrew
// keg, a mise install (under MISE_DATA_DIR when set, which is empty when unset), and the Nix store.
[[nodiscard]] auto classify_installation(std::string_view executable,
                                         std::string_view mise_data_dir, bool receipt)
    -> InstallMethod;

// Names the `nix profile list --json` element whose storePaths contain store_path. Profiles whose
// elements are not a name-keyed object (before version 3) never match.
[[nodiscard]] auto nix_profile_element(std::string_view profile_json, std::string_view store_path)
    -> std::optional<std::string>;

} // namespace frame::app

#endif // FRAME_APP_INSTALL_METHOD_HPP
