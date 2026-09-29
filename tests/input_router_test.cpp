#include "input/input_router.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <variant>

namespace frame::input {
namespace {

TEST(InputMapTest, RejectsDuplicateBindingsBeforePublication) {
  InputMapDraft draft;
  const auto normal = draft.add_context({});
  ASSERT_TRUE(normal.has_value());
  ASSERT_TRUE(draft.bind(*normal, InputChord::byte('x'), invoke(InputCommand::close_pane)));
  ASSERT_TRUE(draft.bind(*normal, InputChord::byte('x'), invoke(InputCommand::toggle_zoom)));

  const auto compiled = draft.compile();

  ASSERT_FALSE(compiled.has_value());
  EXPECT_EQ(compiled.error(), InputMapError::duplicate_binding);
}

TEST(InputMapTest, RejectsUnresolvedContextTransitionsBeforePublication) {
  InputMapDraft draft;
  const auto normal = draft.add_context({});
  ASSERT_TRUE(normal.has_value());
  ASSERT_TRUE(draft.bind(*normal, InputChord::byte('m'), PushContextBinding{}));

  const auto compiled = draft.compile();

  ASSERT_FALSE(compiled.has_value());
  EXPECT_EQ(compiled.error(), InputMapError::invalid_context);
}

TEST(InputMapTest, RejectsInvalidCommandDiscriminantsBeforePublication) {
  InputMapDraft draft;
  const auto normal = draft.add_context({});
  ASSERT_TRUE(normal.has_value());
  ASSERT_TRUE(draft.bind(*normal, InputChord::byte('x'),
                         CommandBinding{.command = InputCommand::count,
                                        .context = CommandContextDisposition::retain}));

  const auto compiled = draft.compile();

  ASSERT_FALSE(compiled.has_value());
  EXPECT_EQ(compiled.error(), InputMapError::invalid_action);
}

TEST(InputMapTest, RejectsInvalidEncodeAsTargetsBeforePublication) {
  InputMapDraft draft;
  const auto normal = draft.add_context({});
  ASSERT_TRUE(normal.has_value());
  ASSERT_TRUE(draft.bind(*normal, InputChord::key(PhysicalKey::arrow_left, key_modifier_super),
                         encode_as(PhysicalKey::unidentified)));

  const auto compiled = draft.compile();

  ASSERT_FALSE(compiled.has_value());
  EXPECT_EQ(compiled.error(), InputMapError::invalid_action);
}

TEST(InputMapTest, RejectsEncodeAsBindingsThatCollideWithLegacyMatching) {
  InputMapDraft byte_draft;
  const auto byte_context = byte_draft.add_context({});
  ASSERT_TRUE(byte_context.has_value());
  ASSERT_TRUE(byte_draft.bind(*byte_context, InputChord::byte('h', key_modifier_super),
                              encode_as(PhysicalKey::home)));
  const auto byte_compiled = byte_draft.compile();
  ASSERT_FALSE(byte_compiled.has_value());
  EXPECT_EQ(byte_compiled.error(), InputMapError::invalid_action);

  InputMapDraft unmodified;
  const auto unmodified_context = unmodified.add_context({});
  ASSERT_TRUE(unmodified_context.has_value());
  ASSERT_TRUE(unmodified.bind(*unmodified_context, InputChord::key(PhysicalKey::arrow_left),
                              encode_as(PhysicalKey::home)));
  const auto unmodified_compiled = unmodified.compile();
  ASSERT_FALSE(unmodified_compiled.has_value());
  EXPECT_EQ(unmodified_compiled.error(), InputMapError::invalid_action);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(InputMapTest, RejectsContextGraphsThatCannotFitTheRuntimeStack) {
  InputMapDraft draft;
  std::array<InputContextId, input_context_stack_max + 1U> contexts{};
  for (auto& stored : contexts) {
    const auto context = draft.add_context({});
    ASSERT_TRUE(context.has_value());
    stored = *context;
  }
  for (std::size_t index = 1; index < contexts.size(); ++index) {
    const auto pushed = push_context(contexts.at(index));
    ASSERT_TRUE(pushed.has_value());
    ASSERT_TRUE(draft.bind(contexts.at(index - 1U), InputChord::byte('a'), *pushed));
  }

  const auto compiled = draft.compile();

  ASSERT_FALSE(compiled.has_value());
  EXPECT_EQ(compiled.error(), InputMapError::context_depth);
}

TEST(InputMapTest, RequiresAValidInvariantPreservingBaseContext) {
  InputMapDraft empty;
  EXPECT_EQ(empty.compile().error(), InputMapError::missing_base);

  InputMapDraft invalid_base;
  const auto context = invalid_base.add_context({.label = {},
                                                 .lifetime = ContextLifetime::one_shot,
                                                 .unbound = UnboundBehavior::replay_deferred,
                                                 .preempts_interaction = false});
  ASSERT_FALSE(context.has_value());
  EXPECT_EQ(context.error(), InputMapError::invalid_options);

  InputMapDraft invalid_label;
  const auto control_label = invalid_label.add_context({.label = "\n"});
  ASSERT_FALSE(control_label.has_value());
  EXPECT_EQ(control_label.error(), InputMapError::invalid_options);
}

TEST(InputRouterTest, PreservesUnboundRunsAndRoutesOneShotPrefixCommands) {
  InputRouter router(default_input_map());
  const std::array input{std::byte{'a'}, std::byte{'b'}, std::byte{0x02}, std::byte{'h'}};

  EXPECT_FALSE(router.legacy_route_requires_checkpoint());
  const auto ordinary = router.route_legacy(input, input.size());
  ASSERT_EQ(ordinary.consumed, 2U);
  const auto* const forwarded = std::get_if<ForwardLegacyInput>(&ordinary.effect);
  ASSERT_NE(forwarded, nullptr);
  EXPECT_TRUE(forwarded->prefix_size == 0U && forwarded->current.size() == 2U);

  const auto prefix = router.route_legacy(std::span(input).subspan(2), 2);
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(prefix.effect));
  EXPECT_EQ(prefix.consumed, 1U);
  EXPECT_TRUE(router.active_label().empty());
  EXPECT_TRUE(router.legacy_route_requires_checkpoint());

  const auto command = router.route_legacy(std::span(input).subspan(3), 1);
  const auto* const routed = std::get_if<RoutedCommand>(&command.effect);
  ASSERT_NE(routed, nullptr);
  EXPECT_EQ(routed->command, InputCommand::focus_left);
  EXPECT_TRUE(router.active_label().empty());
  EXPECT_FALSE(router.legacy_route_requires_checkpoint());
}

TEST(InputRouterTest, DefaultPrefixEntersCommandLineAndItsEditorOwnsCompletionAndHistory) {
  InputRouter router(default_input_map());
  constexpr std::array enter{std::byte{0x02}, std::byte{':'}};
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_legacy(enter, 2).effect));
  const auto entered = router.route_legacy(std::span(enter).subspan(1), 1);
  const auto* const begin = std::get_if<RoutedCommand>(&entered.effect);
  ASSERT_NE(begin, nullptr);
  EXPECT_EQ(begin->command, InputCommand::begin_command_line);

  router.select_base(ConfiguredInputContext::command_line);
  constexpr std::array tab{std::byte{'\t'}};
  const auto completed = router.route_legacy(tab, 1);
  ASSERT_NE(std::get_if<RoutedCommand>(&completed.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(completed.effect).command, InputCommand::command_line_complete);

  const auto up = router.route_key({.action = KeyAction::press,
                                    .key = PhysicalKey::arrow_up,
                                    .modifiers = 0,
                                    .unshifted_codepoint = 0,
                                    .text = {}});
  ASSERT_NE(std::get_if<RoutedCommand>(&up.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(up.effect).command,
            InputCommand::command_line_history_previous);
}

TEST(InputRouterTest, DefaultPrefixOpensTheBoundedMessageViewer) {
  InputRouter router(default_input_map());
  constexpr std::array enter{std::byte{0x02}, std::byte{'~'}};
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_legacy(enter, 2).effect));
  const auto entered = router.route_legacy(std::span(enter).subspan(1), 1);
  const auto* const begin = std::get_if<RoutedCommand>(&entered.effect);
  ASSERT_NE(begin, nullptr);
  EXPECT_EQ(begin->command, InputCommand::show_messages);

  router.select_base(ConfiguredInputContext::messages);
  constexpr std::array up{std::byte{'k'}};
  const auto previous = router.route_legacy(up, 1);
  ASSERT_NE(std::get_if<RoutedCommand>(&previous.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(previous.effect).command, InputCommand::message_view_previous);
}

TEST(InputRouterTest, ReplaysAnUnboundPrefixWithoutLosingInput) {
  InputRouter router(default_input_map());
  constexpr std::array prefix{std::byte{0x02}};
  constexpr std::array unbound{std::byte{'!'}};
  ASSERT_TRUE(std::holds_alternative<ConsumedInput>(router.route_legacy(prefix, 1).effect));

  const auto result = router.route_legacy(unbound, 1);

  const auto* const forwarded = std::get_if<ForwardLegacyInput>(&result.effect);
  ASSERT_NE(forwarded, nullptr);
  ASSERT_EQ(forwarded->prefix_size, 1U);
  EXPECT_EQ(forwarded->prefix.front(), std::byte{0x02});
  ASSERT_EQ(forwarded->current.size(), 1U);
  EXPECT_EQ(forwarded->current.front(), std::byte{'!'});
  EXPECT_TRUE(router.active_label().empty());
}

TEST(InputRouterTest, StopsLegacyBatchBeforeAResolvableEscapeChord) {
  InputMapDraft draft;
  const auto normal = draft.add_context({});
  ASSERT_TRUE(normal.has_value());
  ASSERT_TRUE(draft.bind(*normal, InputChord::key(PhysicalKey::arrow_left),
                         invoke(InputCommand::resize_left)));
  const auto map = draft.compile();
  ASSERT_TRUE(map.has_value());
  InputRouter router(*map);
  constexpr std::array input{std::byte{'x'}, std::byte{0x1B}, std::byte{'['}, std::byte{'D'}};

  const auto ordinary = router.route_legacy(input, input.size());
  ASSERT_NE(std::get_if<ForwardLegacyInput>(&ordinary.effect), nullptr);
  EXPECT_EQ(ordinary.consumed, 1U);

  const auto arrow = router.route_legacy(std::span(input).subspan(1), input.size() - 1U);
  ASSERT_NE(std::get_if<RoutedCommand>(&arrow.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(arrow.effect).command, InputCommand::resize_left);
  EXPECT_EQ(arrow.consumed, 3U);
}

TEST(InputRouterTest, OneShotForwardContextReturnsToItsParentBeforeTheNextByte) {
  InputMapDraft draft;
  const auto normal = draft.add_context({});
  const auto one_shot = draft.add_context({.label = " ONCE ",
                                           .lifetime = ContextLifetime::one_shot,
                                           .unbound = UnboundBehavior::forward,
                                           .preempts_interaction = false});
  ASSERT_TRUE(normal.has_value() && one_shot.has_value());
  const auto pushed = push_context(*one_shot);
  ASSERT_TRUE(pushed.has_value());
  ASSERT_TRUE(draft.bind(*normal, InputChord::byte('x'), *pushed));
  const auto map = draft.compile();
  ASSERT_TRUE(map.has_value());
  InputRouter router(*map);
  constexpr std::array enter_and_text{std::byte{'x'}, std::byte{'a'}, std::byte{'b'}};

  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(
      router.route_legacy(enter_and_text, enter_and_text.size()).effect));
  const auto forwarded =
      router.route_legacy(std::span(enter_and_text).subspan(1), enter_and_text.size() - 1U);

  ASSERT_NE(std::get_if<ForwardLegacyInput>(&forwarded.effect), nullptr);
  EXPECT_EQ(forwarded.consumed, 1U);
  EXPECT_TRUE(forwarded.presentation_changed);
  EXPECT_TRUE(router.active_label().empty());
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(InputRouterTest, TransientResizeContextRepeatsAndExitsWithoutCoreModeState) {
  InputRouter router(default_input_map());
  constexpr std::array enter{std::byte{0x02}, std::byte{'m'}};
  ASSERT_TRUE(std::holds_alternative<ConsumedInput>(router.route_legacy(enter, 2).effect));
  const auto entered = router.route_legacy(std::span(enter).subspan(1), 1);
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(entered.effect));
  EXPECT_TRUE(entered.presentation_changed);
  EXPECT_TRUE(entered.interaction_preemption_requested);
  EXPECT_EQ(router.active_label(), "RESIZE");
  EXPECT_FALSE(router.legacy_route_requires_checkpoint());

  constexpr std::array directions{std::byte{'h'}, std::byte{'h'}, std::byte{'j'}};
  for (std::size_t index = 0; index < directions.size(); ++index) {
    const auto routed = router.route_legacy(std::span(directions).subspan(index), 1);
    const auto* const command = std::get_if<RoutedCommand>(&routed.effect);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->command, index < 2U ? InputCommand::resize_left : InputCommand::resize_down);
    EXPECT_EQ(router.active_label(), "RESIZE");
  }

  constexpr std::array legacy_arrow{std::byte{0x1B}, std::byte{'['}, std::byte{'D'}};
  const auto arrow_command = router.route_legacy(legacy_arrow, legacy_arrow.size());
  ASSERT_NE(std::get_if<RoutedCommand>(&arrow_command.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(arrow_command.effect).command, InputCommand::resize_left);
  EXPECT_EQ(arrow_command.consumed, legacy_arrow.size());

  constexpr std::array nested_prefix{std::byte{0x02}, std::byte{'z'}};
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_legacy(nested_prefix, 2).effect));
  const auto nested_command = router.route_legacy(std::span(nested_prefix).subspan(1), 1);
  ASSERT_NE(std::get_if<RoutedCommand>(&nested_command.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(nested_command.effect).command, InputCommand::toggle_zoom);
  EXPECT_EQ(router.active_label(), "RESIZE");

  constexpr std::array rename{std::byte{0x02}, std::byte{'r'}};
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_legacy(rename, 2).effect));
  const auto rename_command = router.route_legacy(std::span(rename).subspan(1), 1);
  ASSERT_NE(std::get_if<RoutedCommand>(&rename_command.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(rename_command.effect).command, InputCommand::begin_rename_tab);
  EXPECT_TRUE(router.active_label().empty());

  ASSERT_TRUE(std::holds_alternative<ConsumedInput>(router.route_legacy(enter, 2).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(
      router.route_legacy(std::span(enter).subspan(1), 1).effect));
  EXPECT_EQ(router.active_label(), "RESIZE");

  constexpr std::array leave{std::byte{'q'}};
  const auto exited = router.route_legacy(leave, 1);
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(exited.effect));
  EXPECT_TRUE(exited.presentation_changed);
  EXPECT_TRUE(router.active_label().empty());
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(InputRouterTest, TypedKeyOwnershipSurvivesContextTransitionsAndRepeats) {
  InputRouter router(default_input_map());
  const auto key = [](const KeyAction action, const PhysicalKey physical,
                      const std::uint16_t modifiers, const std::uint32_t codepoint) {
    return KeyEvent{.action = action,
                    .key = physical,
                    .modifiers = modifiers,
                    .unshifted_codepoint = codepoint,
                    .text = {}};
  };
  const auto prefix_press = key(KeyAction::press, PhysicalKey::b, key_modifier_control, 'b');
  const auto prefix_release = key(KeyAction::release, PhysicalKey::b, key_modifier_control, 'b');
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(prefix_press).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(prefix_release).effect));

  const auto unbound_press = key(KeyAction::press, PhysicalKey::e, 0, 'e');
  const auto unbound_release = key(KeyAction::release, PhysicalKey::e, 0, 'e');
  EXPECT_TRUE(
      std::holds_alternative<ForwardBytesThenCurrentKey>(router.route_key(unbound_press).effect));
  EXPECT_TRUE(std::holds_alternative<ForwardCurrentKey>(router.route_key(unbound_release).effect));

  const auto held_press = key(KeyAction::press, PhysicalKey::h, 0, 'h');
  const auto held_repeat = key(KeyAction::repeat, PhysicalKey::h, 0, 'h');
  const auto held_release = key(KeyAction::release, PhysicalKey::h, 0, 'h');
  EXPECT_TRUE(std::holds_alternative<ForwardCurrentKey>(router.route_key(held_press).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(prefix_press).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(prefix_release).effect));
  const auto resize_press = key(KeyAction::press, PhysicalKey::m, 0, 'm');
  const auto resize_release = key(KeyAction::release, PhysicalKey::m, 0, 'm');
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(resize_press).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(resize_release).effect));
  EXPECT_TRUE(std::holds_alternative<ForwardCurrentKey>(router.route_key(held_repeat).effect));
  EXPECT_TRUE(std::holds_alternative<ForwardCurrentKey>(router.route_key(held_release).effect));

  const auto resize_left_press = key(KeyAction::press, PhysicalKey::h, 0, 'h');
  const auto resize_left_repeat = key(KeyAction::repeat, PhysicalKey::h, 0, 'h');
  const auto resize_left_release = key(KeyAction::release, PhysicalKey::h, 0, 'h');
  EXPECT_EQ(std::get<RoutedCommand>(router.route_key(resize_left_press).effect).command,
            InputCommand::resize_left);
  EXPECT_EQ(std::get<RoutedCommand>(router.route_key(resize_left_repeat).effect).command,
            InputCommand::resize_left);
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(resize_left_release).effect));

  const auto exit_press = key(KeyAction::press, PhysicalKey::q, 0, 'q');
  const auto exit_repeat = key(KeyAction::repeat, PhysicalKey::q, 0, 'q');
  const auto exit_release = key(KeyAction::release, PhysicalKey::q, 0, 'q');
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(exit_press).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(exit_repeat).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(exit_release).effect));
}

TEST(InputRouterTest, PreservesShiftedControlAndAltLegacyBindingsForTypedKeys) {
  InputRouter router(default_input_map());
  const KeyEvent prefix{.action = KeyAction::press,
                        .key = PhysicalKey::b,
                        .modifiers = key_modifier_control | key_modifier_shift,
                        .unshifted_codepoint = 'b',
                        .text = {}};
  const KeyEvent resize{.action = KeyAction::press,
                        .key = PhysicalKey::h,
                        .modifiers = key_modifier_alt | key_modifier_shift,
                        .unshifted_codepoint = 'h',
                        .text = {}};

  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(prefix).effect));
  const auto routed = router.route_key(resize);

  ASSERT_NE(std::get_if<RoutedCommand>(&routed.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(routed.effect).command, InputCommand::resize_left);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(InputRouterTest, RoutesHostCopyChordsFromTheBaseContext) {
  InputRouter router(default_input_map());
  const KeyEvent super{.action = KeyAction::press,
                       .key = PhysicalKey::c,
                       .modifiers = key_modifier_super,
                       .unshifted_codepoint = 'c',
                       .text = {}};
  const KeyEvent shift_control{.action = KeyAction::press,
                               .key = PhysicalKey::c,
                               .modifiers = key_modifier_control | key_modifier_shift,
                               .unshifted_codepoint = 'c',
                               .text = {}};

  const auto super_routed = router.route_key(super);
  const auto shift_control_routed = router.route_key(shift_control);

  ASSERT_NE(std::get_if<RoutedCommand>(&super_routed.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(super_routed.effect).command, InputCommand::copy_selection);
  ASSERT_NE(std::get_if<RoutedCommand>(&shift_control_routed.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(shift_control_routed.effect).command,
            InputCommand::copy_selection);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(InputRouterTest, DefaultMapEncodesSuperArrowsAsHomeAndEnd) {
  InputRouter router(default_input_map());
  const KeyEvent left{.action = KeyAction::press,
                      .key = PhysicalKey::arrow_left,
                      .modifiers = key_modifier_super,
                      .unshifted_codepoint = 0,
                      .text = {}};
  const KeyEvent right{.action = KeyAction::press,
                       .key = PhysicalKey::arrow_right,
                       .modifiers = key_modifier_super | key_modifier_shift,
                       .unshifted_codepoint = 0,
                       .text = {}};

  const auto left_routed = router.route_key(left);
  const auto right_routed = router.route_key(right);

  ASSERT_NE(std::get_if<EncodeAsKey>(&left_routed.effect), nullptr);
  EXPECT_EQ(std::get<EncodeAsKey>(left_routed.effect).key, PhysicalKey::home);
  EXPECT_EQ(std::get<EncodeAsKey>(left_routed.effect).modifiers, 0U);
  ASSERT_NE(std::get_if<EncodeAsKey>(&right_routed.effect), nullptr);
  EXPECT_EQ(std::get<EncodeAsKey>(right_routed.effect).key, PhysicalKey::end);
  EXPECT_EQ(std::get<EncodeAsKey>(right_routed.effect).modifiers, key_modifier_shift);
}

TEST(InputRouterTest, ReloadPreservesPressTimeReleaseAndCancelsTransientInput) {
  InputRouter router(default_input_map());
  KeyEvent arrow{.key = PhysicalKey::arrow_left, .modifiers = key_modifier_super, .text = {}};
  ASSERT_TRUE(std::holds_alternative<EncodeAsKey>(router.route_key(arrow).effect));
  KeyEvent captured{.key = PhysicalKey::c,
                    .modifiers = key_modifier_super,
                    .unshifted_codepoint = 'c',
                    .text = {}};
  ASSERT_TRUE(std::holds_alternative<RoutedCommand>(router.route_key(captured).effect));
  const std::array prefix{std::byte{0x02}};
  static_cast<void>(router.route_legacy(prefix, prefix.size()));
  router.reconfigure(default_input_map());
  captured.action = KeyAction::repeat;
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(captured).effect));
  captured.action = KeyAction::release;
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(captured).effect));
  arrow.action = KeyAction::release;
  arrow.modifiers = 0;
  const auto released = router.route_key(arrow);
  ASSERT_TRUE(std::holds_alternative<EncodeAsKey>(released.effect));
  EXPECT_EQ(std::get<EncodeAsKey>(released.effect).key, PhysicalKey::home);
  const std::array ordinary{std::byte{'%'}};
  EXPECT_TRUE(std::holds_alternative<ForwardLegacyInput>(
      router.route_legacy(ordinary, ordinary.size()).effect));
}

TEST(InputRouterTest, RemembersEncodeAsThroughAModifierlessRelease) {
  InputRouter router(default_input_map());
  const KeyEvent press{.action = KeyAction::press,
                       .key = PhysicalKey::arrow_left,
                       .modifiers = key_modifier_super,
                       .unshifted_codepoint = 0,
                       .text = {}};
  const KeyEvent release{.action = KeyAction::release,
                         .key = PhysicalKey::arrow_left,
                         .modifiers = 0,
                         .unshifted_codepoint = 0,
                         .text = {}};

  const auto pressed = router.route_key(press);
  ASSERT_NE(std::get_if<EncodeAsKey>(&pressed.effect), nullptr);
  const auto released = router.route_key(release);
  ASSERT_NE(std::get_if<EncodeAsKey>(&released.effect), nullptr);
  EXPECT_EQ(std::get<EncodeAsKey>(released.effect).key, PhysicalKey::home);
  EXPECT_EQ(std::get<EncodeAsKey>(released.effect).modifiers, 0U);
}

TEST(InputRouterTest, ResizeContextConsumesHostLineMotionAndKeepsCopy) {
  InputRouter router(default_input_map());
  const KeyEvent prefix{.action = KeyAction::press,
                        .key = PhysicalKey::b,
                        .modifiers = key_modifier_control,
                        .unshifted_codepoint = 'b',
                        .text = {}};
  const KeyEvent resize{.action = KeyAction::press,
                        .key = PhysicalKey::m,
                        .modifiers = 0,
                        .unshifted_codepoint = 'm',
                        .text = {}};
  const KeyEvent left{.action = KeyAction::press,
                      .key = PhysicalKey::arrow_left,
                      .modifiers = key_modifier_super,
                      .unshifted_codepoint = 0,
                      .text = {}};
  const KeyEvent copy{.action = KeyAction::press,
                      .key = PhysicalKey::c,
                      .modifiers = key_modifier_super,
                      .unshifted_codepoint = 'c',
                      .text = {}};

  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(prefix).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(resize).effect));
  EXPECT_EQ(router.active_label(), "RESIZE");
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(left).effect));
  const auto copied = router.route_key(copy);
  ASSERT_NE(std::get_if<RoutedCommand>(&copied.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(copied.effect).command, InputCommand::copy_selection);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(InputRouterTest, CustomGenerationCanReplaceOrForwardHostCopyChords) {
  InputMapDraft replaced;
  const auto replaced_base = replaced.add_context({});
  ASSERT_TRUE(replaced_base.has_value());
  ASSERT_TRUE(replaced.bind(*replaced_base, InputChord::byte('c', key_modifier_super),
                            invoke(InputCommand::create_tab)));
  const auto replaced_map = replaced.compile();
  ASSERT_TRUE(replaced_map.has_value());
  InputRouter replaced_router(*replaced_map);
  const KeyEvent super_c{.action = KeyAction::press,
                         .key = PhysicalKey::c,
                         .modifiers = key_modifier_super,
                         .unshifted_codepoint = 'c',
                         .text = {}};
  const auto replaced_routed = replaced_router.route_key(super_c);
  ASSERT_NE(std::get_if<RoutedCommand>(&replaced_routed.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(replaced_routed.effect).command, InputCommand::create_tab);

  InputMapDraft empty;
  const auto empty_base = empty.add_context({});
  ASSERT_TRUE(empty_base.has_value());
  const auto empty_map = empty.compile();
  ASSERT_TRUE(empty_map.has_value());
  InputRouter empty_router(*empty_map);
  EXPECT_TRUE(std::holds_alternative<ForwardCurrentKey>(empty_router.route_key(super_c).effect));
}

TEST(InputRouterTest, BlankPresetSupportsPrefixlessDirectBindings) {
  InputMapConfiguration configuration;
  configuration.reset(InputMapPreset::none);
  ASSERT_TRUE(configuration.set(ConfiguredInputContext::normal,
                                InputChord::byte('s', key_modifier_alt),
                                InputCommand::split_left_right));
  const auto map = compile_input_map(configuration);
  ASSERT_TRUE(map.has_value());
  InputRouter router(*map);
  constexpr std::array ordinary{std::byte{0x02}};
  const auto forwarded = router.route_legacy(ordinary, ordinary.size());
  EXPECT_TRUE(std::holds_alternative<ForwardLegacyInput>(forwarded.effect));

  constexpr std::array direct{std::byte{0x1B}, std::byte{'s'}};
  const auto routed = router.route_legacy(direct, direct.size());
  const auto* const command = std::get_if<RoutedCommand>(&routed.effect);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->command, InputCommand::split_left_right);
}

TEST(InputRouterTest, CompilerDoesNotGivePresetMetadataRuntimePriority) {
  InputMapConfiguration seeded;
  auto explicit_policy = seeded;
  explicit_policy.preset = InputMapPreset::none;
  const auto seeded_map = compile_input_map(seeded);
  const auto explicit_map = compile_input_map(explicit_policy);
  ASSERT_TRUE(seeded_map.has_value());
  ASSERT_TRUE(explicit_map.has_value());
  InputRouter seeded_router(*seeded_map);
  InputRouter explicit_router(*explicit_map);
  constexpr std::array prefix{std::byte{0x02}};
  constexpr std::array split{std::byte{'%'}};

  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(
      seeded_router.route_legacy(prefix, prefix.size()).effect));
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(
      explicit_router.route_legacy(prefix, prefix.size()).effect));
  const auto seeded_split = seeded_router.route_legacy(split, split.size());
  const auto explicit_split = explicit_router.route_legacy(split, split.size());
  ASSERT_NE(std::get_if<RoutedCommand>(&seeded_split.effect), nullptr);
  ASSERT_NE(std::get_if<RoutedCommand>(&explicit_split.effect), nullptr);
  EXPECT_EQ(std::get<RoutedCommand>(seeded_split.effect).command,
            std::get<RoutedCommand>(explicit_split.effect).command);
}

TEST(InputRouterTest, CompiledDefaultPolicyRoutesCopyAndSearchModes) {
  InputMapConfiguration configuration;
  const auto map = compile_input_map(configuration);
  ASSERT_TRUE(map.has_value());
  InputRouter router(*map);
  router.select_base(ConfiguredInputContext::copy);

  constexpr std::array move{std::byte{'h'}};
  const auto moved = router.route_legacy(move, move.size());
  const auto* const move_command = std::get_if<RoutedCommand>(&moved.effect);
  ASSERT_NE(move_command, nullptr);
  EXPECT_EQ(move_command->command, InputCommand::copy_move_left);

  constexpr std::array go{std::byte{'g'}};
  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_legacy(go, go.size()).effect));
  const auto top = router.route_legacy(go, go.size());
  const auto* const top_command = std::get_if<RoutedCommand>(&top.effect);
  ASSERT_NE(top_command, nullptr);
  EXPECT_EQ(top_command->command, InputCommand::copy_history_top);

  router.select_base(ConfiguredInputContext::copy_search);
  constexpr std::array query{std::byte{'n'}};
  EXPECT_TRUE(
      std::holds_alternative<ForwardLegacyInput>(router.route_legacy(query, query.size()).effect));
  constexpr std::array backspace{std::byte{0x7F}};
  const auto erased = router.route_legacy(backspace, backspace.size());
  const auto* const erase_command = std::get_if<RoutedCommand>(&erased.effect);
  ASSERT_NE(erase_command, nullptr);
  EXPECT_EQ(erase_command->command, InputCommand::copy_query_backspace);

  router.select_base(ConfiguredInputContext::rename);
  constexpr std::array home{std::byte{0x01}};
  const auto renamed = router.route_legacy(home, home.size());
  const auto* const rename_command = std::get_if<RoutedCommand>(&renamed.effect);
  ASSERT_NE(rename_command, nullptr);
  EXPECT_EQ(rename_command->command, InputCommand::rename_cursor_home);
}

TEST(InputRouterTest, BlankPolicyCanRecreateContextTransitionsWithoutNativeBindings) {
  InputMapConfiguration configuration;
  configuration.reset(InputMapPreset::none);
  ASSERT_TRUE(configuration.set_context(ConfiguredInputContext::prefix,
                                        {.label = " COMMAND ",
                                         .lifetime = ContextLifetime::one_shot,
                                         .unbound = UnboundBehavior::consume,
                                         .preempts_interaction = false}));
  ASSERT_TRUE(configuration.push(ConfiguredInputContext::normal, InputChord::byte('a'),
                                 ConfiguredInputContext::prefix));
  ASSERT_TRUE(configuration.set(ConfiguredInputContext::prefix, InputChord::byte('s'),
                                InputCommand::split_left_right));
  const auto map = compile_input_map(configuration);
  ASSERT_TRUE(map.has_value());
  InputRouter router(*map);
  constexpr std::array enter{std::byte{'a'}};
  constexpr std::array split{std::byte{'s'}};

  EXPECT_TRUE(
      std::holds_alternative<ConsumedInput>(router.route_legacy(enter, enter.size()).effect));
  EXPECT_EQ(router.active_label(), " COMMAND ");
  const auto routed = router.route_legacy(split, split.size());
  const auto* const command = std::get_if<RoutedCommand>(&routed.effect);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->command, InputCommand::split_left_right);
  EXPECT_TRUE(router.active_label().empty());
}

TEST(InputRouterTest, StructuredSuperChordCanBeThePrefix) {
  InputMapConfiguration configuration;
  ASSERT_TRUE(configuration.set_prefix(InputChord::byte('b', key_modifier_super)));
  ASSERT_TRUE(configuration.set(ConfiguredInputContext::prefix, InputChord::byte('s'),
                                InputCommand::split_left_right));
  const auto map = compile_input_map(configuration);
  ASSERT_TRUE(map.has_value());
  InputRouter router(*map);
  const KeyEvent prefix{.action = KeyAction::press,
                        .key = PhysicalKey::b,
                        .modifiers = key_modifier_super,
                        .unshifted_codepoint = 'b',
                        .text = {}};
  const KeyEvent split{.action = KeyAction::press,
                       .key = PhysicalKey::s,
                       .modifiers = 0,
                       .unshifted_codepoint = 's',
                       .text = {}};

  EXPECT_TRUE(std::holds_alternative<ConsumedInput>(router.route_key(prefix).effect));
  const auto routed = router.route_key(split);
  const auto* const command = std::get_if<RoutedCommand>(&routed.effect);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->command, InputCommand::split_left_right);
}

TEST(InputRouterTest, ACompiledMapCanUseDirectBindingsWithoutAnyTransientContext) {
  InputMapDraft draft;
  const auto normal = draft.add_context({});
  ASSERT_TRUE(normal.has_value());
  ASSERT_TRUE(draft.bind(
      *normal, InputChord::key(PhysicalKey::arrow_left, key_modifier_control | key_modifier_alt),
      invoke(InputCommand::resize_left)));
  const auto map = draft.compile();
  ASSERT_TRUE(map.has_value());
  InputRouter router(*map);
  const KeyEvent direct{.action = KeyAction::press,
                        .key = PhysicalKey::arrow_left,
                        .modifiers = key_modifier_control | key_modifier_alt,
                        .unshifted_codepoint = 0,
                        .text = {}};

  const auto result = router.route_key(direct);

  const auto* const command = std::get_if<RoutedCommand>(&result.effect);
  ASSERT_NE(command, nullptr);
  EXPECT_EQ(command->command, InputCommand::resize_left);
  EXPECT_TRUE(router.active_label().empty());
}

} // namespace
} // namespace frame::input
