#include "frame/assert.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <iterator>
#include <source_location>
#include <string_view>

#include <unistd.h>

namespace frame {
namespace {

void write_stderr(const std::string_view fragment) noexcept {
  static_cast<void>(::write(STDERR_FILENO, fragment.data(), fragment.size()));
}

} // namespace

[[noreturn]] void assertion_failed(const char* expression,
                                   const std::source_location location) noexcept {
  std::array<char, 16> line_buffer{};
  const auto line_result = std::to_chars(line_buffer.begin(), line_buffer.end(), location.line());
  const auto line_size =
      static_cast<std::size_t>(std::distance(line_buffer.begin(), line_result.ptr));

  write_stderr("frame invariant failed: ");
  write_stderr(expression);
  write_stderr(" (");
  write_stderr(location.file_name());
  write_stderr(":");
  write_stderr(std::string_view(line_buffer.data(), line_size));
  write_stderr(" in ");
  write_stderr(location.function_name());
  write_stderr(")\n");
  std::abort();
}

} // namespace frame
