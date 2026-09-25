#include "config/config.hpp"
#include "extension/lua_host.hpp"
#include "input/input_router.hpp"
#include "platform/io.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <unistd.h>

namespace lemma::config {
namespace {

class TemporaryConfig final {
public:
  explicit TemporaryConfig(const std::string_view contents)
      : path_("/tmp/lemma-config-test-XXXXXX") {
    const auto descriptor = ::mkstemp(path_.data());
    if (descriptor >= 0) {
      valid_ = platform::write_text(descriptor, contents);
      static_cast<void>(::close(descriptor));
    }
  }

  TemporaryConfig(const TemporaryConfig&) = delete;
  auto operator=(const TemporaryConfig&) -> TemporaryConfig& = delete;
  TemporaryConfig(TemporaryConfig&&) = delete;
  auto operator=(TemporaryConfig&&) -> TemporaryConfig& = delete;
  ~TemporaryConfig() {
    if (!path_.empty()) {
      static_cast<void>(::unlink(path_.c_str()));
    }
  }

  [[nodiscard]] auto valid() const noexcept -> bool { return valid_; }
  [[nodiscard]] auto path() const noexcept -> std::string_view { return path_; }

private:
  std::string path_;
  bool valid_{false};
};

TEST(ConfigurationTest, ParsesCanonicalCommandChords) {
  EXPECT_EQ(parse_key("C-a"), input::InputChord::byte('a', input::key_modifier_control));
  EXPECT_EQ(parse_key("C-S-a"),
            input::InputChord::byte('a', input::key_modifier_control | input::key_modifier_shift));
  EXPECT_EQ(parse_key("S-a"), input::InputChord::byte('A'));
  EXPECT_EQ(parse_key("M-Left"),
            input::InputChord::key(input::PhysicalKey::arrow_left, input::key_modifier_alt));
  EXPECT_EQ(parse_key("Enter"), input::InputChord::byte(0x0DU));
  EXPECT_EQ(parse_key("Cmd-b"), input::InputChord::byte('b', input::key_modifier_super));
  EXPECT_FALSE(parse_key("C-").has_value());
  EXPECT_FALSE(parse_key("mouse-1").has_value());
}

TEST(ConfigurationTest, RoundTripsAndCompilesOneCompleteGeneration) {
  const auto prefix_chord = parse_key("C-a");
  const auto split_chord = parse_key("s");
  const auto removed_chord = parse_key("%");
  ASSERT_TRUE(prefix_chord.has_value());
  ASSERT_TRUE(split_chord.has_value());
  ASSERT_TRUE(removed_chord.has_value());
  Configuration source;
  ASSERT_TRUE(source.input.set_prefix(prefix_chord));
  source.terminal.scrollback_lines = 12'345;
  source.ui.status_line = false;
  source.ui.outer = {
      .title = false, .bell = false, .notifications = false, .progress = true, .cwd = false};
  source.launch.default_cwd = "/tmp";
  source.launch.default_program = {"/bin/sh", "-l"};
  source.history.file = "/tmp/lemma-history";
  ASSERT_TRUE(source.input.set(input::ConfiguredInputContext::prefix,
                               split_chord.value_or(input::InputChord{}),
                               input::InputCommand::split_left_right));
  ASSERT_TRUE(source.input.unbind(input::ConfiguredInputContext::prefix,
                                  removed_chord.value_or(input::InputChord{})));

  const auto document = encode(source);
  ASSERT_TRUE(document.has_value());
  const auto encoded = document.value_or(std::string{});
  EXPECT_NE(encoded.find(R"("contexts")"), std::string::npos);
  EXPECT_NE(encoded.find(R"("kind":"push")"), std::string::npos);
  EXPECT_EQ(encoded.find(R"("modes")"), std::string::npos);
  const auto decoded = decode(encoded);
  ASSERT_TRUE(decoded.configuration.has_value());
  const auto compiled = compile(decoded.configuration.value_or(Configuration{}));
  ASSERT_TRUE(compiled.has_value());
  EXPECT_EQ(compiled->scrollback_lines(), 12'345U);
  EXPECT_FALSE(compiled->status_line());
  EXPECT_FALSE(compiled->outer().title);
  EXPECT_FALSE(compiled->outer().bell);
  EXPECT_FALSE(compiled->outer().notifications);
  EXPECT_TRUE(compiled->outer().progress);
  EXPECT_FALSE(compiled->outer().cwd);
  EXPECT_EQ(compiled->default_cwd(), "/tmp");
  EXPECT_FALSE(compiled->default_program().empty());
  EXPECT_EQ(compiled->history_file(), "/tmp/lemma-history");

  input::InputRouter router(compiled->input_map());
  constexpr std::array prefix{std::byte{0x01}};
  constexpr std::array split{std::byte{'s'}};
  EXPECT_TRUE(std::holds_alternative<input::ConsumedInput>(
      router.route_legacy(prefix, prefix.size()).effect));
  const auto routed = router.route_legacy(split, split.size());
  ASSERT_NE(std::get_if<input::RoutedCommand>(&routed.effect), nullptr);
  EXPECT_EQ(std::get<input::RoutedCommand>(routed.effect).command,
            input::InputCommand::split_left_right);
}

TEST(ConfigurationTest, RejectsMalformedOrUnpublishableDocuments) {
  const auto malformed = decode(R"({"schema":"lemma.config/v1"})");
  EXPECT_FALSE(malformed.configuration.has_value());

  Configuration invalid_prefix;
  ASSERT_TRUE(invalid_prefix.input.set_prefix(
      input::InputChord{.code = 300, .modifiers = 0, .kind = input::ChordKind::byte}));
  const auto compiled = compile(invalid_prefix);
  ASSERT_FALSE(compiled.has_value());
  EXPECT_EQ(compiled.error(), Error::input_map);

  Configuration relative_history;
  relative_history.history.file = "relative/history";
  const auto invalid_history = compile(relative_history);
  ASSERT_FALSE(invalid_history.has_value());
  EXPECT_EQ(invalid_history.error(), Error::invalid_field);
}

TEST(ConfigurationHostTest, LoadsLuaInASeparateResidentProcess) {
  TemporaryConfig file(R"(
local lemma = require("lemma")
assert(lemma.mode == nil)
for _, key in ipairs({ "Space", "Enter", "Tab", "Backspace", "Escape" }) do
  assert(lemma.keymap.send(key))
end
lemma.setup({
  input = { prefix = "C-a" },
  terminal = { scrollback_lines = 12345 },
  ui = { status_line = false, outer_title = false, outer_bell = false,
         outer_notifications = false, outer_progress = true, outer_cwd = false },
  launch = { default_cwd = "/tmp", default_program = { "/bin/sh", "-l" } },
  history = { file = "/tmp/lemma-history" },
})
lemma.context.set("resize", { label = " RESIZE ", lifetime = "persistent", unbound = "consume" })
lemma.keymap.set("prefix", "m", lemma.context.push("resize"))
lemma.keymap.set("resize", "q", lemma.context.pop())
lemma.keymap.set("normal", "Cmd-Left", lemma.keymap.send("Enter"))
lemma.keymap.set("prefix", "C-a", lemma.keymap.replay())
lemma.keymap.set("prefix", "s", "split_left_right")
lemma.keymap.del("prefix", "%")
)");
  ASSERT_TRUE(file.valid());

  auto loaded = extension::load_configuration(file.path());

  ASSERT_EQ(loaded.status, extension::ConfigurationStatus::loaded) << loaded.diagnostic;
  ASSERT_NE(loaded.generation, nullptr);
  EXPECT_TRUE(loaded.host.active());
  EXPECT_EQ(loaded.generation->scrollback_lines(), 12'345U);
  EXPECT_FALSE(loaded.generation->status_line());
  EXPECT_FALSE(loaded.generation->outer().title);
  EXPECT_FALSE(loaded.generation->outer().bell);
  EXPECT_FALSE(loaded.generation->outer().notifications);
  EXPECT_TRUE(loaded.generation->outer().progress);
  EXPECT_FALSE(loaded.generation->outer().cwd);
  EXPECT_EQ(loaded.generation->default_cwd(), "/tmp");
  EXPECT_EQ(loaded.generation->history_file(), "/tmp/lemma-history");
  input::InputRouter router(loaded.generation->input_map());
  constexpr std::array input_bytes{std::byte{0x01}, std::byte{'s'}};
  EXPECT_TRUE(std::holds_alternative<input::ConsumedInput>(
      router.route_legacy(input_bytes, input_bytes.size()).effect));
  const auto command = router.route_legacy(std::span(input_bytes).subspan(1), 1);
  ASSERT_NE(std::get_if<input::RoutedCommand>(&command.effect), nullptr);
  EXPECT_EQ(std::get<input::RoutedCommand>(command.effect).command,
            input::InputCommand::split_left_right);

  router.reset();
  const input::KeyEvent command_left{.action = input::KeyAction::press,
                                     .key = input::PhysicalKey::arrow_left,
                                     .modifiers = input::key_modifier_super,
                                     .unshifted_codepoint = 0,
                                     .text = {}};
  const auto rewritten = router.route_key(command_left);
  ASSERT_NE(std::get_if<input::EncodeAsKey>(&rewritten.effect), nullptr);
  EXPECT_EQ(std::get<input::EncodeAsKey>(rewritten.effect).key, input::PhysicalKey::enter);
}

TEST(ConfigurationHostTest, PublishesCommandMetadataWithTheConfigurationTransaction) {
  TemporaryConfig file(R"(
local lemma = require("lemma")
lemma.command.register("project.open", {
  description = "Open a project", timeout_ms = 1234,
  handler = function(ctx, args) error("not executed during registration") end,
})
)");
  ASSERT_TRUE(file.valid());
  auto loaded = extension::load_configuration(file.path());
  ASSERT_EQ(loaded.status, extension::ConfigurationStatus::loaded) << loaded.diagnostic;
  ASSERT_EQ(loaded.commands.size(), 2);
  EXPECT_EQ(loaded.commands.at(1).name, "project.open");
  EXPECT_EQ(loaded.commands.at(1).description, "Open a project");
  EXPECT_EQ(loaded.commands.at(1).timeout_ms, 1234);
}

TEST(ConfigurationHostTest, BindsBothCommandKindsToLegacyAndStructuredInput) {
  TemporaryConfig file(R"(
local lemma = require('lemma')
lemma.command.register('test.lua', {description='Lua', handler=function() end})
lemma.command.register('test.external', {description='External', argv={'cat', 'literal argument'}})
lemma.keymap.set('normal', 'l', 'test.lua')
lemma.keymap.set('normal', 'M-Left', 'test.external')
)");
  auto loaded = extension::load_configuration(file.path());
  ASSERT_EQ(loaded.status, extension::ConfigurationStatus::loaded) << loaded.diagnostic;
  ASSERT_EQ(loaded.commands.size(), 3U);
  input::InputRouter router(loaded.generation->input_map());
  constexpr std::array bytes{std::byte{'l'}};
  const auto legacy = router.route_legacy(bytes, bytes.size());
  ASSERT_TRUE(std::holds_alternative<input::RoutedHostedCommand>(legacy.effect));
  EXPECT_EQ(std::get<input::RoutedHostedCommand>(legacy.effect).index, 1U);
  input::KeyEvent key{.action = input::KeyAction::press,
                      .key = input::PhysicalKey::arrow_left,
                      .modifiers = input::key_modifier_alt,
                      .unshifted_codepoint = 0,
                      .text = {}};
  const auto structured = router.route_key(key);
  ASSERT_TRUE(std::holds_alternative<input::RoutedHostedCommand>(structured.effect));
  EXPECT_EQ(std::get<input::RoutedHostedCommand>(structured.effect).index, 2U);
  key.action = input::KeyAction::release;
  EXPECT_TRUE(std::holds_alternative<input::ConsumedInput>(router.route_key(key).effect));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ConfigurationHostTest, RejectsDuplicateOrInvalidCommandsWithoutPublishingConfiguration) {
  for (const auto* const declaration :
       {"lemma.command.register('pane', {description='bad', handler=function() end})",
        "lemma.command.register('test.bad', {description='bad', timeout_ms=0, handler=function() "
        "end})",
        "lemma.command.register('test.bad', {description='bad', handler=42})",
        "lemma.command.register('test.bad', {description='bad', argv={}})",
        "lemma.command.register('test.bad', {description='bad', argv={'cat', 42}})",
        "lemma.command.register('test.bad', {description='bad', argv={''}})",
        "lemma.command.register('test.bad', {description='bad', argv={'cat'}, handler=function() "
        "end})",
        "lemma.keymap.set('normal', 'x', 'test.undeclared')",
        "lemma.command.register('test.bad', {description='bad', extra=true, handler=function() "
        "end})",
        "for i=1,2 do lemma.command.register('test.bad', {description='bad', handler=function() "
        "end}) end"}) {
    TemporaryConfig file(
        std::string("local lemma = require('lemma')\nlemma.setup({input={prefix='C-a'}})\n") +
        declaration);
    ASSERT_TRUE(file.valid());
    const auto loaded = extension::load_configuration(file.path());
    EXPECT_EQ(loaded.status, extension::ConfigurationStatus::invalid) << declaration;
    EXPECT_EQ(loaded.generation, nullptr);
    EXPECT_FALSE(loaded.host.active());
  }
}

TEST(ConfigurationHostTest, RejectsTheWholeGenerationAfterALuaError) {
  TemporaryConfig file(R"(
local lemma = require("lemma")
lemma.setup({ unknown = {} })
)");
  ASSERT_TRUE(file.valid());

  auto loaded = extension::load_configuration(file.path());

  EXPECT_EQ(loaded.status, extension::ConfigurationStatus::invalid);
  EXPECT_EQ(loaded.generation, nullptr);
  EXPECT_FALSE(loaded.host.active());
  EXPECT_NE(loaded.diagnostic.find("unknown lemma.setup option"), std::string::npos);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ConfigurationHostTest, RejectsInvalidUiOptions) {
  for (const auto* const ui : {"{ outer_title = 'yes' }", "{ status_line = 1 }", "{ title = true }",
                               "{ outer_notifications = 'osc9' }", "{ outer_progress = 1 }",
                               "{ outer_cwd = 0 }", "{ outer_bell = 'visual' }"}) {
    TemporaryConfig file(std::string("require('lemma').setup({ ui = ") + ui + " })\n");
    ASSERT_TRUE(file.valid());
    const auto loaded = extension::load_configuration(file.path());
    EXPECT_EQ(loaded.status, extension::ConfigurationStatus::invalid) << ui;
    EXPECT_EQ(loaded.generation, nullptr) << ui;
  }
}

} // namespace
} // namespace lemma::config
