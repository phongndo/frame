#include "api/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace frame::api {

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto valid_utf8(const std::string_view value) noexcept -> bool {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto leading = static_cast<unsigned char>(std::span(value).subspan(offset, 1).front());
    if (leading <= 0x7fU) {
      ++offset;
      continue;
    }
    std::size_t continuation_count = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if (leading >= 0xc2U && leading <= 0xdfU) {
      continuation_count = 1;
      codepoint = leading & 0x1fU;
      minimum = 0x80U;
    } else if (leading >= 0xe0U && leading <= 0xefU) {
      continuation_count = 2;
      codepoint = leading & 0x0fU;
      minimum = 0x800U;
    } else if (leading >= 0xf0U && leading <= 0xf4U) {
      continuation_count = 3;
      codepoint = leading & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1U) {
      return false;
    }
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto byte =
          static_cast<unsigned char>(std::span(value).subspan(offset + index, 1).front());
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | (byte & 0x3fU);
    }
    if (codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
      return false;
    }
    offset += continuation_count + 1U;
  }
  return true;
}

namespace {

[[nodiscard]] auto append_utf8(std::string& output, const std::uint32_t codepoint) -> bool {
  if (codepoint <= 0x7fU) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else if (codepoint <= 0xffffU) {
    if (codepoint >= 0xd800U && codepoint <= 0xdfffU) {
      return false;
    }
    output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else if (codepoint <= 0x10ffffU) {
    output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else {
    return false;
  }
  return true;
}

class Parser final {
public:
  explicit Parser(const std::string_view input) noexcept : input_(input) {}

  [[nodiscard]] auto parse() -> std::optional<JsonValue> {
    auto result = parse_value(0);
    skip_whitespace();
    return result.has_value() && offset_ == input_.size() ? std::move(result) : std::nullopt;
  }

  [[nodiscard]] auto error_offset() const noexcept -> std::size_t { return offset_; }

private:
  void skip_whitespace() {
    while (offset_ < input_.size()) {
      const auto character = input_.substr(offset_, 1).front();
      if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
        break;
      }
      ++offset_;
    }
  }

  [[nodiscard]] auto consume(const char character) -> bool {
    skip_whitespace();
    if (offset_ >= input_.size() || input_.substr(offset_, 1).front() != character) {
      return false;
    }
    ++offset_;
    return true;
  }

  [[nodiscard]] auto consume_literal(const std::string_view literal) -> bool {
    if (literal.size() > input_.size() - offset_ ||
        input_.substr(offset_, literal.size()) != literal) {
      return false;
    }
    offset_ += literal.size();
    return true;
  }

  [[nodiscard]] auto hex_quad() -> std::optional<std::uint32_t> {
    if (4U > input_.size() - offset_) {
      return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char character : input_.substr(offset_, 4)) {
      value <<= 4U;
      if (character >= '0' && character <= '9') {
        value |= static_cast<std::uint32_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        value |= static_cast<std::uint32_t>(character - 'a' + 10);
      } else if (character >= 'A' && character <= 'F') {
        value |= static_cast<std::uint32_t>(character - 'A' + 10);
      } else {
        return std::nullopt;
      }
    }
    offset_ += 4U;
    return value;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto parse_string() -> std::optional<std::string> {
    skip_whitespace();
    if (offset_ >= input_.size() || input_.substr(offset_, 1).front() != '"') {
      return std::nullopt;
    }
    ++offset_;
    std::string output;
    while (offset_ < input_.size()) {
      const auto character = static_cast<unsigned char>(input_.substr(offset_++, 1).front());
      if (character == '"') {
        return valid_utf8(output) ? std::optional{std::move(output)} : std::nullopt;
      }
      if (character < 0x20U) {
        return std::nullopt;
      }
      if (character != '\\') {
        output.push_back(static_cast<char>(character));
        continue;
      }
      if (offset_ >= input_.size()) {
        return std::nullopt;
      }
      const auto escaped = input_.substr(offset_++, 1).front();
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        output.push_back(escaped);
        break;
      case 'b':
        output.push_back('\b');
        break;
      case 'f':
        output.push_back('\f');
        break;
      case 'n':
        output.push_back('\n');
        break;
      case 'r':
        output.push_back('\r');
        break;
      case 't':
        output.push_back('\t');
        break;
      case 'u': {
        auto codepoint = hex_quad();
        if (!codepoint.has_value()) {
          return std::nullopt;
        }
        if (*codepoint >= 0xd800U && *codepoint <= 0xdbffU) {
          if (!consume_literal("\\u")) {
            return std::nullopt;
          }
          const auto low = hex_quad();
          if (!low.has_value() || *low < 0xdc00U || *low > 0xdfffU) {
            return std::nullopt;
          }
          codepoint = 0x10000U + ((*codepoint - 0xd800U) << 10U) + (*low - 0xdc00U);
        }
        if (!append_utf8(output, *codepoint)) {
          return std::nullopt;
        }
        break;
      }
      default:
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] auto parse_number() -> std::optional<std::int64_t> {
    const auto start = offset_;
    if (offset_ < input_.size() && input_.substr(offset_, 1).front() == '-') {
      ++offset_;
    }
    const auto digits = offset_;
    while (offset_ < input_.size()) {
      const auto character = input_.substr(offset_, 1).front();
      if (character < '0' || character > '9') {
        break;
      }
      ++offset_;
    }
    if (digits == offset_ || (offset_ - digits > 1U && input_.substr(digits, 1).front() == '0')) {
      return std::nullopt;
    }
    std::int64_t value = 0;
    const auto token = input_.substr(start, offset_ - start);
    const auto parsed = std::from_chars(token.begin(), token.end(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token.end() ? std::optional{value}
                                                                 : std::nullopt;
  }

  [[nodiscard]] auto parse_array(const std::size_t depth) -> std::optional<JsonValue> {
    JsonValue result{.kind = JsonKind::array,
                     .boolean = false,
                     .number = 0,
                     .string = {},
                     .array = {},
                     .object = {}};
    if (consume(']')) {
      return result;
    }
    while (true) {
      auto value = parse_value(depth + 1U);
      if (!value.has_value()) {
        return std::nullopt;
      }
      result.array.push_back(std::move(*value));
      if (consume(']')) {
        return result;
      }
      if (!consume(',')) {
        return std::nullopt;
      }
    }
  }

  [[nodiscard]] auto parse_object(const std::size_t depth) -> std::optional<JsonValue> {
    JsonValue result{.kind = JsonKind::object,
                     .boolean = false,
                     .number = 0,
                     .string = {},
                     .array = {},
                     .object = {}};
    if (consume('}')) {
      return result;
    }
    while (true) {
      auto key = parse_string();
      if (!key.has_value() || !consume(':') ||
          std::ranges::any_of(result.object,
                              [&](const JsonMember& entry) { return entry.key == *key; })) {
        return std::nullopt;
      }
      auto value = parse_value(depth + 1U);
      if (!value.has_value()) {
        return std::nullopt;
      }
      result.object.push_back(JsonMember{.key = std::move(*key), .value = std::move(*value)});
      if (consume('}')) {
        return result;
      }
      if (!consume(',')) {
        return std::nullopt;
      }
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto parse_value(const std::size_t depth) -> std::optional<JsonValue> {
    skip_whitespace();
    if (depth > json_depth_max || offset_ >= input_.size() || ++nodes_ > json_nodes_max) {
      return std::nullopt;
    }
    const auto character = input_.substr(offset_, 1).front();
    if (character == '"') {
      auto string = parse_string();
      return string.has_value() ? std::optional{JsonValue{.kind = JsonKind::string,
                                                          .boolean = false,
                                                          .number = 0,
                                                          .string = std::move(*string),
                                                          .array = {},
                                                          .object = {}}}
                                : std::nullopt;
    }
    if (character == '[') {
      ++offset_;
      return parse_array(depth);
    }
    if (character == '{') {
      ++offset_;
      return parse_object(depth);
    }
    if (character == 't' && consume_literal("true")) {
      return JsonValue{.kind = JsonKind::boolean,
                       .boolean = true,
                       .number = 0,
                       .string = {},
                       .array = {},
                       .object = {}};
    }
    if (character == 'f' && consume_literal("false")) {
      return JsonValue{.kind = JsonKind::boolean,
                       .boolean = false,
                       .number = 0,
                       .string = {},
                       .array = {},
                       .object = {}};
    }
    if (character == 'n' && consume_literal("null")) {
      return JsonValue{};
    }
    if (character == '-' || (character >= '0' && character <= '9')) {
      const auto number = parse_number();
      return number.has_value() ? std::optional{JsonValue{.kind = JsonKind::number,
                                                          .boolean = false,
                                                          .number = *number,
                                                          .string = {},
                                                          .array = {},
                                                          .object = {}}}
                                : std::nullopt;
    }
    return std::nullopt;
  }

  std::string_view input_;
  std::size_t offset_{0};
  std::size_t nodes_{0};
};

} // namespace

auto parse_json(const std::string_view input) -> JsonParseResult {
  if (input.size() > json_bytes_max) {
    return {.value = std::nullopt, .error_offset = json_bytes_max};
  }
  Parser parser(input);
  auto value = parser.parse();
  return {.value = std::move(value), .error_offset = parser.error_offset()};
}

auto json_member(const JsonValue& object, const std::string_view key) noexcept -> const JsonValue* {
  if (object.kind != JsonKind::object) {
    return nullptr;
  }
  const auto found = std::ranges::find_if(
      object.object, [&](const JsonMember& entry) { return entry.key == key; });
  return found == object.object.end() ? nullptr : &found->value;
}

auto json_string(const JsonValue& object, const std::string_view key) noexcept
    -> std::optional<std::string_view> {
  const auto* const value = json_member(object, key);
  return value != nullptr && value->kind == JsonKind::string
             ? std::optional{std::string_view(value->string)}
             : std::nullopt;
}

auto json_boolean(const JsonValue& object, const std::string_view key) noexcept
    -> std::optional<bool> {
  const auto* const value = json_member(object, key);
  return value != nullptr && value->kind == JsonKind::boolean ? std::optional{value->boolean}
                                                              : std::nullopt;
}

auto json_unsigned(const JsonValue& object, const std::string_view key) noexcept
    -> std::optional<std::uint64_t> {
  const auto* const value = json_member(object, key);
  return value != nullptr && value->kind == JsonKind::number && value->number >= 0
             ? std::optional{static_cast<std::uint64_t>(value->number)}
             : std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto append_json_string(std::string& output, const std::string_view value,
                        const std::size_t maximum) -> bool {
  const auto append = [&](const std::string_view text) {
    if (output.size() > maximum || text.size() > maximum - output.size()) {
      return false;
    }
    output.append(text);
    return true;
  };
  if (!append("\"")) {
    return false;
  }
  constexpr std::string_view digits = "0123456789abcdef";
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    std::string_view escaped;
    switch (character) {
    case '"':
      escaped = "\\\"";
      break;
    case '\\':
      escaped = "\\\\";
      break;
    case '\b':
      escaped = "\\b";
      break;
    case '\f':
      escaped = "\\f";
      break;
    case '\n':
      escaped = "\\n";
      break;
    case '\r':
      escaped = "\\r";
      break;
    case '\t':
      escaped = "\\t";
      break;
    default:
      break;
    }
    if (!escaped.empty()) {
      if (!append(escaped)) {
        return false;
      }
    } else if (byte < 0x20U) {
      const auto high = digits.substr((byte >> 4U) & 0x0fU, 1).front();
      const auto low = digits.substr(byte & 0x0fU, 1).front();
      const std::array encoded{'\\', 'u', '0', '0', high, low};
      if (!append({encoded.data(), encoded.size()})) {
        return false;
      }
    } else if (output.size() == maximum) {
      return false;
    } else {
      output.push_back(character);
    }
  }
  return append("\"");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto append_json_value(std::string& output, const JsonValue& value, const std::size_t maximum)
    -> bool {
  const auto append = [&](const std::string_view text) {
    if (output.size() > maximum || text.size() > maximum - output.size()) {
      return false;
    }
    output.append(text);
    return true;
  };
  switch (value.kind) {
  case JsonKind::null:
    return append("null");
  case JsonKind::boolean:
    return append(value.boolean ? "true" : "false");
  case JsonKind::number: {
    std::array<char, 32> encoded{};
    const auto result = std::to_chars(encoded.begin(), encoded.end(), value.number);
    return result.ec == std::errc{} &&
           append({encoded.data(), static_cast<std::size_t>(result.ptr - encoded.data())});
  }
  case JsonKind::string:
    return append_json_string(output, value.string, maximum);
  case JsonKind::array:
    if (!append("[")) {
      return false;
    }
    for (std::size_t index = 0; index < value.array.size(); ++index) {
      const auto& entry = std::span(value.array).subspan(index, 1).front();
      if ((index > 0 && !append(",")) || !append_json_value(output, entry, maximum)) {
        return false;
      }
    }
    return append("]");
  case JsonKind::object:
    if (!append("{")) {
      return false;
    }
    for (std::size_t index = 0; index < value.object.size(); ++index) {
      const auto& entry = std::span(value.object).subspan(index, 1).front();
      if ((index > 0 && !append(",")) || !append_json_string(output, entry.key, maximum) ||
          !append(":") || !append_json_value(output, entry.value, maximum)) {
        return false;
      }
    }
    return append("}");
  }
  return false;
}

} // namespace frame::api
