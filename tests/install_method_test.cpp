#include "app/install_method.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace frame::app {
namespace {

// `nix profile list --json` from Nix 2.35 for a profile holding `nix profile add
// github:phongndo/frame` and `nix profile add nixpkgs#hello`.
constexpr std::string_view profile_v3 =
    R"({"elements":{"frame":{"active":true,"attrPath":"packages.x86_64-linux.default",)"
    R"("originalUrl":"github:phongndo/frame","outputs":null,"priority":5,"storePaths":)"
    R"(["/nix/store/bv83hbzbwiqz2d8kr8r2051dvkpi63gx-frame-0.1.0"],)"
    R"("url":"github:phongndo/frame/914ffabaae94da679f6321331834877999dfd828"},)"
    R"("hello":{"active":true,"attrPath":"legacyPackages.x86_64-linux.hello",)"
    R"("originalUrl":"flake:nixpkgs","outputs":null,"priority":5,"storePaths":)"
    R"(["/nix/store/2y3s86m1yj9xnypd44vivfz3mwybzlki-hello-2.12.3"],)"
    R"("url":"path:/nix/store/hbd89dn4si5jb3imhymh1bapmf7i5jc4-source"}},"version":3})";

constexpr std::string_view frame_store_path =
    "/nix/store/bv83hbzbwiqz2d8kr8r2051dvkpi63gx-frame-0.1.0";

[[nodiscard]] auto classify(const std::string_view executable,
                            const std::string_view mise_data_dir = {}, const bool receipt = false)
    -> InstallMethod {
  return classify_installation(executable, mise_data_dir, receipt);
}

TEST(InstallMethodTest, HomebrewKegsNameTheirPrefixAndFormula) {
  EXPECT_EQ(classify("/opt/homebrew/Cellar/frame/1.2.3/bin/frame"),
            InstallMethod(Homebrew{.prefix = "/opt/homebrew", .formula = "frame"}));
  EXPECT_EQ(classify("/home/linuxbrew/.linuxbrew/Cellar/frame/1.2.3/bin/frame"),
            InstallMethod(Homebrew{.prefix = "/home/linuxbrew/.linuxbrew", .formula = "frame"}));
  EXPECT_EQ(classify("/usr/local/Cellar/frame-head/HEAD-914ffab/bin/frame"),
            InstallMethod(Homebrew{.prefix = "/usr/local", .formula = "frame-head"}));
  EXPECT_EQ(classify("/opt/homebrew/Cellar/frame/1.2.3"), InstallMethod(Unmanaged{}));
}

TEST(InstallMethodTest, MiseInstallsFollowMiseDataDir) {
  constexpr std::string_view default_install =
      "/home/me/.local/share/mise/installs/github-phongndo-frame/1.2.3/bin/frame";
  constexpr std::string_view custom_install =
      "/opt/mise/installs/github-phongndo-frame/1.2.3/bin/frame";
  EXPECT_EQ(classify(default_install), InstallMethod(Mise{}));
  EXPECT_EQ(classify(custom_install, "/opt/mise"), InstallMethod(Mise{}));
  EXPECT_EQ(classify(custom_install, "/opt/mise/"), InstallMethod(Mise{}));
  // That mise would upgrade its own data directory, not this executable's.
  EXPECT_EQ(classify(default_install, "/opt/mise"), InstallMethod(Unmanaged{}));
  EXPECT_EQ(classify("/opt/mise-old/installs/frame/1.2.3/bin/frame", "/opt/mise"),
            InstallMethod(Unmanaged{}));
}

TEST(InstallMethodTest, NixStoreExecutablesNameTheirStorePath) {
  EXPECT_EQ(classify(std::string(frame_store_path) + "/bin/frame"),
            InstallMethod(NixStore{.store_path = std::string(frame_store_path)}));
  EXPECT_EQ(classify("/nix/store/"), InstallMethod(Unmanaged{}));
}

TEST(InstallMethodTest, SourceBuildsAreUnmanaged) {
  EXPECT_EQ(classify("/home/me/src/frame/build/frame"), InstallMethod(Unmanaged{}));
  EXPECT_EQ(classify("/home/me/src/frame/build/release/frame"), InstallMethod(Unmanaged{}));
}

TEST(InstallMethodTest, InstallScriptReceiptWins) {
  constexpr std::string_view release =
      "/home/me/.local/share/frame/releases/v1.2.3-x86_64-unknown-linux-gnu.Ab12Cd/bin/frame";
  EXPECT_EQ(classify(release, {}, true), InstallMethod(InstallScript{}));
  EXPECT_EQ(classify(release), InstallMethod(Unmanaged{}));
  EXPECT_EQ(classify("/opt/homebrew/Cellar/frame/1.2.3/bin/frame", {}, true),
            InstallMethod(InstallScript{}));
  EXPECT_EQ(classify(std::string(frame_store_path) + "/bin/frame", {}, true),
            InstallMethod(InstallScript{}));
}

TEST(InstallMethodTest, NixProfileElementOwnsTheStorePath) {
  EXPECT_EQ(nix_profile_element(profile_v3, frame_store_path), std::optional<std::string>("frame"));
  EXPECT_EQ(
      nix_profile_element(profile_v3, "/nix/store/2y3s86m1yj9xnypd44vivfz3mwybzlki-hello-2.12.3"),
      std::optional<std::string>("hello"));
}

TEST(InstallMethodTest, NixProfileWithoutTheStorePathHasNoElement) {
  EXPECT_EQ(
      nix_profile_element(profile_v3, "/nix/store/00000000000000000000000000000000-frame-0.1.0"),
      std::nullopt);
  EXPECT_EQ(nix_profile_element(R"({"elements":{},"version":3})", frame_store_path), std::nullopt);
}

TEST(InstallMethodTest, UnreadableNixProfilesHaveNoElement) {
  EXPECT_EQ(nix_profile_element("", frame_store_path), std::nullopt);
  EXPECT_EQ(nix_profile_element(profile_v3.substr(0, profile_v3.size() - 1U), frame_store_path),
            std::nullopt);
  EXPECT_EQ(nix_profile_element("error: profile is incompatible", frame_store_path), std::nullopt);
  // Before version 3, elements were an unnamed array.
  EXPECT_EQ(nix_profile_element(
                R"({"elements":[{"storePaths":["/nix/store/bv83hbzbwiqz2d8kr8r2051dvkpi63gx-)"
                R"(frame-0.1.0"]}],"version":2})",
                frame_store_path),
            std::nullopt);
}

} // namespace
} // namespace frame::app
