#include "app/update.hpp"

#include "api/json.hpp"
#include "app/install_method.hpp"
#include "platform/io.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib> // IWYU pragma: keep — POSIX setenv.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace frame::app {
namespace {

constexpr std::string_view install_command =
    "curl -fsSL https://raw.githubusercontent.com/phongndo/frame/main/scripts/install.sh | sh";
constexpr std::string_view mise_tool = "github:phongndo/frame";

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

[[nodiscard]] auto report(const std::string_view message) noexcept -> int {
  static_cast<void>(std::fwrite(message.data(), 1, message.size(), stderr));
  return 1;
}

[[nodiscard]] auto command_line(const std::span<const std::string> command) -> std::string {
  std::string line;
  for (const auto& argument : command) {
    line += line.empty() ? "" : " ";
    line += argument;
  }
  return line;
}

[[nodiscard]] auto argument_vector(std::span<std::string> command) -> std::vector<char*> {
  std::vector<char*> arguments;
  arguments.reserve(command.size() + 1U);
  for (auto& argument : command) {
    arguments.push_back(argument.data());
  }
  arguments.push_back(nullptr);
  return arguments;
}

// Hands this process to the package manager that owns the installation.
[[nodiscard]] auto replace_process(std::vector<std::string> command) -> int {
  const auto line = command_line(command);
  static_cast<void>(report("Updating with: " + line + "\n"));
  auto arguments = argument_vector(command);
  ::execvp(arguments.front(), arguments.data());
  return report("frame update: could not run " + command.front() + ": " + std::strerror(errno) +
                ". Run it yourself:\n  " + line + "\n");
}

// Reads a descriptor to its end, failing past the JSON input limit.
[[nodiscard]] auto read_to_end(const int descriptor) -> std::optional<std::string> {
  std::string text;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto received = ::read(descriptor, buffer.data(), buffer.size());
    if (received == 0) {
      return text;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0 || static_cast<std::size_t>(received) > api::json_bytes_max - text.size()) {
      return std::nullopt;
    }
    text.append(buffer.data(), static_cast<std::size_t>(received));
  }
}

[[nodiscard]] auto exited_successfully(const pid_t child) noexcept -> bool {
  int status = 0;
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Returns the standard output of a successful PATH command, with no shell, input, or diagnostics.
[[nodiscard]] auto captured_output(std::vector<std::string> command) -> std::optional<std::string> {
  auto arguments = argument_vector(command);
  std::array<int, 2> output{-1, -1};
  if (::pipe(output.data()) != 0) {
    return std::nullopt;
  }
  // POSIX descriptor calls have a variadic ABI.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
  static_cast<void>(::fcntl(output.front(), F_SETFD, FD_CLOEXEC));
  static_cast<void>(::fcntl(output.back(), F_SETFD, FD_CLOEXEC));
  const auto child = ::fork();
  if (child == 0) {
    const auto null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null >= 0 && ::dup2(null, STDIN_FILENO) >= 0 && ::dup2(output.back(), STDOUT_FILENO) >= 0 &&
        ::dup2(null, STDERR_FILENO) >= 0) {
      ::execvp(arguments.front(), arguments.data());
    }
    ::_exit(127);
  }
  // NOLINTEND(cppcoreguidelines-pro-type-vararg)
  platform::close_descriptor(output.back());
  if (child < 0) {
    platform::close_descriptor(output.front());
    return std::nullopt;
  }
  auto text = read_to_end(output.front());
  // Closing early ends an oversized writer with SIGPIPE.
  platform::close_descriptor(output.front());
  return exited_successfully(child) ? std::move(text) : std::nullopt;
}

// A version without its tag's leading v, as mise spells it.
[[nodiscard]] auto bare_version(std::string_view version) -> std::string {
  if (version.size() > 1U && version.front() == 'v' && version.at(1) >= '0' &&
      version.at(1) <= '9') {
    version.remove_prefix(1);
  }
  return std::string(version);
}

[[nodiscard]] auto run_installer(const std::string& share, const Installation& installation,
                                 const std::optional<std::string_view> version) -> int {
  const auto installer = share + "/install.sh";
  if (::access(installer.c_str(), R_OK) != 0) {
    return report("frame update: this installation is incomplete. Reinstall with:\n  " +
                  std::string(install_command) + "\n");
  }
  if (::setenv("FRAME_VERSION", std::string(version.value_or("latest")).c_str(), 1) != 0 ||
      ::setenv("FRAME_INSTALL_DIR", installation.install_dir.c_str(), 1) != 0 ||
      ::setenv("FRAME_DATA_DIR", installation.data_dir.c_str(), 1) != 0 ||
      ::setenv("FRAME_CURRENT_RELEASE", installation.release.c_str(), 1) != 0) {
    return report("frame update: could not prepare the installer environment\n");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): execl is the POSIX interface.
  ::execl("/bin/sh", "sh", installer.c_str(), static_cast<char*>(nullptr));
  return report(std::string("frame update: could not run the installer: ") + std::strerror(errno) +
                "\n");
}

[[nodiscard]] auto run_homebrew(const Homebrew& homebrew,
                                const std::optional<std::string_view> version) -> int {
  if (version.has_value()) {
    return report("frame update: Homebrew installs only the latest release; run `frame update` "
                  "without --version.\n");
  }
  auto brew = homebrew.prefix + "/bin/brew";
  if (::access(brew.c_str(), X_OK) != 0) {
    brew = "brew";
  }
  return replace_process({std::move(brew), "upgrade", homebrew.formula});
}

[[nodiscard]] auto run_mise(const std::optional<std::string_view> version) -> int {
  if (version.has_value()) {
    return report("frame update: pin a version with mise instead:\n  mise use -g " +
                  std::string(mise_tool) + "@" + bare_version(*version) + "\n");
  }
  return replace_process({"mise", "upgrade", std::string(mise_tool)});
}

[[nodiscard]] auto run_nix(const NixStore& nix, const std::optional<std::string_view> version)
    -> int {
  const auto profile = captured_output({"nix", "profile", "list", "--json"});
  // `nix profile upgrade` with an unknown name succeeds without doing anything, so never guess.
  const auto element =
      profile.has_value() ? nix_profile_element(*profile, nix.store_path) : std::nullopt;
  if (!element.has_value()) {
    return report("frame update: this Frame comes from a Nix configuration or `nix run`; update "
                  "its flake input (nix flake update frame) and rebuild.\n");
  }
  if (version.has_value()) {
    return report("frame update: pin a version with Nix instead:\n  nix profile remove " +
                  *element + " && nix profile add github:phongndo/frame/v" +
                  bare_version(*version) + "\n");
  }
  return replace_process({"nix", "profile", "upgrade", *element});
}

// MISE_DATA_DIR may reach the canonical executable path through a symlink.
[[nodiscard]] auto mise_data_dir() -> std::string {
  const char* const configured = std::getenv("MISE_DATA_DIR");
  if (configured == nullptr || *configured == '\0') {
    return {};
  }
  std::error_code error;
  auto canonical = std::filesystem::canonical(configured, error);
  return error ? std::string(configured) : std::move(canonical).string();
}

} // namespace

auto run_update(const std::span<char*> arguments) -> int {
  std::optional<std::string_view> version;
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
  const auto installation = read_installation(share + "/installation");
  const auto method = classify_installation(executable, mise_data_dir(), installation.has_value());

  if (std::holds_alternative<InstallScript>(method) && installation.has_value()) {
    return run_installer(share, *installation, version);
  }
  if (const auto* const homebrew = std::get_if<Homebrew>(&method); homebrew != nullptr) {
    return run_homebrew(*homebrew, version);
  }
  if (std::holds_alternative<Mise>(method)) {
    return run_mise(version);
  }
  if (const auto* const nix = std::get_if<NixStore>(&method); nix != nullptr) {
    return run_nix(*nix, version);
  }
  return report("frame update: this Frame was not installed by Homebrew, the install script, mise, "
                "or Nix. Rebuild it from source, or reinstall it with one of those:\n  "
                "https://github.com/phongndo/frame/blob/main/docs/installation.md\n");
}

} // namespace frame::app
