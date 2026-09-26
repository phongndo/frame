#ifndef LEMMA_CONFIG_CONFIG_HPP
#define LEMMA_CONFIG_CONFIG_HPP

#include "api/json.hpp"
#include "input/input_router.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lemma::config {

inline constexpr std::string_view configuration_schema = "lemma.config/v1";
inline constexpr std::size_t configuration_document_bytes_max = std::size_t{64} * 1'024U;
inline constexpr std::size_t configuration_path_bytes_max = 4'096;
inline constexpr std::size_t default_program_bytes_max = 4'096;
inline constexpr std::size_t default_program_arguments_max = 64;

struct TerminalConfiguration final {
  std::optional<std::size_t> scrollback_lines;
  bool clipboard_read{false};
  bool clipboard_write{false};
};

// Attention and context forwarded to the attached client's outer terminal. See
// docs/usage.md#attention-and-directory and #window-title.
struct OuterPresentation final {
  bool title{true};
  bool bell{true};
  bool notifications{true};
  // Off by default: terminals without OSC 9;4 can show each report as an OSC 9 notification.
  bool progress{false};
  bool cwd{true};
};

struct UiConfiguration final {
  bool status_line{true};
  OuterPresentation outer;
};

struct LaunchConfiguration final {
  std::string default_cwd;
  std::vector<std::string> default_program;
};

struct HistoryConfiguration final {
  // Empty disables persistence. A configured path is absolute so daemon startup does not inherit
  // an implicit working-directory dependency.
  std::string file;
};

inline constexpr std::size_t extensions_max = 8;
struct ExtensionConfiguration final {
  std::string name;
  std::vector<std::string> argv;
};

struct Configuration final {
  input::InputMapConfiguration input;
  TerminalConfiguration terminal;
  UiConfiguration ui;
  LaunchConfiguration launch;
  HistoryConfiguration history;
  std::vector<ExtensionConfiguration> extensions;
};

enum class Error : std::uint8_t {
  invalid_document,
  invalid_schema,
  invalid_field,
  invalid_context,
  invalid_key,
  invalid_command,
  capacity,
  input_map,
};

struct DecodeFailure final {
  Error error{Error::invalid_document};
  std::string_view field;
};

struct DecodeResult final {
  std::optional<Configuration> configuration;
  DecodeFailure failure;
};

class Generation final {
public:
  Generation(input::CompiledInputMap&& input_map, std::optional<std::size_t> scrollback_lines,
             bool status_line, std::string&& default_cwd, std::vector<std::byte>&& default_program,
             std::string&& history_file, std::vector<ExtensionConfiguration> extensions = {},
             bool clipboard_read = false, bool clipboard_write = false,
             OuterPresentation outer = {}) noexcept
      : input_map_(std::move(input_map)), scrollback_lines_(scrollback_lines),
        default_cwd_(std::move(default_cwd)), default_program_(std::move(default_program)),
        history_file_(std::move(history_file)), extensions_(std::move(extensions)),
        status_line_(status_line), clipboard_read_(clipboard_read),
        clipboard_write_(clipboard_write), outer_(outer) {}
  Generation(const Generation&) = delete;
  auto operator=(const Generation&) -> Generation& = delete;
  Generation(Generation&&) noexcept = default;
  auto operator=(Generation&&) noexcept -> Generation& = default;
  ~Generation() = default;

  [[nodiscard]] auto input_map() const noexcept -> const input::CompiledInputMap& {
    return input_map_;
  }
  [[nodiscard]] auto scrollback_lines() const noexcept -> std::optional<std::size_t> {
    return scrollback_lines_;
  }
  [[nodiscard]] auto default_cwd() const noexcept -> std::string_view { return default_cwd_; }
  [[nodiscard]] auto default_program() const noexcept -> std::span<const std::byte> {
    return default_program_;
  }
  [[nodiscard]] auto status_line() const noexcept -> bool { return status_line_; }
  [[nodiscard]] auto history_file() const noexcept -> std::string_view { return history_file_; }
  [[nodiscard]] auto clipboard_read() const noexcept -> bool { return clipboard_read_; }
  [[nodiscard]] auto clipboard_write() const noexcept -> bool { return clipboard_write_; }
  [[nodiscard]] auto outer() const noexcept -> OuterPresentation { return outer_; }

  [[nodiscard]] auto extensions() const noexcept -> std::span<const ExtensionConfiguration> {
    return extensions_;
  }

private:
  input::CompiledInputMap input_map_;
  std::optional<std::size_t> scrollback_lines_;
  std::string default_cwd_;
  std::vector<std::byte> default_program_;
  std::string history_file_;
  std::vector<ExtensionConfiguration> extensions_;
  bool status_line_{true};
  bool clipboard_read_{false};
  bool clipboard_write_{false};
  OuterPresentation outer_;
};

[[nodiscard]] auto parse_key(std::string_view value) noexcept -> std::optional<input::InputChord>;
[[nodiscard]] auto parse_context(std::string_view value) noexcept
    -> std::optional<input::ConfiguredInputContext>;
[[nodiscard]] auto parse_command(std::string_view value) noexcept
    -> std::optional<input::InputCommand>;
[[nodiscard]] auto command_name(input::InputCommand command) noexcept -> std::string_view;
[[nodiscard]] auto context_name(input::ConfiguredInputContext context) noexcept -> std::string_view;
[[nodiscard]] auto error_name(Error error) noexcept -> std::string_view;

[[nodiscard]] auto encode(const Configuration& configuration) -> std::optional<std::string>;
[[nodiscard]] auto decode(const api::JsonValue& document) noexcept -> DecodeResult;
[[nodiscard]] auto decode(std::string_view document) -> DecodeResult;
[[nodiscard]] auto compile(const Configuration& configuration) noexcept
    -> std::expected<Generation, Error>;

} // namespace lemma::config

#endif // LEMMA_CONFIG_CONFIG_HPP
