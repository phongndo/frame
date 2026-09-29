#include "terminal/terminal_impl.hpp"

#include "frame/limits.hpp"
#include "frame/terminal/terminal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace frame::vt {
namespace {
constexpr std::size_t representations_max = 32;

[[nodiscard]] auto text(const GhosttyString value) noexcept -> std::string_view {
  // Ghostty's binary-safe strings use unsigned bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {reinterpret_cast<const char*>(value.ptr), value.len};
}

[[nodiscard]] auto native(const std::span<const std::byte> value) noexcept -> GhosttyString {
  // Ghostty's binary-safe strings use unsigned bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return {.ptr = reinterpret_cast<const std::uint8_t*>(value.data()), .len = value.size()};
}

void reject(const GhosttyClipboardRead* request, const GhosttyClipboardReadResult result) noexcept {
  const GhosttyClipboardReadReply reply{.size = sizeof(GhosttyClipboardReadReply),
                                        .result = result,
                                        .contents = nullptr,
                                        .contents_len = 0,
                                        .available = nullptr,
                                        .available_len = 0,
                                        .remember = false};
  request->reply(request, &reply);
}
void reject(const GhosttyClipboardWrite* request,
            const GhosttyClipboardWriteResult result) noexcept {
  const GhosttyClipboardWriteReply reply{
      .size = sizeof(GhosttyClipboardWriteReply), .result = result, .remember = false};
  request->reply(request, &reply);
}
} // namespace

Terminal::Impl::ClipboardPending::~ClipboardPending() {
  ghostty_clipboard_read_free(read);
  ghostty_clipboard_write_free(write);
}

void Terminal::Impl::clipboard_read([[maybe_unused]] GhosttyTerminal terminal_handle,
                                    void* userdata, const GhosttyClipboardRead* request) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  if (!impl.clipboard_read_allowed) {
    reject(request, GHOSTTY_CLIPBOARD_READ_RESULT_DENIED);
    return;
  }
  if (impl.clipboard != nullptr || request->mimes_len > representations_max) {
    reject(request, GHOSTTY_CLIPBOARD_READ_RESULT_BUSY);
    return;
  }
  try {
    auto pending = std::make_unique<ClipboardPending>();
    pending->contents.reserve(request->mimes_len);
    if (ghostty_clipboard_read_defer(request, &pending->read) != GHOSTTY_SUCCESS) {
      reject(request, GHOSTTY_CLIPBOARD_READ_RESULT_IO_ERROR);
      return;
    }
    for (const auto mime : std::span(pending->read->mimes, pending->read->mimes_len)) {
      pending->contents.push_back({.mime = text(mime), .data = {}});
    }
    ++impl.clipboard_id;
    impl.clipboard = std::move(pending);
  } catch (...) {
    // Callback boundary: allocation failed before retention; deny instead of blocking parsing.
    reject(request, GHOSTTY_CLIPBOARD_READ_RESULT_IO_ERROR);
  }
}

void Terminal::Impl::clipboard_write([[maybe_unused]] GhosttyTerminal terminal_handle,
                                     void* userdata,
                                     const GhosttyClipboardWrite* request) noexcept {
  auto& impl = *static_cast<Impl*>(userdata);
  if (!impl.clipboard_write_allowed) {
    if (impl.effects.clipboard_writes_denied < std::numeric_limits<std::uint64_t>::max()) {
      ++impl.effects.clipboard_writes_denied;
    }
    reject(request, GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED);
    return;
  }
  if (impl.clipboard != nullptr || request->contents_len > representations_max) {
    reject(request, GHOSTTY_CLIPBOARD_WRITE_RESULT_BUSY);
    return;
  }
  std::size_t bytes = 0;
  for (const auto& content : std::span(request->contents, request->contents_len)) {
    if (content.data.len > limits::clipboard_decoded_bytes_max - bytes) {
      reject(request, GHOSTTY_CLIPBOARD_WRITE_RESULT_INVALID_DATA);
      return;
    }
    bytes += content.data.len;
  }
  try {
    auto pending = std::make_unique<ClipboardPending>();
    pending->contents.reserve(request->contents_len);
    if (ghostty_clipboard_write_defer(request, &pending->write) != GHOSTTY_SUCCESS) {
      reject(request, GHOSTTY_CLIPBOARD_WRITE_RESULT_IO_ERROR);
      return;
    }
    for (const auto& content : std::span(pending->write->contents, pending->write->contents_len)) {
      pending->contents.push_back(
          {.mime = text(content.mime),
           .data = std::as_bytes(std::span(content.data.ptr, content.data.len))});
    }
    ++impl.clipboard_id;
    impl.clipboard = std::move(pending);
  } catch (...) {
    // Callback boundary: allocation failed before retention; no write is acknowledged.
    reject(request, GHOSTTY_CLIPBOARD_WRITE_RESULT_IO_ERROR);
  }
}

void Terminal::set_clipboard_access(const bool read, const bool write) noexcept {
  impl_->clipboard_read_allowed = read;
  impl_->clipboard_write_allowed = write;
  if (impl_->clipboard != nullptr && ((impl_->clipboard->read != nullptr && !read) ||
                                      (impl_->clipboard->write != nullptr && !write))) {
    cancel_clipboard();
  }
}

auto Terminal::clipboard_request() const noexcept -> std::optional<ClipboardRequest> {
  const auto* const pending = impl_->clipboard.get();
  if (pending == nullptr) {
    return std::nullopt;
  }
  const auto* const read = pending->read;
  const bool osc52 = read != nullptr ? read->osc52 : pending->write->osc52;
  return ClipboardRequest{.id = impl_->clipboard_id,
                          .protocol = osc52 ? ClipboardProtocol::osc52 : ClipboardProtocol::kitty,
                          .read = read != nullptr,
                          .primary =
                              (read != nullptr ? read->location : pending->write->location) !=
                              GHOSTTY_CLIPBOARD_LOCATION_STANDARD,
                          .list = read != nullptr && read->list,
                          .name = text(read != nullptr ? read->name : pending->write->name),
                          .contents = pending->contents};
}

// One bounded completion validates representations before emitting an ordered native reply.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto Terminal::complete_clipboard(const std::uint64_t id, const ClipboardStatus status,
                                  const std::span<const ClipboardContent> contents) noexcept
    -> bool {
  if (impl_->clipboard == nullptr || impl_->clipboard_id != id ||
      contents.size() > representations_max) {
    return false;
  }
  std::size_t bytes = 0;
  for (const auto& content : contents) {
    if (content.data.size() > limits::clipboard_decoded_bytes_max - bytes ||
        content.mime.size() > 255) {
      return false;
    }
    bytes += content.data.size();
  }
  try {
    impl_->clipboard_responses.reserve(impl_->clipboard_responses.size() + (bytes * 2U) + 8192U);
  } catch (...) {
    // PTY response boundary: never emit a truncated successful reply on allocation failure.
    cancel_clipboard();
    return false;
  }
  impl_->capturing_clipboard_reply = true;
  if (const auto* request = impl_->clipboard->read) {
    std::array<GhosttyClipboardContent, representations_max> representations{};
    std::array<GhosttyString, representations_max> available{};
    for (std::size_t i = 0; i < contents.size(); ++i) {
      const auto& content = contents.subspan(i, 1).front();
      const auto mime = native(std::as_bytes(std::span(content.mime)));
      std::span(representations).subspan(i, 1).front() = {.mime = mime,
                                                          .data = native(content.data)};
      std::span(available).subspan(i, 1).front() = mime;
    }
    GhosttyClipboardReadResult result = GHOSTTY_CLIPBOARD_READ_RESULT_IO_ERROR;
    switch (status) {
    case ClipboardStatus::success:
      result = GHOSTTY_CLIPBOARD_READ_RESULT_SUCCESS;
      break;
    case ClipboardStatus::denied:
      result = GHOSTTY_CLIPBOARD_READ_RESULT_DENIED;
      break;
    case ClipboardStatus::unsupported:
      result = GHOSTTY_CLIPBOARD_READ_RESULT_UNSUPPORTED;
      break;
    case ClipboardStatus::busy:
      result = GHOSTTY_CLIPBOARD_READ_RESULT_BUSY;
      break;
    case ClipboardStatus::invalid_data:
    case ClipboardStatus::io_error:
      break;
    }
    const GhosttyClipboardReadReply reply{.size = sizeof(GhosttyClipboardReadReply),
                                          .result = result,
                                          .contents = representations.data(),
                                          .contents_len = contents.size(),
                                          .available = available.data(),
                                          .available_len = contents.size(),
                                          .remember = false};
    request->reply(request, &reply);
  } else {
    reject(impl_->clipboard->write, static_cast<GhosttyClipboardWriteResult>(status));
  }
  impl_->capturing_clipboard_reply = false;
  impl_->clipboard.reset();
  return !impl_->pty_response_integrity_failed;
}

void Terminal::cancel_clipboard() noexcept {
  if (impl_->clipboard == nullptr) {
    return;
  }
  if (impl_->clipboard->read != nullptr) {
    reject(impl_->clipboard->read, GHOSTTY_CLIPBOARD_READ_RESULT_DENIED);
  } else {
    reject(impl_->clipboard->write, GHOSTTY_CLIPBOARD_WRITE_RESULT_DENIED);
  }
  impl_->clipboard.reset();
}
} // namespace frame::vt
