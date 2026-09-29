#include "clipboard/transaction.hpp"
#include "frame/base64.hpp"
#include "frame/limits.hpp"
#include "frame/terminal/terminal.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <algorithm>
#include <atomic>
#include <new>

namespace frame::clipboard {
namespace {
auto valid_mime(const std::string_view mime) noexcept -> bool {
  return !mime.empty() && mime.size() <= 255U &&
         std::ranges::all_of(mime,
                             [](const unsigned char value) { return value > 32U && value < 127U; });
}
auto bytes(const std::span<const std::byte> data) noexcept -> std::string_view {
  // Borrowed byte view, never used after the owning request/packet expires.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}
auto osc52_command(const vt::ClipboardRequest& request, std::string& output) -> bool {
  if (request.list || request.contents.size() > 1 || (request.read && request.contents.empty()) ||
      (!request.contents.empty() && request.contents.front().mime != "text/plain")) {
    return false;
  }
  output = request.primary ? "\x1b]52;p;" : "\x1b]52;c;";
  if (request.read) {
    output += '?';
  } else if (!request.contents.empty()) {
    output += base64::encode(bytes(request.contents.front().data));
  }
  output += "\x1b\\";
  return true;
}
} // namespace

void Transaction::published() noexcept {
  published_ = true;
  if (!done_ && protocol_ == vt::ClipboardProtocol::osc52 && !read_) {
    status_ = vt::ClipboardStatus::success;
    done_ = true;
  }
}

auto Transaction::frame_prefix(const std::span<const std::byte> data,
                               const std::size_t capacity) noexcept -> std::size_t {
  const auto count = std::min({data.size(), capacity, std::size_t{64} * 1024U});
  for (auto end = count; end >= 2U; --end) {
    if (data.subspan(end - 2U, 1).front() == std::byte{0x1b} &&
        data.subspan(end - 1U, 1).front() == std::byte{'\\'}) {
      return end;
    }
  }
  return 0;
}
auto Transaction::abort_write(const std::span<std::byte> output) const noexcept -> std::size_t {
  if (read_ || !active()) {
    return 0;
  }
  if (protocol_ == vt::ClipboardProtocol::osc52) {
    if (output.empty()) {
      return 0;
    }
    output.front() = std::byte{0x18}; // CAN cancels the unterminated OSC; never commit its prefix.
    return 1;
  }
  // Invalid MIME metadata aborts an unfinished write with EINVAL under OSC 5522.
  // Unlike a no-MIME commit it cannot publish a truncated clipboard representation.
  constexpr std::string_view prefix = "\x1b]5522;type=wdata:id=";
  constexpr std::string_view suffix = ":mime=!;\x1b\\";
  const auto size = prefix.size() + id_.size() + suffix.size();
  if (size > output.size()) {
    return 0;
  }
  auto cursor = output.begin();
  for (const auto part : {prefix, std::string_view(id_), suffix}) {
    cursor = std::ranges::copy(std::as_bytes(std::span(part)), cursor).out;
  }
  return size;
}

// Preparation validates the complete bounded request before publishing any output.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Transaction::begin(const vt::ClipboardRequest& request, const Clock::time_point now) noexcept
    -> std::expected<std::string, vt::ClipboardStatus> {
  if (active()) {
    return std::unexpected(vt::ClipboardStatus::busy);
  }
  try {
    static std::atomic<std::uint64_t> next_id{1};
    const auto id = next_id.fetch_add(1, std::memory_order_relaxed);
    if (id == 0 || request.contents.size() > 32U) {
      return std::unexpected(vt::ClipboardStatus::io_error);
    }
    std::size_t size = 0;
    for (const auto& item : request.contents) {
      if (!valid_mime(item.mime) || item.data.size() > limits::clipboard_decoded_bytes_max - size) {
        return std::unexpected(vt::ClipboardStatus::invalid_data);
      }
      size += item.data.size();
    }
    const auto correlation = std::to_string(id);
    std::string output;
    if (request.protocol == vt::ClipboardProtocol::osc52) {
      if (!osc52_command(request, output)) {
        return std::unexpected(vt::ClipboardStatus::invalid_data);
      }
      if (request.read) {
        contents_.push_back({.mime = "text/plain", .data = {}});
      }
    } else {
      output = "\x1b]5522;type=";
      output += request.read ? "read" : "write";
      output += ":id=" + correlation + ":name=TGVtbWE=";
      if (request.primary) {
        output += ":loc=primary";
      }
      if (request.read) {
        output += ';';
        if (request.list) {
          output += base64::encode(".");
        }
        for (const auto& item : request.contents) {
          if (output.back() != ';') {
            output += ' ';
          }
          output += base64::encode(item.mime);
          requested_.emplace_back(item.mime);
        }
      }
      output += "\x1b\\";
      if (!request.read) {
        for (const auto& item : request.contents) {
          const auto mime = base64::encode(item.mime);
          // wdata's base64 payload is at most 4096 bytes; each packet is independently bounded.
          for (std::size_t offset = 0; offset < item.data.size() || offset == 0; offset += 3072U) {
            output += "\x1b]5522;type=wdata:mime=" + mime + ';';
            output += base64::encode(bytes(
                item.data.subspan(offset, std::min(std::size_t{3072}, item.data.size() - offset))));
            output += "\x1b\\";
          }
        }
        output += "\x1b]5522;type=wdata\x1b\\";
      }
    }
    id_ = correlation;
    request_id_ = request.id;
    protocol_ = request.protocol;
    primary_ = request.primary;
    read_ = request.read;
    list_ = request.list;
    deadline_ = now + std::chrono::seconds(30);
    return output;
  } catch (const std::bad_alloc&) {
    // Reject preparation atomically; no partial request is published to the outer terminal.
    reset();
    return std::unexpected(vt::ClipboardStatus::io_error);
  }
}

auto Transaction::content(const std::string_view mime) -> Content* {
  for (auto& item : contents_) {
    if (item.mime == mime) {
      return &item;
    }
  }
  if (!valid_mime(mime) || contents_.size() == views_.size()) {
    return nullptr;
  }
  contents_.push_back({.mime = std::string(mime), .data = {}});
  return &contents_.back();
}

void Transaction::consume(const std::string_view record) noexcept {
  if (!active() || done_) {
    return;
  }
  try {
    parse(record);
  } catch (const std::bad_alloc&) {
    // External clipboard transfer boundary: discard partial data and report a failed operation.
    fail(vt::ClipboardStatus::io_error);
  }
}

// OSC 52 has one uncorrelated, potentially large record. Decode bounded quartets as parts arrive.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Transaction::parse_osc52(const std::string_view part) {
  if (!read_ || part.starts_with("\x1b]5522;")) {
    return;
  }
  const std::string_view prefix = primary_ ? "\x1b]52;p;" : "\x1b]52;c;";
  const auto complete = [&](const std::size_t index) {
    if (osc52_quartet_size_ != 0 || index + 1 != part.size()) {
      fail(vt::ClipboardStatus::invalid_data);
      return;
    }
    read_complete_ = true;
    status_ = vt::ClipboardStatus::success;
    done_ = true;
  };
  for (std::size_t index = 0; index < part.size(); ++index) {
    const auto character = part.at(index);
    if (osc52_prefix_size_ < prefix.size()) {
      if (character != prefix.at(osc52_prefix_size_++)) {
        fail(vt::ClipboardStatus::invalid_data);
        return;
      }
      continue;
    }
    if (osc52_escape_) {
      if (character != '\\') {
        fail(vt::ClipboardStatus::invalid_data);
        return;
      }
      complete(index);
      return;
    }
    if (character == '\x1b') {
      osc52_escape_ = true;
      continue;
    }
    if (character == '\a') {
      complete(index);
      return;
    }
    if (osc52_padded_) {
      fail(vt::ClipboardStatus::invalid_data);
      return;
    }
    osc52_quartet_.at(osc52_quartet_size_++) = character;
    if (osc52_quartet_size_ != osc52_quartet_.size()) {
      continue;
    }
    const auto decoded = base64::decode({osc52_quartet_.data(), osc52_quartet_.size()},
                                        limits::clipboard_decoded_bytes_max - decoded_bytes_);
    if (!decoded) {
      fail(vt::ClipboardStatus::invalid_data);
      return;
    }
    contents_.front().data.append(*decoded);
    decoded_bytes_ += decoded->size();
    osc52_padded_ = osc52_quartet_.back() == '=';
    osc52_quartet_size_ = 0;
  }
}

// Correlation, phase, and size validation share one rejection boundary.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Transaction::parse(std::string_view record) {
  if (protocol_ == vt::ClipboardProtocol::osc52) {
    if (osc52_prefix_size_ == 0 && record.starts_with("\x1b]5522;")) {
      return;
    }
    parse_osc52(record);
    return;
  }
  if (!record.starts_with("\x1b]5522;")) {
    return;
  }
  record.remove_prefix(7);
  if (record.ends_with("\x1b\\")) {
    record.remove_suffix(2);
  } else if (record.ends_with('\a')) {
    record.remove_suffix(1);
  } else {
    fail(vt::ClipboardStatus::invalid_data);
    return;
  }
  const auto separator = record.find(';');
  const auto payload =
      separator == std::string_view::npos ? std::string_view{} : record.substr(separator + 1U);
  auto metadata = record.substr(0, separator);
  std::string_view type;
  std::string_view status;
  std::string_view id;
  std::string_view mime;
  unsigned seen = 0;
  while (!metadata.empty()) {
    const auto colon = metadata.find(':');
    const auto field = metadata.substr(0, colon);
    const auto equals = field.find('=');
    const auto key = field.substr(0, equals);
    if (equals == std::string_view::npos) {
      fail(vt::ClipboardStatus::invalid_data);
      return;
    }
    const auto value = field.substr(equals + 1U);
    unsigned bit = 0;
    if (key == "id") {
      id = value;
      bit = 1;
    } else if (key == "type") {
      type = value;
      bit = 2;
    } else if (key == "status") {
      status = value;
      bit = 4;
    } else if (key == "mime") {
      mime = value;
      bit = 8;
    }
    if ((seen & bit) != 0) {
      fail(vt::ClipboardStatus::invalid_data);
      return;
    }
    seen |= bit;
    if (colon == std::string_view::npos) {
      break;
    }
    metadata.remove_prefix(colon + 1U);
  }
  // Unsolicited paste events and late replies cannot acquire the current request's ownership.
  if (id != id_) {
    return;
  }
  if (type != (read_ ? "read" : "write")) {
    fail(vt::ClipboardStatus::invalid_data);
    return;
  }
  if (status == "EPERM") {
    fail(vt::ClipboardStatus::denied);
    return;
  }
  if (status == "ENOSYS") {
    fail(vt::ClipboardStatus::unsupported);
    return;
  }
  if (status == "EBUSY") {
    fail(vt::ClipboardStatus::busy);
    return;
  }
  if (status == "OK" && read_ && !started_) {
    started_ = true;
    return;
  }
  if (status == "DONE" && (!read_ || started_)) {
    if (list_) {
      std::string_view remaining = listing_;
      if (remaining.ends_with('\n')) {
        remaining.remove_suffix(1);
      }
      while (!remaining.empty()) {
        const auto space = remaining.find(' ');
        if (content(remaining.substr(0, space)) == nullptr) {
          fail(vt::ClipboardStatus::invalid_data);
          return;
        }
        if (space == std::string_view::npos) {
          break;
        }
        remaining.remove_prefix(space + 1U);
      }
    }
    fail(vt::ClipboardStatus::success);
    return;
  }
  if (status != "DATA" || !read_ || !started_) {
    fail(vt::ClipboardStatus::io_error);
    return;
  }
  const auto decoded_mime = base64::decode(mime, 255);
  const auto decoded = base64::decode(payload, 4096);
  if (!decoded_mime || !decoded ||
      decoded->size() > limits::clipboard_decoded_bytes_max - decoded_bytes_) {
    fail(vt::ClipboardStatus::invalid_data);
    return;
  }
  decoded_bytes_ += decoded->size();
  if (*decoded_mime == "." && list_) {
    if (decoded->size() > (std::size_t{32} * 256U) - listing_.size()) {
      fail(vt::ClipboardStatus::invalid_data);
      return;
    }
    listing_ += *decoded;
    return;
  }
  if (std::ranges::find(requested_, *decoded_mime) == requested_.end()) {
    fail(vt::ClipboardStatus::invalid_data);
    return;
  }
  auto* const target = content(*decoded_mime);
  if (target == nullptr) {
    fail(vt::ClipboardStatus::invalid_data);
    return;
  }
  target->data += *decoded;
}

auto Transaction::contents() noexcept -> std::span<const vt::ClipboardContent> {
  if (!done_ || status_ != vt::ClipboardStatus::success) {
    return {};
  }
  for (std::size_t i = 0; i < contents_.size(); ++i) {
    const auto& item = std::span(contents_).subspan(i, 1).front();
    std::span(views_).subspan(i, 1).front() = {.mime = item.mime,
                                               .data = std::as_bytes(std::span(item.data))};
  }
  return std::span(views_).first(contents_.size());
}
} // namespace frame::clipboard
