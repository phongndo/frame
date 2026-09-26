#include "app/application.hpp"
#include "daemon/server.hpp"
#include "protocol/attachment.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

[[nodiscard]] auto send_all(const int connection, std::span<const std::byte> bytes) noexcept
    -> bool {
  while (!bytes.empty()) {
    const auto sent = ::send(connection, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent <= 0) {
      return false;
    }
    bytes = bytes.subspan(static_cast<std::size_t>(sent));
  }
  return true;
}

[[nodiscard]] auto parse_count(const std::string_view text) noexcept -> std::uint32_t {
  std::uint32_t value = 0;
  const auto parsed = std::from_chars(text.begin(), text.end(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.end() ? value : 0;
}

[[nodiscard]] auto attach_handshake(const int connection, const std::string_view session,
                                    const lemma::protocol::Dimensions dimensions) noexcept -> bool {
  namespace protocol = lemma::protocol;
  protocol::ServerDecoder decoder;
  if (!decoder.prepare().has_value() ||
      !send_all(connection, protocol::encode_client_hello(session, dimensions).bytes())) {
    return false;
  }
  while (true) {
    const auto decoded = decoder.next();
    if (!decoded.has_value()) {
      return false;
    }
    if (decoded->has_value()) {
      return (**decoded).kind == protocol::ServerMessageKind::hello;
    }
    const auto available = decoder.writable_bytes();
    const auto received = ::recv(connection, available.data(), available.size(), 0);
    if (received <= 0 || !decoder.commit(static_cast<std::size_t>(received)).has_value()) {
      return false;
    }
  }
}

// COUNT alternating cell-size/resize pairs back to back, followed by one input byte `K`.
[[nodiscard]] auto encode_burst(const std::uint32_t count,
                                const lemma::protocol::Dimensions dimensions,
                                std::uint32_t& sequence) -> std::vector<std::byte> {
  namespace protocol = lemma::protocol;
  std::vector<std::byte> burst;
  const auto append = [&](const std::span<const std::byte> bytes) {
    burst.insert(burst.end(), bytes.begin(), bytes.end());
  };
  for (std::uint32_t index = 0; index < count; ++index) {
    append(protocol::encode_cell_size({.width = 8, .height = 16}, sequence++).bytes());
    const auto columns = static_cast<std::uint16_t>(dimensions.columns + 1U + (index % 2U));
    append(
        protocol::encode_resize({.columns = columns, .rows = dimensions.rows}, sequence++).bytes());
  }
  constexpr std::array input{std::byte{'K'}};
  append(protocol::encode_input_header(input.size(), sequence++));
  append(input);
  return burst;
}

// Drains presentation output and writes more of the burst as socket readiness allows.
[[nodiscard]] auto service_socket(const int connection, const short revents,
                                  std::span<const std::byte>& unsent,
                                  const std::span<std::byte> discard) noexcept -> bool {
  if ((revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
      ::recv(connection, discard.data(), discard.size(), MSG_DONTWAIT) <= 0) {
    return false;
  }
  if (unsent.empty() || (revents & POLLOUT) == 0) {
    return true;
  }
  const auto sent = ::send(connection, unsent.data(), unsent.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
  if (sent < 0) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
  }
  unsent = unsent.subspan(static_cast<std::size_t>(sent));
  return true;
}

// Writes the burst while draining presentation output, as an attached client would, and keeps
// the attachment until stdin closes.
[[nodiscard]] auto deliver_until_stdin_closes(const int connection,
                                              std::span<const std::byte> unsent) noexcept -> bool {
  std::array<std::byte, std::size_t{64} * 1024U> discard{};
  while (true) {
    const auto events = static_cast<short>(unsent.empty() ? POLLIN : POLLIN | POLLOUT);
    std::array<pollfd, 2> descriptors{{
        {.fd = connection, .events = events, .revents = 0},
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
    }};
    if (::poll(descriptors.data(), descriptors.size(), -1) <= 0) {
      continue;
    }
    if (!service_socket(connection, descriptors.front().revents, unsent, discard)) {
      return false;
    }
    if ((descriptors.back().revents & (POLLIN | POLLHUP)) != 0 &&
        ::read(STDIN_FILENO, discard.data(), discard.size()) <= 0) {
      return unsent.empty();
    }
  }
}

// Attaches like a client whose geometry messages outran the daemon.
[[nodiscard]] auto geometry_burst(const lemma::daemon::RuntimeEndpoint& endpoint,
                                  const std::string_view session, const std::uint32_t count)
    -> int {
  constexpr lemma::protocol::Dimensions initial{.columns = 100, .rows = 30};
  const int connection = lemma::daemon::open_server_connection(endpoint);
  if (connection < 0) {
    return 1;
  }
  std::uint32_t sequence = 2;
  const bool succeeded =
      attach_handshake(connection, session, initial) &&
      deliver_until_stdin_closes(connection, encode_burst(count, initial, sequence)) &&
      send_all(connection, lemma::protocol::encode_detach(sequence).bytes());
  static_cast<void>(::close(connection));
  return succeeded ? 0 : 1;
}

} // namespace

int main(const int argc, char** argv) {
  try {
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() < 2) {
      return 2;
    }
    const auto endpoint =
        lemma::daemon::RuntimeEndpoint::create(std::string_view(arguments.subspan<1, 1>().front()));
    if (!endpoint.has_value()) {
      return 2;
    }
    if (arguments.size() == 5U &&
        std::string_view(arguments.subspan<2, 1>().front()) == "geometry-burst") {
      const auto count = parse_count(arguments.subspan<4, 1>().front());
      return count == 0 ? 2 : geometry_burst(*endpoint, arguments.subspan<3, 1>().front(), count);
    }

    std::vector<char*> app_arguments;
    app_arguments.reserve(arguments.size() - 1U);
    app_arguments.push_back(arguments.front());
    for (char* const argument : arguments.subspan(2)) {
      app_arguments.push_back(argument);
    }
    if (app_arguments.size() > 1U) {
      const std::string_view command(std::span(app_arguments).subspan(1, 1).front());
      if (command == "tab" || command == "pane" || command == "shutdown" || command == "demo") {
        return lemma::app::run_legacy(*endpoint, static_cast<int>(app_arguments.size()),
                                      app_arguments.data());
      }
    }
    return lemma::app::run(*endpoint, static_cast<int>(app_arguments.size()), app_arguments.data());
  } catch (...) {
    return 2;
  }
}
