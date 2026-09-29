#include "extension/commands.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <unistd.h>

namespace frame::extension {
namespace {

// GTest assertions establish presence before the test examines decoded values.
// NOLINTBEGIN(bugprone-unchecked-optional-access)

class SocketPair final {
public:
  SocketPair() : valid_(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors_.data()) == 0) {}
  ~SocketPair() {
    for (const int descriptor : descriptors_) {
      if (descriptor >= 0) {
        static_cast<void>(::close(descriptor));
      }
    }
  }
  SocketPair(const SocketPair&) = delete;
  auto operator=(const SocketPair&) -> SocketPair& = delete;
  SocketPair(SocketPair&&) = delete;
  auto operator=(SocketPair&&) -> SocketPair& = delete;
  [[nodiscard]] auto valid() const noexcept -> bool { return valid_; }
  [[nodiscard]] auto left() const noexcept -> int { return descriptors_.front(); }
  [[nodiscard]] auto right() const noexcept -> int { return descriptors_.back(); }

private:
  std::array<int, 2> descriptors_{-1, -1};
  bool valid_{false};
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ExtensionCommandsTest, QualifiedNamesCannotShadowNativeRoots) {
  for (const auto* const name : {"project.open", "agent-1.run_task", "user.tabs.next"}) {
    EXPECT_TRUE(valid_command_name(name));
  }
  for (const auto* const name : {"pane", "pane split", ".open", "project.", "project..open",
                                 "1.open", "project.1", "project.Open", "project.\nopen"}) {
    EXPECT_FALSE(valid_command_name(name));
  }
  EXPECT_FALSE(valid_command_name(std::string(command_name_bytes_max, 'a') + ".b"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ExtensionCommandsTest, RegistrationRejectsDuplicateAndUnboundedDescriptors) {
  const auto parsed = api::parse_json(R"([
    {"name":"project.open","description":"Open project","timeout_ms":1000}
  ])");
  ASSERT_TRUE(parsed.value.has_value());
  auto commands = decode_commands(parsed.value.value());
  ASSERT_TRUE(commands.has_value());
  ASSERT_EQ(commands.value().size(), 1);
  EXPECT_EQ(commands.value().front().name, "project.open");
  EXPECT_EQ(commands.value().front().timeout_ms, 1000);
  auto duplicate = parsed.value.value();
  duplicate.array.push_back(duplicate.array.front());
  EXPECT_FALSE(decode_commands(duplicate).has_value());
  for (const auto* const text :
       {R"([{"name":"pane","description":"","timeout_ms":1}])",
        R"([{"name":"a.b","description":"","timeout_ms":0}])",
        R"([{"name":"a.b","description":"","timeout_ms":600001}])",
        R"([{"name":"a.b","description":"\u001b","timeout_ms":1}])",
        R"([{"name":"a.b","description":"","timeout_ms":1,"extra":true}])"}) {
    const auto invalid = api::parse_json(text);
    ASSERT_TRUE(invalid.value.has_value());
    EXPECT_FALSE(decode_commands(invalid.value.value()).has_value()) << text;
  }
}

TEST(ExtensionChannelTest, FragmentedRecordsAreDecodedOnceAndInOrder) {
  SocketPair sockets;
  ASSERT_TRUE(sockets.valid());
  CommandChannel channel(sockets.left());
  const std::string first = R"({"kind":"complete","invocation":7,"payload":{"ok":true}})";
  ASSERT_EQ(::send(sockets.right(), first.data(), 9, MSG_NOSIGNAL), 9);
  channel.read_ready();
  EXPECT_FALSE(channel.buffered());
  EXPECT_FALSE(channel.receive().has_value());
  const auto suffix = first.substr(9) + '\n' + first + '\n';
  ASSERT_EQ(::send(sockets.right(), suffix.data(), suffix.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(suffix.size()));
  channel.read_ready();
  ASSERT_TRUE(channel.buffered());
  const auto message = channel.receive();
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message.value().invocation, 7);
  EXPECT_EQ(api::json_boolean(message.value().payload, "ok"), true);
  EXPECT_TRUE(channel.buffered());
  EXPECT_TRUE(channel.receive().has_value());
  EXPECT_FALSE(channel.receive().has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ExtensionChannelTest, PartialWritesAndBackpressureRetainTheExactSuffix) {
  SocketPair sockets;
  ASSERT_TRUE(sockets.valid());
  const int buffer_size = 1024;
  ASSERT_EQ(::setsockopt(sockets.left(), SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)),
            0);
  CommandChannel writer(sockets.left());
  CommandChannel reader(sockets.right());
  const std::string content(std::size_t{128} * 1024U, 'x');
  const auto payload = '"' + content + "\"\n";
  ASSERT_TRUE(writer.send("result", 42, payload));
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    writer.write_ready(); // The peer is deliberately not drained: most attempts hit EAGAIN.
  }
  std::optional<CommandMessage> message;
  for (std::size_t turn = 0; turn < 4096 && !message.has_value(); ++turn) {
    writer.write_ready();
    reader.read_ready();
    message = reader.receive();
  }
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message.value().payload.kind, api::JsonKind::string);
  EXPECT_EQ(message.value().payload.string, content);
  EXPECT_FALSE(reader.receive().has_value());
}

TEST(ExtensionChannelTest, QueueCapacityAndMalformedRecordsFailExplicitly) {
  SocketPair sockets;
  ASSERT_TRUE(sockets.valid());
  CommandChannel channel(sockets.left());
  const auto payload = '"' + std::string(std::size_t{700} * 1024U, 'x') + '"';
  EXPECT_TRUE(channel.send("result", 1, payload));
  EXPECT_TRUE(channel.send("result", 2, payload));
  EXPECT_FALSE(channel.send("result", 3, payload));
  EXPECT_FALSE(channel.send("result", 4, std::string(api::json_bytes_max, 'x')));
  constexpr std::string_view malformed = "{\"kind\":\"proc\",\"invocation\":0,\"payload\":{}}\n";
  ASSERT_EQ(::send(sockets.right(), malformed.data(), malformed.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(malformed.size()));
  channel.read_ready();
  EXPECT_FALSE(channel.receive().has_value());
  EXPECT_EQ(channel.descriptor(), -1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ExtensionRuntimeTest, CancellationRevokesProcOwnershipButRetainsBoundedWatchdogUntilAck) {
  SocketPair sockets;
  ASSERT_TRUE(sockets.valid());
  const std::array commands{
      CommandDescriptor{.name = "test.run", .description = "Run", .timeout_ms = 1000}};
  CommandRuntime runtime(sockets.left(), commands, nullptr, nullptr);
  const auto now = std::chrono::steady_clock::time_point{};
  const InvocationContext context{.session = SessionId::from_parts(1, 2),
                                  .tab = TabId::from_parts(3, 4),
                                  .pane = PaneId::from_parts(5, 6),
                                  .connection = ConnectionId::from_parts(1, 7)};
  for (std::size_t index = 0; index < invocations_max; ++index) {
    ASSERT_TRUE(runtime.start("test.run", {}, context, now));
  }
  EXPECT_FALSE(runtime.start("test.run", {}, context, now));
  auto* invocation = runtime.find(1);
  ASSERT_NE(invocation, nullptr);
  EXPECT_EQ(invocation->context.pane, context.pane);
  runtime.cancel(1);
  EXPECT_EQ(runtime.find(1), nullptr);
  EXPECT_FALSE(runtime.result(1, "{}"));
  EXPECT_FALSE(runtime.start("test.run", {}, context, now));
  EXPECT_EQ(runtime.poll_timeout(-1, now), 1000);
  EXPECT_EQ(runtime.poll_timeout(-1, now + std::chrono::seconds(1)), 0);
  runtime.finish(1);
  EXPECT_TRUE(runtime.start("test.run", {}, context, now));
  EXPECT_EQ(runtime.find(1), nullptr);
  EXPECT_NE(runtime.find(invocations_max + 1U), nullptr);
  runtime.fail();
  EXPECT_TRUE(runtime.commands().empty());
  EXPECT_EQ(runtime.find(2), nullptr);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace
} // namespace frame::extension
