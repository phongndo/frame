#include "app/proc.hpp"

#include "api/json.hpp"
#include "daemon/server.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace frame::app {
namespace {

[[nodiscard]] auto read_bounded_stream(std::istream& stream) -> std::optional<std::string> {
  std::string input;
  std::array<char, std::size_t{16} * 1'024U> buffer{};
  while (input.size() <= api::json_bytes_max) {
    const auto remaining = (api::json_bytes_max + 1U) - input.size();
    const auto request = std::min(buffer.size(), remaining);
    stream.read(buffer.data(), static_cast<std::streamsize>(request));
    const auto read = static_cast<std::size_t>(stream.gcount());
    input.append(buffer.data(), read);
    if (read < request) {
      return stream.eof() && input.size() <= api::json_bytes_max ? std::optional{std::move(input)}
                                                                 : std::nullopt;
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto read_proc(const std::string_view source) -> std::optional<std::string> {
  if (source == "-") {
    return read_bounded_stream(std::cin);
  }
  std::ifstream stream(std::string(source), std::ios::binary);
  return stream ? read_bounded_stream(stream) : std::nullopt;
}

} // namespace

auto run_proc_document(const daemon::RuntimeEndpoint& endpoint, const std::string_view source)
    -> int {
  try {
    const auto document = read_proc(source);
    if (!document.has_value()) {
      constexpr std::string_view error =
          R"({"schema":"frame.proc-result/v1","ok":false,"error":{"reason":"read_failed"},"results":[]}
)";
      std::cout << error;
      return 2;
    }
    return daemon::run_proc(endpoint, *document);
  } catch (...) {
    return 1;
  }
}

} // namespace frame::app
