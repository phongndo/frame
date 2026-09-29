#ifndef FRAME_EXTENSION_EXTERNAL_COMMAND_HPP
#define FRAME_EXTENSION_EXTERNAL_COMMAND_HPP

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace frame::extension {

// Used only inside the isolated host. Owns one child and a bounded diagnostic pipe; neither
// launch, cancellation nor reaping introduces a synchronous dependency in the daemon.
class ExternalCommand final {
public:
  ExternalCommand() noexcept = default;
  ExternalCommand(const ExternalCommand&) = delete;
  auto operator=(const ExternalCommand&) -> ExternalCommand& = delete;
  ExternalCommand(ExternalCommand&& other) noexcept;
  auto operator=(ExternalCommand&& other) noexcept -> ExternalCommand&;
  ~ExternalCommand();

  [[nodiscard]] auto start(std::span<const std::string> arguments, std::string_view context)
      -> bool;
  [[nodiscard]] auto descriptor() const noexcept -> int { return descriptor_; }
  [[nodiscard]] auto reaping() const noexcept -> bool { return process_ > 0 && descriptor_ < 0; }
  void cancel() const noexcept;
  [[nodiscard]] auto service(short events) -> std::optional<std::string>;

private:
  void reset() noexcept;
  void read_output();
  [[nodiscard]] auto completion(int status, bool observed) -> std::string;
  int process_{-1};
  int descriptor_{-1};
  std::size_t output_bytes_{0};
  std::string diagnostic_;
};

} // namespace frame::extension

#endif // FRAME_EXTENSION_EXTERNAL_COMMAND_HPP
