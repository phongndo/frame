#include "api/command.hpp"
#include "api/json.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// libFuzzer owns this ABI name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
  if (data == nullptr || size > frame::api::json_bytes_max) {
    return 0;
  }
  // JSON is text but may contain arbitrary bytes; string_view retains embedded NULs for the parser.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  auto parsed = frame::api::parse_json(input);
  if (!parsed.value.has_value()) {
    return 0;
  }

  std::string encoded;
  if (!frame::api::append_json_value(encoded, *parsed.value)) {
    return 0;
  }
  const auto round_trip = frame::api::parse_json(encoded);
  if (!round_trip.value.has_value()) {
    __builtin_trap();
  }
  std::string canonical;
  if (!frame::api::append_json_value(canonical, *round_trip.value) || canonical != encoded) {
    __builtin_trap();
  }
  const bool command_decoded = frame::api::decode_command(*parsed.value).command.has_value();
  const bool canonical_command_decoded =
      frame::api::decode_command(*round_trip.value).command.has_value();
  const bool subscription_decoded =
      frame::api::decode_event_subscription(*parsed.value).subscription.has_value();
  const bool canonical_subscription_decoded =
      frame::api::decode_event_subscription(*round_trip.value).subscription.has_value();
  if (command_decoded != canonical_command_decoded ||
      subscription_decoded != canonical_subscription_decoded) {
    __builtin_trap();
  }
  return 0;
}
