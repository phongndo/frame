#ifndef FRAME_ASSERT_HPP
#define FRAME_ASSERT_HPP

#include <source_location>

namespace frame {

[[noreturn]] void
assertion_failed(const char* expression,
                 std::source_location location = std::source_location::current()) noexcept;

} // namespace frame

#define FRAME_ASSERT(expression)                                                                   \
  (static_cast<bool>(expression)                                                                   \
       ? static_cast<void>(0)                                                                      \
       : ::frame::assertion_failed(#expression, std::source_location::current()))

#endif // FRAME_ASSERT_HPP
