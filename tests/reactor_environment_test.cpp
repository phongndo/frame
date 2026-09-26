#include "core/engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

namespace lemma::core {
namespace {

struct ConnectedListener final {
  int listener{-1};
  int client{-1};
};

[[nodiscard]] auto connect_listener(const sockaddr* const address, const socklen_t size) noexcept
    -> std::optional<ConnectedListener> {
  const auto listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0) {
    return std::nullopt;
  }
  if (::bind(listener, address, size) != 0 || ::listen(listener, 1) != 0) {
    static_cast<void>(::close(listener));
    return std::nullopt;
  }
  const auto client = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (client < 0 || ::connect(client, address, size) != 0) {
    if (client >= 0) {
      static_cast<void>(::close(client));
    }
    static_cast<void>(::close(listener));
    return std::nullopt;
  }
  return ConnectedListener{.listener = listener, .client = client};
}

[[nodiscard]] auto connected_listener() noexcept -> std::optional<ConnectedListener> {
  // Use the production transport. TCP delivery/FIN depends on host scheduling even on loopback;
  // declaring it ready and advancing virtual time makes host latency look like reactor work.
  auto directory = std::to_array("/tmp/lemma-reactor-XXXXXX");
  if (::mkdtemp(directory.data()) == nullptr) {
    return std::nullopt;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = std::span(address.sun_path);
  constexpr auto suffix = std::to_array("/socket");
  static_assert(directory.size() + suffix.size() <= sizeof(address.sun_path));
  std::ranges::copy(std::span(directory).first(directory.size() - 1U), path.begin());
  std::ranges::copy(suffix, path.subspan(directory.size() - 1U).begin());
  // POSIX takes the protocol-specific address through its generic address type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  const auto* const generic_address = reinterpret_cast<const sockaddr*>(&address);
  const auto result = connect_listener(generic_address, sizeof(address));
  static_cast<void>(::unlink(path.data()));
  static_cast<void>(::rmdir(directory.data()));
  return result;
}

enum class ScriptMode : std::uint8_t {
  fragmented_request,
  partial_request_timeout,
};

struct ScriptedReactor final {
  ReactorClock::time_point now;
  int listener{-1};
  int client{-1};
  int wake_read{-1};
  std::array<std::string_view, 4> fragments{};
  std::size_t fragment_count{0};
  std::size_t stage{0};
  std::size_t polls{0};
  int initial_poll_timeout{-2};
  int retry_poll_timeout{-2};
  std::size_t clock_reads{0};
  std::size_t releases{0};
  std::size_t sends{0};
  std::size_t blocked_sends{0};
  std::size_t partial_sends{0};
  std::size_t interrupted_polls{0};
  std::size_t readiness_events{0};
  std::size_t reaped_exits{0};
  std::array<char, std::size_t{4} * 1024> response{};
  std::size_t response_size{0};
  ScriptMode mode{ScriptMode::fragmented_request};
  bool child_exit_pending{false};
  bool block_next_send{true};
  bool partial_next_send{true};
  bool interrupt_next_poll{true};
  bool wake_before_accept{false};
  bool early_wake_delivered{false};
  bool positive_timeout_seen{false};
  bool timeout_closed_peer{false};
  bool stop{false};
  bool failed{false};
};

thread_local ScriptedReactor* active_script = nullptr;

[[nodiscard]] auto scripted_stop() noexcept -> bool {
  return active_script != nullptr && active_script->stop;
}

[[nodiscard]] auto scripted_reap(void* const context) noexcept -> std::optional<ChildExit> {
  auto& script = *static_cast<ScriptedReactor*>(context);
  if (!script.child_exit_pending) {
    return std::nullopt;
  }
  script.child_exit_pending = false;
  ++script.reaped_exits;
  return ChildExit{.process = 424'242, .status = 0};
}

[[nodiscard]] auto pending_descriptor(const ScriptedReactor& script,
                                      const std::span<pollfd> descriptors) noexcept -> pollfd* {
  const auto found = std::ranges::find_if(descriptors, [&script](const pollfd& descriptor) {
    return descriptor.fd != script.listener && descriptor.fd != script.wake_read;
  });
  return found == descriptors.end() ? nullptr : std::to_address(found);
}

[[nodiscard]] auto send_fragment(ScriptedReactor& script, pollfd& pending,
                                 const std::string_view fragment) noexcept -> bool {
  const auto sent = ::send(script.client, fragment.data(), fragment.size(), MSG_NOSIGNAL);
  if (sent < 0 || static_cast<std::size_t>(sent) != fragment.size() ||
      (pending.events & POLLIN) == 0) {
    return false;
  }
  pending.revents = POLLIN;
  return true;
}

[[nodiscard]] auto poll_request_fragment(ScriptedReactor& script,
                                         const std::span<pollfd> descriptors) noexcept -> int {
  auto* const pending = pending_descriptor(script, descriptors);
  const auto fragment = std::span(script.fragments).subspan(script.stage - 1U, 1).front();
  if (pending == nullptr || !send_fragment(script, *pending, fragment)) {
    script.failed = true;
    return -1;
  }
  const bool final_fragment = script.stage == script.fragment_count;
  ++script.stage;
  if (!final_fragment) {
    return 1;
  }
  if (!script.wake_before_accept) {
    descriptors.subspan(1, 1).front().revents = POLLIN;
    script.child_exit_pending = true;
    return 2;
  }
  return 1;
}

[[nodiscard]] auto poll_response_write(ScriptedReactor& script,
                                       const std::span<pollfd> descriptors) noexcept -> int {
  auto* const pending = pending_descriptor(script, descriptors);
  if (pending == nullptr || (pending->events & POLLOUT) == 0) {
    script.failed = true;
    return -1;
  }
  pending->revents = POLLOUT;
  ++script.stage;
  return 1;
}

[[nodiscard]] auto collect_response(ScriptedReactor& script) noexcept -> int {
  const auto available = std::span(script.response).subspan(script.response_size);
  const auto received = ::recv(script.client, available.data(), available.size(), MSG_DONTWAIT);
  if (received > 0) {
    script.response_size += static_cast<std::size_t>(received);
  } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
    script.failed = true;
    return -1;
  }
  const std::string_view response(script.response.data(), script.response_size);
  if (response.ends_with('\n') && response.contains(R"("schema":"lemma.proc-result/v1")") &&
      response.contains(R"("status":"applied")")) {
    script.stop = true;
  } else if (script.polls > 32U) {
    script.failed = true;
    return -1;
  }
  return 0;
}

[[nodiscard]] auto poll_fragmented(ScriptedReactor& script,
                                   const std::span<pollfd> descriptors) noexcept -> int {
  if (script.wake_before_accept && !script.early_wake_delivered) {
    descriptors.subspan(1, 1).front().revents = POLLIN;
    script.child_exit_pending = true;
    script.early_wake_delivered = true;
    return 1;
  }
  if (script.stage == 0U) {
    descriptors.front().revents = POLLIN;
    ++script.stage;
    return 1;
  }
  if (script.stage <= script.fragment_count) {
    return poll_request_fragment(script, descriptors);
  }
  auto* const pending = pending_descriptor(script, descriptors);
  if (script.stage == script.fragment_count + 1U && pending != nullptr &&
      (pending->events & POLLIN) != 0) {
    // Proc execution may need a turn after the last request fragment. The Unix transport has
    // already delivered that fragment; this is not a retry loop for host TCP delivery.
    pending->revents = POLLIN;
    return 1;
  }
  if (script.stage <= script.fragment_count + 2U) {
    return poll_response_write(script, descriptors);
  }
  return collect_response(script);
}

[[nodiscard]] auto poll_timeout(ScriptedReactor& script,
                                const std::span<pollfd> descriptors) noexcept -> int {
  if (script.stage == 0U) {
    descriptors.front().revents = POLLIN;
    ++script.stage;
    return 1;
  }
  if (script.stage == 1U) {
    auto* const pending = pending_descriptor(script, descriptors);
    if (pending == nullptr || !send_fragment(script, *pending, "{")) {
      script.failed = true;
      return -1;
    }
    ++script.stage;
    return 1;
  }
  if (script.stage == 2U) {
    script.now += std::chrono::seconds{6};
    ++script.stage;
    return 0;
  }
  std::byte byte{};
  const auto received = ::recv(script.client, &byte, 1, MSG_DONTWAIT);
  if (received == 0 || (received < 0 && errno == ECONNRESET)) {
    // Both EOF and reset represent a closed production connection.
    script.timeout_closed_peer = true;
    script.stop = true;
  }
  const bool receive_failed =
      received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNRESET;
  if (receive_failed || (!script.timeout_closed_peer && script.polls > 16U)) {
    script.failed = true;
    return -1;
  }
  return 0;
}

[[nodiscard]] auto scripted_poll(void* const context, const std::span<pollfd> descriptors,
                                 const int timeout_milliseconds) noexcept -> int {
  auto& script = *static_cast<ScriptedReactor*>(context);
  if (script.polls == 0) {
    script.initial_poll_timeout = timeout_milliseconds;
  } else if (script.polls == 1) {
    script.retry_poll_timeout = timeout_milliseconds;
  }
  ++script.polls;
  script.positive_timeout_seen = script.positive_timeout_seen || timeout_milliseconds > 0;
  if (descriptors.size() < 2U || descriptors.front().fd != script.listener ||
      (descriptors.front().events & POLLIN) == 0 ||
      descriptors.subspan(1, 1).front().fd != script.wake_read ||
      (descriptors.subspan(1, 1).front().events & POLLIN) == 0) {
    script.failed = true;
    return -1;
  }
  for (auto& descriptor : descriptors) {
    descriptor.revents = 0;
  }
  if (script.interrupt_next_poll) {
    script.interrupt_next_poll = false;
    ++script.interrupted_polls;
    errno = EINTR;
    return -1;
  }
  script.now += std::chrono::milliseconds{1};
  const auto ready = script.mode == ScriptMode::fragmented_request
                         ? poll_fragmented(script, descriptors)
                         : poll_timeout(script, descriptors);
  if (ready > 0) {
    script.readiness_events += static_cast<std::size_t>(ready);
  }
  return ready;
}

[[nodiscard]] auto scripted_now(void* const context) noexcept -> ReactorClock::time_point {
  auto& script = *static_cast<ScriptedReactor*>(context);
  ++script.clock_reads;
  return script.now;
}

// Sends at most `limit` leading bytes of head followed by tail in one gathered call.
[[nodiscard]] auto send_gathered(const int descriptor, const std::span<const std::byte> head,
                                 const std::span<const std::byte> tail, const std::size_t limit,
                                 const int flags) noexcept -> ReactorIoResult {
  const auto head_bytes = std::min(limit, head.size());
  const auto tail_bytes = std::min(limit - head_bytes, tail.size());
  // iovec is a C ABI that never writes through iov_base for sendmsg.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
  std::array<iovec, 2> vectors{{
      {.iov_base = const_cast<std::byte*>(head.data()), .iov_len = head_bytes},
      {.iov_base = const_cast<std::byte*>(tail.data()), .iov_len = tail_bytes},
  }};
  // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
  msghdr message{};
  message.msg_iov = vectors.data();
  message.msg_iovlen = vectors.size();
  const auto sent = ::sendmsg(descriptor, &message, flags);
  return {.bytes = sent, .error = sent < 0 ? errno : 0};
}

[[nodiscard]] auto scripted_send(void* const context, const int descriptor,
                                 const std::span<const std::byte> head,
                                 const std::span<const std::byte> tail, const int flags) noexcept
    -> ReactorIoResult {
  auto& script = *static_cast<ScriptedReactor*>(context);
  ++script.sends;
  const auto size = head.size() + tail.size();
  if (script.block_next_send && script.mode == ScriptMode::fragmented_request) {
    script.block_next_send = false;
    ++script.blocked_sends;
    return {.bytes = -1, .error = EAGAIN};
  }
  if (script.partial_next_send && script.mode == ScriptMode::fragmented_request && size > 1U) {
    script.partial_next_send = false;
    ++script.partial_sends;
    return send_gathered(descriptor, head, tail, size / 2U, flags);
  }
  return send_gathered(descriptor, head, tail, size, flags);
}

void release_listener(void* const context) noexcept {
  auto& script = *static_cast<ScriptedReactor*>(context);
  ++script.releases;
  if (script.listener >= 0) {
    static_cast<void>(::close(script.listener));
    script.listener = -1;
  }
}

[[nodiscard]] auto run_script(ScriptedReactor& script) noexcept -> int {
  active_script = &script;
  const ReactorEnvironment environment{
      .context = &script,
      .poll = &scripted_poll,
      .now = &scripted_now,
      .send = &scripted_send,
      .input_map = nullptr,
      .scrollback_lines = std::nullopt,
      .default_program = {},
      .default_cwd = {},
      .command_history_file = {},
      .status_line = true,
      .extension_commands = {},
  };
  const auto result = run_server_with_environment(
      script.listener, &release_listener, &script, &scripted_stop,
      ChildReaper{.wake_descriptor = script.wake_read, .reap = &scripted_reap, .context = &script},
      environment);
  active_script = nullptr;
  return result;
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ReactorEnvironmentTest, ReportedReadinessHasExactBytesAndSynchronousClosure) {
  const auto connection = connected_listener();
  ASSERT_TRUE(connection.has_value());
  const auto connected = connection.value_or(ConnectedListener{});
  const auto accepted = ::accept(connected.listener, nullptr, nullptr);
  if (accepted < 0) {
    static_cast<void>(::close(connected.client));
    static_cast<void>(::close(connected.listener));
    FAIL() << "accept failed";
  }
  ScriptedReactor script{.now = {}, .client = connected.client};
  pollfd pending{.fd = accepted, .events = POLLIN, .revents = 0};
  constexpr std::string_view fragment = "fragment-boundary";
  std::array<char, fragment.size()> received{};
  bool exact = accepted >= 0;
  for (std::size_t index = 0; index < 64U && exact; ++index) {
    exact = send_fragment(script, pending, fragment) && pending.revents == POLLIN &&
            ::recv(accepted, received.data(), received.size(), MSG_DONTWAIT) ==
                static_cast<ssize_t>(fragment.size()) &&
            std::string_view(received.data(), received.size()) == fragment;
  }
  static_cast<void>(::close(accepted));
  char byte = 0;
  const auto closed = ::recv(connected.client, &byte, 1, MSG_DONTWAIT);
  static_cast<void>(::close(connected.client));
  static_cast<void>(::close(connected.listener));
  EXPECT_TRUE(exact);
  EXPECT_EQ(closed, 0);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ReactorEnvironmentTest, ResponseCollectionWaitsForRecordBoundary) {
  const auto connection = connected_listener();
  ASSERT_TRUE(connection.has_value());
  const auto connected = connection.value_or(ConnectedListener{});
  const auto accepted = ::accept(connected.listener, nullptr, nullptr);
  if (accepted < 0) {
    static_cast<void>(::close(connected.client));
    static_cast<void>(::close(connected.listener));
    FAIL() << "accept failed";
  }
  constexpr std::string_view prefix = R"({"schema":"lemma.proc-result/v1","status":"applied")";
  constexpr std::string_view suffix = "}\n";
  ScriptedReactor script{.now = {}, .client = connected.client};
  EXPECT_EQ(::send(accepted, prefix.data(), prefix.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(prefix.size()));
  EXPECT_EQ(collect_response(script), 0);
  EXPECT_FALSE(script.stop);
  EXPECT_EQ(::send(accepted, suffix.data(), suffix.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(suffix.size()));
  EXPECT_EQ(collect_response(script), 0);
  EXPECT_TRUE(script.stop);
  EXPECT_EQ(script.response_size, prefix.size() + suffix.size());
  static_cast<void>(::close(accepted));
  static_cast<void>(::close(connected.client));
  static_cast<void>(::close(connected.listener));
}

TEST(ReactorEnvironmentTest, ProductionEnvironmentIsComplete) {
  EXPECT_TRUE(production_reactor_environment().valid());
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ReactorEnvironmentTest, ScriptedWorldControlsFragmentationBackpressureChildExitAndOrdering) {
  const auto connection = connected_listener();
  ASSERT_TRUE(connection.has_value());
  if (!connection.has_value()) {
    return;
  }
  const auto connected = connection.value();
  std::array<int, 2> wake{-1, -1};
  ASSERT_EQ(::pipe(wake.data()), 0);
  ScriptedReactor script{
      .now = {},
      .listener = connected.listener,
      .client = connected.client,
      .wake_read = wake.front(),
      .fragments = {"{", R"("schema":"lemma.proc/v1",)",
                    R"("commands":[{"command":"daemon.inspect"})", "]}\n"},
      .fragment_count = 4,
  };

  const auto result = run_script(script);

  static_cast<void>(::close(connected.client));
  static_cast<void>(::close(wake.front()));
  static_cast<void>(::close(wake.back()));
  EXPECT_EQ(result, 0);
  EXPECT_FALSE(script.failed);
  EXPECT_TRUE(script.stop);
  EXPECT_GT(script.polls, script.fragment_count);
  EXPECT_GT(script.clock_reads, 0U);
  EXPECT_TRUE(script.positive_timeout_seen);
  EXPECT_EQ(script.blocked_sends, 1U);
  EXPECT_EQ(script.partial_sends, 1U);
  EXPECT_EQ(script.interrupted_polls, 1U);
  EXPECT_GE(script.sends, 3U);
  EXPECT_EQ(script.reaped_exits, 1U);
  EXPECT_EQ(script.releases, 1U);
  RecordProperty("reactor_poll_calls", static_cast<int>(script.polls));
  RecordProperty("readiness_events", static_cast<int>(script.readiness_events));
  RecordProperty("outbound_send_calls", static_cast<int>(script.sends));
  RecordProperty("blocked_sends", static_cast<int>(script.blocked_sends));
  RecordProperty("partial_sends", static_cast<int>(script.partial_sends));
  RecordProperty("child_wakeups", static_cast<int>(script.reaped_exits));
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ReactorEnvironmentTest, ChildWakeCanPrecedeAcceptAndFragmentedRequest) {
  const auto connection = connected_listener();
  ASSERT_TRUE(connection.has_value());
  if (!connection.has_value()) {
    return;
  }
  const auto connected = connection.value();
  std::array<int, 2> wake{-1, -1};
  ASSERT_EQ(::pipe(wake.data()), 0);
  ScriptedReactor script{
      .now = {},
      .listener = connected.listener,
      .client = connected.client,
      .wake_read = wake.front(),
      .fragments = {"{", R"("schema":"lemma.proc/v1",)",
                    R"("commands":[{"command":"daemon.inspect"})", "]}\n"},
      .fragment_count = 4,
      .wake_before_accept = true,
  };

  const auto result = run_script(script);

  static_cast<void>(::close(connected.client));
  static_cast<void>(::close(wake.front()));
  static_cast<void>(::close(wake.back()));
  EXPECT_EQ(result, 0);
  EXPECT_FALSE(script.failed);
  EXPECT_TRUE(script.stop);
  EXPECT_TRUE(script.early_wake_delivered);
  EXPECT_EQ(script.reaped_exits, 1U);
  EXPECT_EQ(script.blocked_sends, 1U);
  EXPECT_EQ(script.partial_sends, 1U);
  EXPECT_EQ(script.releases, 1U);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ReactorEnvironmentTest, ChildReapedBeforePollCannotSleepBeforePublishingItsOutcome) {
  const auto connection = connected_listener();
  ASSERT_TRUE(connection.has_value());
  if (!connection.has_value()) {
    return;
  }
  const auto connected = connection.value();
  std::array<int, 2> wake{-1, -1};
  ASSERT_EQ(::pipe(wake.data()), 0);
  ScriptedReactor script{
      .now = {},
      .listener = connected.listener,
      .client = connected.client,
      .wake_read = wake.front(),
      .fragments = {"{", R"("schema":"lemma.proc/v1",)",
                    R"("commands":[{"command":"daemon.inspect"})", "]}\n"},
      .fragment_count = 4,
      .child_exit_pending = true,
      .wake_before_accept = true,
  };

  const auto result = run_script(script);

  static_cast<void>(::close(connected.client));
  static_cast<void>(::close(wake.front()));
  static_cast<void>(::close(wake.back()));
  EXPECT_EQ(result, 0);
  EXPECT_FALSE(script.failed);
  EXPECT_TRUE(script.stop);
  EXPECT_TRUE(script.early_wake_delivered);
  EXPECT_EQ(script.reaped_exits, 2U);
  EXPECT_EQ(script.initial_poll_timeout, 0);
  EXPECT_EQ(script.retry_poll_timeout, 0);
  EXPECT_EQ(script.blocked_sends, 1U);
  EXPECT_EQ(script.partial_sends, 1U);
  EXPECT_EQ(script.releases, 1U);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ReactorEnvironmentTest, VirtualDeadlineClosesAFragmentedRequestWithoutHostTime) {
  const auto connection = connected_listener();
  ASSERT_TRUE(connection.has_value());
  if (!connection.has_value()) {
    return;
  }
  const auto connected = connection.value();
  std::array<int, 2> wake{-1, -1};
  ASSERT_EQ(::pipe(wake.data()), 0);
  ScriptedReactor script{
      .now = {},
      .listener = connected.listener,
      .client = connected.client,
      .wake_read = wake.front(),
      .mode = ScriptMode::partial_request_timeout,
      .block_next_send = false,
  };

  const auto result = run_script(script);

  static_cast<void>(::close(connected.client));
  static_cast<void>(::close(wake.front()));
  static_cast<void>(::close(wake.back()));
  EXPECT_EQ(result, 0);
  EXPECT_FALSE(script.failed);
  EXPECT_TRUE(script.timeout_closed_peer);
  EXPECT_TRUE(script.positive_timeout_seen);
  EXPECT_GE(script.now.time_since_epoch(), std::chrono::seconds{6});
  EXPECT_EQ(script.releases, 1U);
}

} // namespace
} // namespace lemma::core
