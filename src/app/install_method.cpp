#include "app/install_method.hpp"

#include "api/json.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace frame::app {
namespace {

// <prefix>/Cellar/<formula>/<version>/...
[[nodiscard]] auto homebrew_keg(const std::string_view path) -> std::optional<Homebrew> {
  constexpr std::string_view cellar = "/Cellar/";
  for (auto at = path.find(cellar); at != std::string_view::npos; at = path.find(cellar, at + 1U)) {
    const auto formula = at + cellar.size();
    const auto version = path.find('/', formula);
    if (version == std::string_view::npos || version == formula) {
      continue;
    }
    const auto contents = path.find('/', version + 1U);
    if (contents != std::string_view::npos && contents > version + 1U &&
        contents + 1U < path.size()) {
      return Homebrew{.prefix = std::string(path.substr(0, at)),
                      .formula = std::string(path.substr(formula, version - formula))};
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto mise_install(const std::string_view path, std::string_view data_dir) -> bool {
  if (data_dir.empty()) {
    return path.contains("/mise/installs/");
  }
  while (data_dir.ends_with('/')) {
    data_dir.remove_suffix(1);
  }
  return path.starts_with(data_dir) && path.substr(data_dir.size()).starts_with("/installs/");
}

} // namespace

auto classify_installation(const std::string_view executable, const std::string_view mise_data_dir,
                           const bool receipt) -> InstallMethod {
  if (receipt) {
    return InstallScript{};
  }
  if (auto keg = homebrew_keg(executable); keg.has_value()) {
    return std::move(*keg);
  }
  if (mise_install(executable, mise_data_dir)) {
    return Mise{};
  }
  constexpr std::string_view store = "/nix/store/";
  if (executable.starts_with(store) && executable.size() > store.size() &&
      !executable.substr(store.size()).starts_with('/')) {
    return NixStore{.store_path =
                        std::string(executable.substr(0, executable.find('/', store.size())))};
  }
  return Unmanaged{};
}

auto nix_profile_element(const std::string_view profile_json, const std::string_view store_path)
    -> std::optional<std::string> {
  const auto profile = api::parse_json(profile_json);
  const auto* const elements =
      profile.value.has_value() ? api::json_member(*profile.value, "elements") : nullptr;
  if (elements == nullptr || elements->kind != api::JsonKind::object) {
    return std::nullopt;
  }
  const auto owns = [store_path](const api::JsonMember& element) {
    const auto* const paths = api::json_member(element.value, "storePaths");
    return paths != nullptr && paths->kind == api::JsonKind::array &&
           std::ranges::any_of(paths->array, [store_path](const api::JsonValue& path) {
             return path.kind == api::JsonKind::string && path.string == store_path;
           });
  };
  const auto found = std::ranges::find_if(elements->object, owns);
  return found == elements->object.end() ? std::nullopt : std::optional{found->key};
}

} // namespace frame::app
