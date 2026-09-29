#ifndef FRAME_API_JSON_HPP
#define FRAME_API_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frame::api {

inline constexpr std::size_t json_bytes_max = std::size_t{1} * 1'024U * 1'024U;
inline constexpr std::size_t json_nodes_max = 4'096;
inline constexpr std::size_t json_depth_max = 32;

enum class JsonKind : std::uint8_t {
  null,
  boolean,
  number,
  string,
  array,
  object,
};

struct JsonMember;

struct JsonValue final {
  JsonKind kind{JsonKind::null};
  bool boolean{false};
  std::int64_t number{0};
  std::string string;
  std::vector<JsonValue> array;
  std::vector<JsonMember> object;
};

struct JsonMember final {
  std::string key;
  JsonValue value;
};

struct JsonParseResult final {
  std::optional<JsonValue> value;
  std::size_t error_offset{0};
};

[[nodiscard]] auto valid_utf8(std::string_view value) noexcept -> bool;
[[nodiscard]] auto parse_json(std::string_view input) -> JsonParseResult;
[[nodiscard]] auto json_member(const JsonValue& object, std::string_view key) noexcept
    -> const JsonValue*;
[[nodiscard]] auto json_string(const JsonValue& object, std::string_view key) noexcept
    -> std::optional<std::string_view>;
[[nodiscard]] auto json_boolean(const JsonValue& object, std::string_view key) noexcept
    -> std::optional<bool>;
[[nodiscard]] auto json_unsigned(const JsonValue& object, std::string_view key) noexcept
    -> std::optional<std::uint64_t>;
[[nodiscard]] auto append_json_string(std::string& output, std::string_view value,
                                      std::size_t maximum = json_bytes_max) -> bool;
[[nodiscard]] auto append_json_value(std::string& output, const JsonValue& value,
                                     std::size_t maximum = json_bytes_max) -> bool;

} // namespace frame::api

#endif // FRAME_API_JSON_HPP
