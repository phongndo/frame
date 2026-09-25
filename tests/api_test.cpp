#include "api/command.hpp"
#include "api/json.hpp"
#include "api/schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace lemma::api {
namespace {

// Assertions establish optional presence before the test examines decoded values.
// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST(ApiTest, EmbedsParseableVersionedSchema) {
  const auto schema = schema_document();
  ASSERT_FALSE(schema.empty());
  const auto parsed = parse_json(schema);
  ASSERT_TRUE(parsed.value.has_value()) << parsed.error_offset;
  EXPECT_EQ(json_string(*parsed.value, "$schema"), "https://json-schema.org/draft/2020-12/schema");
  EXPECT_EQ(json_string(*parsed.value, "$id"), "urn:lemma:schema:api:v1");
  const auto* const definitions = json_member(*parsed.value, "$defs");
  ASSERT_NE(definitions, nullptr);
  EXPECT_EQ(json_member(*definitions, "waitResult"), nullptr);
  EXPECT_EQ(json_member(*definitions, "action"), nullptr);
  EXPECT_EQ(json_member(*definitions, "actionResult"), nullptr);
  EXPECT_NE(json_member(*definitions, "proc"), nullptr);
  EXPECT_NE(json_member(*definitions, "command"), nullptr);
  EXPECT_NE(json_member(*definitions, "commandResult"), nullptr);
}

// GoogleTest assertions and nested schema traversal inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ApiTest, DefinesStructuredPaneListingGeometry) {
  const auto parsed = parse_json(schema_document());
  ASSERT_TRUE(parsed.value.has_value()) << parsed.error_offset;
  const auto* const definitions = json_member(*parsed.value, "$defs");
  ASSERT_NE(definitions, nullptr);
  const auto* const pane_listing = json_member(*definitions, "paneListing");
  ASSERT_NE(pane_listing, nullptr);
  const auto* const required = json_member(*pane_listing, "required");
  ASSERT_NE(required, nullptr);
  ASSERT_EQ(required->kind, JsonKind::array);
  const auto has_required = [required](const std::string_view field) {
    return std::ranges::any_of(required->array, [field](const JsonValue& entry) {
      return entry.kind == JsonKind::string && entry.string == field;
    });
  };
  EXPECT_TRUE(has_required("column"));
  EXPECT_TRUE(has_required("row"));
  EXPECT_TRUE(has_required("columns"));
  EXPECT_TRUE(has_required("rows"));

  const auto* const command_result = json_member(*definitions, "commandResult");
  ASSERT_NE(command_result, nullptr);
  const auto* const properties = json_member(*command_result, "properties");
  ASSERT_NE(properties, nullptr);
  const auto* const panes = json_member(*properties, "panes");
  ASSERT_NE(panes, nullptr);
  const auto* const items = json_member(*panes, "items");
  ASSERT_NE(items, nullptr);
  EXPECT_EQ(json_string(*items, "$ref"), std::optional<std::string_view>{"#/$defs/paneListing"});
}

TEST(ApiTest, DecodesConcreteCommandAndRejectsUnknownFields) {
  constexpr std::string_view valid = R"({
        "command":"pane.resize",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "direction":"left",
    "amount":3
  })";
  const auto document = parse_json(valid);
  ASSERT_TRUE(document.value.has_value());
  const auto decoded = decode_command(*document.value);
  ASSERT_TRUE(decoded.command.has_value()) << decoded.error.reason;
  EXPECT_EQ(decoded.command->kind, CommandKind::pane_resize);
  EXPECT_EQ(decoded.command->session.id, SessionId::from_parts(2, 7));
  EXPECT_EQ(decoded.command->pane.id, PaneId::from_parts(4, 9));
  EXPECT_EQ(decoded.command->amount, 3);
  const auto encoded = encode_command(*decoded.command);
  ASSERT_TRUE(encoded.has_value());
  const auto round_trip_document = parse_json(*encoded);
  ASSERT_TRUE(round_trip_document.value.has_value());
  const auto round_trip = decode_command(*round_trip_document.value);
  ASSERT_TRUE(round_trip.command.has_value());
  EXPECT_EQ(round_trip.command->kind, CommandKind::pane_resize);
  EXPECT_EQ(round_trip.command->session.id, SessionId::from_parts(2, 7));
  EXPECT_EQ(round_trip.command->pane.id, PaneId::from_parts(4, 9));

  constexpr std::string_view invalid = R"({"command":"session.list","typo":true})";
  const auto invalid_document = parse_json(invalid);
  ASSERT_TRUE(invalid_document.value.has_value());
  const auto rejected = decode_command(*invalid_document.value);
  EXPECT_FALSE(rejected.command.has_value());
  EXPECT_EQ(rejected.error.reason, "unknown_field");
  EXPECT_EQ(rejected.error.field, "typo");
}

TEST(ApiTest, DecodesBoundedWaitCommandAndRejectsConflictingConditions) {
  constexpr std::string_view valid = R"({
        "command":"pane.wait",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "contains":"ready",
    "after_generation":12,
    "timeout_ms":2000
  })";
  const auto document = parse_json(valid);
  ASSERT_TRUE(document.value.has_value());
  const auto decoded = decode_command(*document.value);
  ASSERT_TRUE(decoded.command.has_value()) << decoded.error.reason;
  EXPECT_EQ(decoded.command->kind, CommandKind::pane_wait);
  EXPECT_EQ(decoded.command->wait_condition, WaitCondition::contains);
  EXPECT_EQ(decoded.command->contains, "ready");
  EXPECT_EQ(decoded.command->after_terminal_generation, 12U);
  EXPECT_EQ(decoded.command->wait_timeout_milliseconds, 2000U);
  const auto encoded = encode_command(*decoded.command);
  ASSERT_TRUE(encoded.has_value());
  const auto round_trip_document = parse_json(*encoded);
  ASSERT_TRUE(round_trip_document.value.has_value());
  const auto round_trip = decode_command(*round_trip_document.value);
  ASSERT_TRUE(round_trip.command.has_value());
  EXPECT_EQ(round_trip.command->wait_condition, WaitCondition::contains);
  EXPECT_EQ(round_trip.command->contains, "ready");

  constexpr std::string_view conflicting = R"({
        "command":"pane.wait",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "exit_code":0,
    "until_prompt":true
  })";
  const auto conflicting_document = parse_json(conflicting);
  ASSERT_TRUE(conflicting_document.value.has_value());
  const auto rejected = decode_command(*conflicting_document.value);
  EXPECT_FALSE(rejected.command.has_value());
  EXPECT_EQ(rejected.error.reason, "conflicting_fields");

  constexpr std::string_view invalid_generation = R"({
        "command":"pane.wait",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "after_generation":12
  })";
  const auto generation_document = parse_json(invalid_generation);
  ASSERT_TRUE(generation_document.value.has_value());
  EXPECT_FALSE(decode_command(*generation_document.value).command.has_value());
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ApiTest, DecodesCommandCompletionWait) {
  constexpr std::string_view valid = R"({
    "command":"pane.wait",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "until_command":true,
    "after_commands":3
  })";
  const auto document = parse_json(valid);
  ASSERT_TRUE(document.value.has_value());
  const auto decoded = decode_command(*document.value);
  ASSERT_TRUE(decoded.command.has_value()) << decoded.error.reason;
  EXPECT_EQ(decoded.command->wait_condition, WaitCondition::command);
  EXPECT_EQ(decoded.command->after_commands, std::optional<std::uint64_t>{3});
  EXPECT_EQ(wait_condition_name(WaitCondition::command), "command");
  const auto encoded = encode_command(*decoded.command);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_NE(encoded->find(R"("until_command":true,"after_commands":3)"), std::string::npos);

  for (const std::string_view invalid : {
           R"({"command":"pane.wait","session":{"id":"2:7"},"pane":{"id":"4:9"},
               "until_command":true,"until_prompt":true})",
           R"({"command":"pane.wait","session":{"id":"2:7"},"pane":{"id":"4:9"},
               "until_command":false})",
           R"({"command":"pane.wait","session":{"id":"2:7"},"pane":{"id":"4:9"},
               "after_commands":1})",
           R"({"command":"pane.wait","session":{"id":"2:7"},"pane":{"id":"4:9"},
               "until_command":true,"after_generation":1})",
       }) {
    const auto rejected_document = parse_json(invalid);
    ASSERT_TRUE(rejected_document.value.has_value());
    EXPECT_FALSE(decode_command(*rejected_document.value).command.has_value()) << invalid;
  }
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ApiTest, DecodesSignalObservationForGlobalAndScopedFeeds) {
  for (const std::string_view valid : {
           R"({"schema":"lemma.events/v1","signals":true})",
           R"({"schema":"lemma.events/v1","session":{"name":"work"},"signals":true})",
       }) {
    const auto document = parse_json(valid);
    ASSERT_TRUE(document.value.has_value());
    const auto decoded = decode_event_subscription(*document.value);
    ASSERT_TRUE(decoded.subscription.has_value()) << decoded.error.reason;
    EXPECT_TRUE(decoded.subscription->signals);
  }
  const auto invalid = parse_json(R"({"schema":"lemma.events/v1","signals":1})");
  ASSERT_TRUE(invalid.value.has_value());
  const auto rejected = decode_event_subscription(*invalid.value);
  EXPECT_FALSE(rejected.subscription.has_value());
  EXPECT_EQ(rejected.error.field, "signals");
}

TEST(ApiTest, DecodesBoundedMultiPaneObservation) {
  constexpr std::string_view valid = R"({
    "schema":"lemma.events/v1",
    "session":{"id":"0:1"},
    "panes":[{"id":"1:1"},{"id":"2:1"}],
    "screen":true
  })";
  const auto document = parse_json(valid);
  ASSERT_TRUE(document.value.has_value());
  const auto decoded = decode_event_subscription(*document.value);
  ASSERT_TRUE(decoded.subscription.has_value()) << decoded.error.reason;
  ASSERT_EQ(decoded.subscription->panes.size(), 2U);
  EXPECT_EQ(decoded.subscription->panes.at(1).id, PaneId::from_parts(2, 1));

  constexpr std::string_view duplicate = R"({
    "schema":"lemma.events/v1",
    "session":{"id":"0:1"},
    "panes":[{"id":"1:1"},{"id":"1:1"}],
    "screen":true
  })";
  const auto duplicate_document = parse_json(duplicate);
  ASSERT_TRUE(duplicate_document.value.has_value());
  EXPECT_FALSE(decode_event_subscription(*duplicate_document.value).subscription.has_value());
}

TEST(ApiTest, RejectsAgentInputBatchCapacityOverflow) {
  std::string input =
      R"({"command":"pane.input","session":{"id":"0:1"},"pane":{"id":"0:1"},"events":[)";
  for (std::size_t index = 0; index <= input_events_max; ++index) {
    input += index == 0 ? R"({"kind":"key","key":"a"})" : R"(,{"kind":"key","key":"a"})";
  }
  input += "]}";
  const auto input_document = parse_json(input);
  ASSERT_TRUE(input_document.value.has_value());
  EXPECT_FALSE(decode_command(*input_document.value).command.has_value());
}

TEST(ApiTest, RejectsObservationPaneCapacityOverflow) {
  std::string subscription = R"({"schema":"lemma.events/v1","session":{"id":"0:1"},"panes":[)";
  for (std::size_t index = 0; index <= event_panes_max; ++index) {
    if (index > 0) {
      subscription += ',';
    }
    subscription += R"({"id":")" + std::to_string(index) + R"(:1"})";
  }
  subscription += "]}";
  const auto subscription_document = parse_json(subscription);
  ASSERT_TRUE(subscription_document.value.has_value());
  EXPECT_FALSE(decode_event_subscription(*subscription_document.value).subscription.has_value());
}

TEST(ApiTest, RequiresPaneFilterForScreenObservation) {
  constexpr std::string_view invalid = R"({"schema":"lemma.events/v1","screen":true})";
  const auto document = parse_json(invalid);
  ASSERT_TRUE(document.value.has_value());
  const auto rejected = decode_event_subscription(*document.value);
  EXPECT_FALSE(rejected.subscription.has_value());
  EXPECT_EQ(rejected.error.field, "pane");
}

TEST(ApiTest, DecodesIntrospectionInputCaptureAndFocusPolicies) {
  constexpr std::string_view input = R"({
        "command":"pane.input",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "events":[
      {"kind":"paste","text":"just test"},
      {"kind":"key","key":"c","modifiers":["control"],"phase":"repeat"},
      {"kind":"key","key":"enter"}
    ]
  })";
  const auto input_document = parse_json(input);
  ASSERT_TRUE(input_document.value.has_value());
  const auto decoded_input = decode_command(*input_document.value);
  ASSERT_TRUE(decoded_input.command.has_value()) << decoded_input.error.reason;
  ASSERT_EQ(decoded_input.command->input_events.size(), 3U);
  EXPECT_EQ(decoded_input.command->input_events.at(0).kind, InputEventKind::paste);
  EXPECT_EQ(decoded_input.command->input_events.at(1).modifiers, input_modifier_control);
  EXPECT_EQ(decoded_input.command->input_events.at(1).phase, InputKeyPhase::repeat);
  EXPECT_EQ(decoded_input.command->input_events.at(2).key, InputKey::enter);
  const auto encoded_input = encode_command(*decoded_input.command);
  ASSERT_TRUE(encoded_input.has_value());
  const auto round_trip_document = parse_json(*encoded_input);
  ASSERT_TRUE(round_trip_document.value.has_value());
  const auto round_trip = decode_command(*round_trip_document.value);
  ASSERT_TRUE(round_trip.command.has_value());
  EXPECT_EQ(round_trip.command->input_events.size(), 3U);

  constexpr std::string_view legacy_key_action = R"({
    "command":"pane.input",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "events":[{"kind":"key","key":"enter","action":"release"}]
  })";
  const auto legacy_key_action_document = parse_json(legacy_key_action);
  ASSERT_TRUE(legacy_key_action_document.value.has_value());
  EXPECT_FALSE(decode_command(*legacy_key_action_document.value).command.has_value());

  constexpr std::string_view split = R"({
        "command":"pane.split",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "direction":"right",
    "focus":"preserve",
    "if_session_revision":12
  })";
  const auto split_document = parse_json(split);
  ASSERT_TRUE(split_document.value.has_value());
  const auto decoded_split = decode_command(*split_document.value);
  ASSERT_TRUE(decoded_split.command.has_value()) << decoded_split.error.reason;
  EXPECT_EQ(decoded_split.command->focus, FocusPolicy::preserve);
  EXPECT_EQ(decoded_split.command->expected_session_revision, 12U);

  constexpr std::string_view capture = R"({
        "command":"pane.capture",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "source":"recent",
    "format":"ansi",
    "wrap":"logical",
    "lines":120
  })";
  const auto capture_document = parse_json(capture);
  ASSERT_TRUE(capture_document.value.has_value());
  const auto decoded_capture = decode_command(*capture_document.value);
  ASSERT_TRUE(decoded_capture.command.has_value()) << decoded_capture.error.reason;
  EXPECT_EQ(decoded_capture.command->capture_source, CaptureSource::recent);
  EXPECT_EQ(decoded_capture.command->capture_format, CaptureFormat::ansi);
  EXPECT_EQ(decoded_capture.command->capture_wrap, CaptureWrap::logical);

  const auto invalid_capture = parse_json(R"({
        "command":"pane.capture",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "source":7
  })");
  ASSERT_TRUE(invalid_capture.value.has_value());
  const auto rejected_capture = decode_command(*invalid_capture.value);
  EXPECT_FALSE(rejected_capture.command.has_value());
  EXPECT_EQ(rejected_capture.error.field, "source");

  const auto ignored_lines = parse_json(R"({
        "command":"pane.capture",
    "session":{"id":"2:7"},
    "pane":{"id":"4:9"},
    "source":"last-command",
    "lines":10
  })");
  ASSERT_TRUE(ignored_lines.value.has_value());
  const auto rejected_lines = decode_command(*ignored_lines.value);
  EXPECT_FALSE(rejected_lines.command.has_value());
  EXPECT_EQ(rejected_lines.error.field, "lines");

  const auto daemon_document = parse_json(R"({"command":"daemon.inspect"})");
  ASSERT_TRUE(daemon_document.value.has_value());
  const auto daemon = decode_command(*daemon_document.value);
  ASSERT_TRUE(daemon.command.has_value());
  EXPECT_EQ(daemon.command->kind, CommandKind::daemon_inspect);
}

// GTest assertions expand into branches; cases cover the closed selector grammar.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ApiTest, DecodesAndEncodesAttachmentSwitch) {
  const auto document = parse_json(R"({"command":"attachment.switch","connection":"2:7",
    "session":{"id":"3:8"},"if_session_revision":9})");
  ASSERT_TRUE(document.value.has_value());
  const auto decoded = decode_command(*document.value);
  ASSERT_TRUE(decoded.command.has_value()) << decoded.error.reason;
  EXPECT_EQ(decoded.command->kind, CommandKind::attachment_switch);
  EXPECT_EQ(decoded.command->connection, ConnectionId::from_parts(2, 7));
  const auto encoded = encode_command(*decoded.command);
  ASSERT_TRUE(encoded.has_value());
  const auto parsed = parse_json(*encoded);
  ASSERT_TRUE(parsed.value.has_value());
  const auto round_trip = decode_command(*parsed.value);
  ASSERT_TRUE(round_trip.command.has_value());
  EXPECT_EQ(round_trip.command->connection, decoded.command->connection);
  EXPECT_EQ(round_trip.command->session.id, decoded.command->session.id);
  for (
      const auto* const invalid :
      {R"({"command":"attachment.switch","session":{"name":"target"}})",
       R"({"command":"attachment.switch","connection":"0:0","session":{"name":"target"}})",
       R"({"command":"attachment.switch","connection":"0:1"})",
       R"({"command":"attachment.switch","connection":"0:1","session":{"name":"target"},"steal":true})"}) {
    const auto value = parse_json(invalid);
    ASSERT_TRUE(value.value.has_value());
    EXPECT_FALSE(decode_command(*value.value).command.has_value()) << invalid;
  }
}

TEST(ApiTest, DecodesAndEncodesSurfaceLifecycleCommands) {
  const auto create_document = parse_json(R"({
    "command":"surface.create",
    "placement":{"kind":"dock.right","size":24},
    "focusable":false,
    "opaque":true
  })");
  ASSERT_TRUE(create_document.value.has_value());
  const auto create = decode_command(*create_document.value);
  ASSERT_TRUE(create.command.has_value()) << create.error.reason;
  EXPECT_EQ(create.command->kind, CommandKind::surface_create);
  EXPECT_EQ(create.command->surface_placement.kind, SurfacePlacementKind::dock_right);
  EXPECT_EQ(create.command->surface_placement.columns, 24U);
  EXPECT_FALSE(create.command->focusable);
  const auto encoded = encode_command(*create.command);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_NE(encoded->find(R"("kind":"dock.right")"), std::string::npos);

  const auto configure_document = parse_json(R"({
    "command":"surface.configure",
    "surface":{"id":"3:9"},
    "placement":{"kind":"overlay","column":2,"row":3,"columns":10,"rows":4}
  })");
  ASSERT_TRUE(configure_document.value.has_value());
  const auto configure = decode_command(*configure_document.value);
  ASSERT_TRUE(configure.command.has_value()) << configure.error.reason;
  EXPECT_EQ(configure.command->kind, CommandKind::surface_configure);
  EXPECT_EQ(configure.command->surface.id, SurfaceId::from_parts(3, 9));
  EXPECT_EQ(configure.command->surface_placement.kind, SurfacePlacementKind::overlay);
  EXPECT_EQ(configure.command->surface_placement.rows, 4U);

  const auto invalid =
      parse_json(R"({"command":"surface.create","placement":{"kind":"dock.left","size":0}})");
  ASSERT_TRUE(invalid.value.has_value());
  EXPECT_FALSE(decode_command(*invalid.value).command.has_value());
}

TEST(ApiTest, RejectsExplicitZeroCaptureLines) {
  constexpr std::string_view invalid =
      R"({"command":"pane.capture","session":{"id":"0:1"},"pane":{"id":"0:1"},"lines":0})";
  const auto document = parse_json(invalid);
  ASSERT_TRUE(document.value.has_value());
  const auto rejected = decode_command(*document.value);
  EXPECT_FALSE(rejected.command.has_value());
  EXPECT_EQ(rejected.error.field, "lines");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ApiTest, EnforcesJsonDepthNodeAndDuplicateMemberBounds) {
  std::string maximum_depth(32, '[');
  maximum_depth += "null";
  maximum_depth.append(32, ']');
  EXPECT_TRUE(parse_json(maximum_depth).value.has_value());

  std::string excessive_depth(33, '[');
  excessive_depth += "null";
  excessive_depth.append(33, ']');
  EXPECT_FALSE(parse_json(excessive_depth).value.has_value());

  std::string maximum_nodes{"["};
  for (std::size_t index = 0; index < json_nodes_max - 1U; ++index) {
    if (index > 0) {
      maximum_nodes += ',';
    }
    maximum_nodes += "null";
  }
  maximum_nodes += ']';
  EXPECT_TRUE(parse_json(maximum_nodes).value.has_value());
  maximum_nodes.insert(maximum_nodes.size() - 1U, ",null");
  EXPECT_FALSE(parse_json(maximum_nodes).value.has_value());

  EXPECT_FALSE(parse_json(R"({"same":1,"same":2})").value.has_value());
}

TEST(ApiTest, RejectsNonIntegerNumbersAndInvalidUnicode) {
  EXPECT_FALSE(parse_json("1.0").value.has_value());
  EXPECT_FALSE(parse_json("1e2").value.has_value());
  EXPECT_FALSE(parse_json(R"("\uD800")").value.has_value());
  EXPECT_FALSE(parse_json(std::string{"\"\xC0\xAF\"", 4}).value.has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace
} // namespace lemma::api
