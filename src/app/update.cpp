#include "app/update.hpp"

#include "platform/io.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace frame::app {
namespace {

constexpr std::string_view install_command =
    "curl -fsSL https://raw.githubusercontent.com/phongndo/frame/main/scripts/install.sh | sh";

// scripts/install.sh writes this receipt into each release it installs.
struct Installation final {
  std::string release;
  std::string install_dir;
  std::string data_dir;
};

[[nodiscard]] auto read_installation(const std::string& path) -> std::optional<Installation> {
  std::ifstream stream(path);
  Installation installation;
  for (std::string line; std::getline(stream, line);) {
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      return std::nullopt;
    }
    const std::string_view key(line.data(), separator);
    auto value = line.substr(separator + 1U);
    if (key == "release") {
      installation.release = std::move(value);
    } else if (key == "install_dir") {
      installation.install_dir = std::move(value);
    } else if (key == "data_dir") {
      installation.data_dir = std::move(value);
    }
  }
  if (!stream.eof() || installation.release.empty() || !installation.install_dir.starts_with('/') ||
      !installation.data_dir.starts_with('/')) {
    return std::nullopt;
  }
  return installation;
}

[[nodiscard]] auto manager_guidance(const std::string_view executable) -> std::string {
  if (executable.contains("/Cellar/")) {
    return "Homebrew manages this installation. Update it with:\n  brew upgrade frame\n";
  }
  if (executable.starts_with("/nix/store/")) {
    return "Nix manages this installation. Update the flake input or profile that provides it.\n";
  }
  if (executable.contains("/mise/installs/")) {
    return "mise manages this installation. Update it with:\n  mise upgrade "
           "github:phongndo/frame\n";
  }
  return "Update it the way it was installed, or switch to the portable installer:\n  " +
         std::string(install_command) + "\n";
}

[[nodiscard]] auto report(const std::string_view message) noexcept -> int {
  static_cast<void>(std::fwrite(message.data(), 1, message.size(), stderr));
  return 1;
}

} // namespace

auto run_update(const std::span<char*> arguments) -> int {
  std::string_view version = "latest";
  if (arguments.size() == 2U && std::string_view(arguments.front()) == "--version") {
    version = arguments.back();
  } else if (!arguments.empty()) {
    static_cast<void>(
        report("invalid frame update arguments\nUsage: frame update [--version VERSION]\n"));
    return 2;
  }

  std::array<char, 4096> buffer{};
  const auto size = platform::executable_path(buffer);
  const std::string executable(buffer.data(), size);
  const auto bin = executable.rfind('/');
  const auto prefix =
      bin == std::string::npos || bin == 0U ? std::string::npos : executable.rfind('/', bin - 1U);
  if (size == 0 || prefix == std::string::npos) {
    return report("frame update: could not locate the running executable\n");
  }
  const auto share = executable.substr(0, prefix) + "/share/frame";
  const auto receipt = share + "/installation";
  if (::access(receipt.c_str(), F_OK) != 0) {
    return report("frame update: " + executable +
                  " was not installed by Frame's portable installer.\n" +
                  manager_guidance(executable));
  }
  const auto installation = read_installation(receipt);
  const auto installer = share + "/install.sh";
  if (!installation.has_value() || ::access(installer.c_str(), R_OK) != 0) {
    return report("frame update: this installation is incomplete. Reinstall with:\n  " +
                  std::string(install_command) + "\n");
  }

  if (::setenv("FRAME_VERSION", std::string(version).c_str(), 1) != 0 ||
      ::setenv("FRAME_INSTALL_DIR", installation->install_dir.c_str(), 1) != 0 ||
      ::setenv("FRAME_DATA_DIR", installation->data_dir.c_str(), 1) != 0 ||
      ::setenv("FRAME_CURRENT_RELEASE", installation->release.c_str(), 1) != 0) {
    return report("frame update: could not prepare the installer environment\n");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): execl is the POSIX interface.
  ::execl("/bin/sh", "sh", installer.c_str(), static_cast<char*>(nullptr));
  return report(std::string("frame update: could not run the installer: ") + std::strerror(errno) +
                "\n");
}

} // namespace frame::app
