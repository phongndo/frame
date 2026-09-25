#include "core/copy_mode.hpp"
#include "core/session.hpp"
#include "lemma/command.hpp"
#include "lemma/generational_store.hpp"
#include "lemma/id.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>

namespace lemma {
namespace {

using core::Attachment;
using core::CopyModePhase;
using core::LaunchEnvironmentMode;
using core::Pane;
using core::PaneExitPolicy;
using core::ProcessExitKind;
using core::Session;
using core::Tab;
using core::TabOrder;

static_assert(std::is_trivially_copyable_v<Command>);
static_assert(std::is_trivially_copyable_v<CommandResult>);
static_assert(!std::is_same_v<AttachmentId, ConnectionId>);
static_assert(std::is_trivially_destructible_v<Attachment>);

struct CommandCapture final {
  Command command;
  CommandResult result;
  std::size_t calls{0};
  std::size_t observations{0};
};

[[nodiscard]] auto capture_command(void* const context, const Command& command) noexcept
    -> CommandResult {
  auto& capture = *static_cast<CommandCapture*>(context);
  capture.command = command;
  ++capture.calls;
  return {.status = CommandStatus::applied};
}

void observe_command(void* const context, const Command& command,
                     const CommandResult result) noexcept {
  auto& capture = *static_cast<CommandCapture*>(context);
  capture.command = command;
  capture.result = result;
  ++capture.observations;
}

TEST(CommandDispatcherTest, DispatchesValidatedBoundedValue) {
  CommandCapture capture;
  const CommandDispatcher dispatcher(&capture_command, &capture, &observe_command, &capture);
  const Command command{
      .kind = CommandKind::select_tab,
      .origin = CommandOrigin::cli,
      .target = {.session = SessionId::from_parts(2, 3),
                 .tab = TabId::from_parts(4, 5),
                 .pane = {},
                 .peer_pane = {},
                 .attachment = {}},
      .payload = CommandCoordinate{.value = 7},
  };

  const auto result = dispatcher.dispatch(command);

  EXPECT_TRUE(result.succeeded());
  EXPECT_EQ(result.status, CommandStatus::applied);
  EXPECT_EQ(capture.calls, 1U);
  EXPECT_EQ(capture.observations, 1U);
  EXPECT_EQ(capture.result.status, CommandStatus::applied);
  EXPECT_EQ(capture.command.kind, CommandKind::select_tab);
  EXPECT_EQ(capture.command.origin, CommandOrigin::cli);
  EXPECT_EQ(capture.command.target.session, command.target.session);
  EXPECT_EQ(capture.command.target.tab, command.target.tab);
  EXPECT_EQ(std::get<CommandCoordinate>(capture.command.payload).value, 7U);
}

TEST(CommandDispatcherTest, DispatchesTypedOneCellAndBatchedResizeCommands) {
  CommandCapture capture;
  const CommandDispatcher dispatcher(&capture_command, &capture);

  const auto one_cell =
      dispatcher.dispatch({.kind = CommandKind::resize_left, .origin = CommandOrigin::keymap});
  const auto batched =
      dispatcher.dispatch({.kind = CommandKind::resize_right,
                           .origin = CommandOrigin::cli,
                           .payload = CommandCoordinate{.value = command_resize_amount_max}});

  EXPECT_EQ(one_cell.status, CommandStatus::applied);
  EXPECT_EQ(batched.status, CommandStatus::applied);
  EXPECT_EQ(capture.calls, 2U);
  EXPECT_EQ(capture.command.kind, CommandKind::resize_right);
  EXPECT_EQ(std::get<CommandCoordinate>(capture.command.payload).value, command_resize_amount_max);
}

TEST(CommandDispatcherTest, DispatchesTypedAbsoluteZoomCommand) {
  CommandCapture capture;
  const CommandDispatcher dispatcher(&capture_command, &capture);
  const Command command{
      .kind = CommandKind::set_zoom,
      .origin = CommandOrigin::cli,
      .target = {.session = SessionId::from_parts(1, 2),
                 .tab = TabId::from_parts(2, 3),
                 .pane = PaneId::from_parts(3, 4),
                 .peer_pane = {},
                 .attachment = {}},
      .payload = PaneZoomCommand{.enabled = true},
  };

  EXPECT_EQ(dispatcher.dispatch(command).status, CommandStatus::applied);
  EXPECT_EQ(capture.calls, 1U);
  EXPECT_TRUE(std::get<PaneZoomCommand>(capture.command.payload).enabled);

  auto missing_value = command;
  missing_value.payload = std::monostate{};
  EXPECT_EQ(dispatcher.dispatch(missing_value).status, CommandStatus::invalid_command);
  EXPECT_EQ(capture.calls, 1U);
}

TEST(CommandDispatcherTest, RequiresAttachmentIdentityForInteractionCancellation) {
  CommandCapture capture;
  const CommandDispatcher dispatcher(&capture_command, &capture);
  const Command command{
      .kind = CommandKind::cancel_attachment_interaction,
      .origin = CommandOrigin::keymap,
      .target = {.session = SessionId::from_parts(1, 2),
                 .tab = {},
                 .pane = {},
                 .peer_pane = {},
                 .attachment = AttachmentId::from_parts(1, 2)},
  };

  EXPECT_EQ(dispatcher.dispatch(command).status, CommandStatus::applied);
  EXPECT_EQ(capture.calls, 1U);
  auto missing_attachment = command;
  missing_attachment.target.attachment = {};
  EXPECT_EQ(dispatcher.dispatch(missing_attachment).status, CommandStatus::invalid_target);
  EXPECT_EQ(capture.calls, 1U);
}

TEST(CommandDispatcherTest, DispatchesGenerationSafeDividerResizeCommand) {
  CommandCapture capture;
  const CommandDispatcher dispatcher(&capture_command, &capture);
  const Command command{
      .kind = CommandKind::resize_left_right_divider,
      .origin = CommandOrigin::client,
      .target = {.session = SessionId::from_parts(1, 2),
                 .tab = TabId::from_parts(3, 4),
                 .pane = PaneId::from_parts(5, 6),
                 .peer_pane = PaneId::from_parts(7, 8),
                 .attachment = AttachmentId::from_parts(1, 2)},
      .payload = CommandCoordinate{.value = 45},
  };

  const auto result = dispatcher.dispatch(command);

  EXPECT_EQ(result.status, CommandStatus::applied);
  EXPECT_EQ(capture.calls, 1U);
  EXPECT_EQ(capture.command.kind, CommandKind::resize_left_right_divider);
  EXPECT_EQ(capture.command.target.peer_pane, command.target.peer_pane);
  EXPECT_EQ(std::get<CommandCoordinate>(capture.command.payload).value, 45U);
}

TEST(CommandDispatcherTest, RejectsInvalidValuesBeforeExecutor) {
  CommandCapture capture;
  const CommandDispatcher dispatcher(&capture_command, &capture, &observe_command, &capture);

  EXPECT_EQ(dispatcher.dispatch({}).status, CommandStatus::invalid_command);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::select_tab,
                           .origin = CommandOrigin::client,
                           .payload = CommandCoordinate{.value = command_tab_slots_max}})
                .status,
            CommandStatus::invalid_command);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::resize_left,
                           .origin = CommandOrigin::client,
                           .payload = CommandCoordinate{.value = 0}})
                .status,
            CommandStatus::invalid_command);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::resize_right,
                           .origin = CommandOrigin::client,
                           .payload = CommandCoordinate{.value = static_cast<std::uint16_t>(
                                                            command_resize_amount_max + 1U)}})
                .status,
            CommandStatus::invalid_command);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::close_pane,
                           .origin = CommandOrigin::client,
                           .target = {.session = {},
                                      .tab = {},
                                      .pane = PaneId::from_parts(1, 1),
                                      .peer_pane = {},
                                      .attachment = {}}})
                .status,
            CommandStatus::invalid_target);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::close_tab,
                           .origin = CommandOrigin::cli,
                           .target = {.session = {},
                                      .tab = TabId::from_parts(1, 1),
                                      .pane = {},
                                      .peer_pane = {},
                                      .attachment = {}}})
                .status,
            CommandStatus::invalid_target);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::detach_client,
                           .origin = CommandOrigin::client,
                           .target = {.session = {},
                                      .tab = {},
                                      .pane = {},
                                      .peer_pane = {},
                                      .attachment = AttachmentId::from_parts(1, 1)}})
                .status,
            CommandStatus::invalid_target);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::focus_next,
                           .origin = CommandOrigin::client,
                           .payload = CommandCoordinate{.value = 1}})
                .status,
            CommandStatus::invalid_command);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::resize_left_right_divider,
                           .origin = CommandOrigin::client,
                           .target = {.session = SessionId::from_parts(1, 1),
                                      .tab = TabId::from_parts(1, 1),
                                      .pane = PaneId::from_parts(1, 1),
                                      .peer_pane = {},
                                      .attachment = AttachmentId::from_parts(1, 1)},
                           .payload = CommandCoordinate{.value = 10}})
                .status,
            CommandStatus::invalid_target);
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::focus_next,
                           .origin = CommandOrigin::client,
                           .target = {.session = SessionId::from_parts(1, 1),
                                      .tab = TabId::from_parts(1, 1),
                                      .pane = PaneId::from_parts(1, 1),
                                      .peer_pane = PaneId::from_parts(2, 1),
                                      .attachment = AttachmentId::from_parts(1, 1)}})
                .status,
            CommandStatus::invalid_target);
  EXPECT_EQ(capture.calls, 0U);
  EXPECT_EQ(capture.observations, 10U);

  const CommandDispatcher missing_executor(nullptr, nullptr);
  EXPECT_EQ(missing_executor
                .dispatch({.kind = CommandKind::focus_next, .origin = CommandOrigin::internal})
                .status,
            CommandStatus::failed);
}

TEST(CommandDispatcherTest, ValidatesTypedRenameReorderAndSwapPayloads) {
  CommandCapture capture;
  const CommandDispatcher dispatcher(&capture_command, &capture);
  const auto session = SessionId::from_parts(1, 1);
  const auto tab = TabId::from_parts(2, 1);
  const auto other_tab = TabId::from_parts(3, 1);
  const auto pane = PaneId::from_parts(4, 1);
  const auto other_pane = PaneId::from_parts(5, 1);
  const auto attachment = AttachmentId::from_parts(1, 1);
  const auto name = SessionNameValue::create("renamed");
  const auto title = TabTitleValue::create("build logs");
  ASSERT_TRUE(name.has_value());
  ASSERT_TRUE(title.has_value());
  const auto name_value = name.value_or(SessionNameValue{});
  const auto title_value = title.value_or(TabTitleValue{});

  EXPECT_TRUE(dispatcher
                  .dispatch({.kind = CommandKind::begin_rename_session,
                             .origin = CommandOrigin::client,
                             .target = {.session = session,
                                        .tab = {},
                                        .pane = {},
                                        .peer_pane = {},
                                        .attachment = attachment}})
                  .succeeded());
  EXPECT_TRUE(dispatcher
                  .dispatch({.kind = CommandKind::begin_rename_tab,
                             .origin = CommandOrigin::client,
                             .target = {.session = session,
                                        .tab = tab,
                                        .pane = {},
                                        .peer_pane = {},
                                        .attachment = attachment}})
                  .succeeded());
  EXPECT_TRUE(dispatcher
                  .dispatch({.kind = CommandKind::rename_session,
                             .origin = CommandOrigin::cli,
                             .target = {.session = session,
                                        .tab = {},
                                        .pane = {},
                                        .peer_pane = {},
                                        .attachment = {}},
                             .payload = name_value})
                  .succeeded());
  EXPECT_TRUE(dispatcher
                  .dispatch({.kind = CommandKind::rename_tab,
                             .origin = CommandOrigin::cli,
                             .target = {.session = session,
                                        .tab = tab,
                                        .pane = {},
                                        .peer_pane = {},
                                        .attachment = {}},
                             .payload = title_value})
                  .succeeded());
  EXPECT_TRUE(dispatcher
                  .dispatch({.kind = CommandKind::place_tab,
                             .origin = CommandOrigin::keymap,
                             .target = {.session = session,
                                        .tab = tab,
                                        .pane = {},
                                        .peer_pane = {},
                                        .attachment = {}},
                             .payload = TabPlacementCommand{.before = other_tab}})
                  .succeeded());
  EXPECT_TRUE(dispatcher
                  .dispatch({.kind = CommandKind::swap_panes,
                             .origin = CommandOrigin::keymap,
                             .target = {.session = session,
                                        .tab = tab,
                                        .pane = pane,
                                        .peer_pane = {},
                                        .attachment = {}},
                             .payload = PaneSwapCommand{.other = other_pane}})
                  .succeeded());
  EXPECT_EQ(dispatcher
                .dispatch({.kind = CommandKind::swap_panes,
                           .origin = CommandOrigin::keymap,
                           .target = {.session = session,
                                      .tab = tab,
                                      .pane = pane,
                                      .peer_pane = {},
                                      .attachment = {}},
                           .payload = PaneSwapCommand{.other = pane}})
                .status,
            CommandStatus::invalid_command);
}

TEST(SessionModelTest, TabOrderIsOneBoundedStableIdPermutation) {
  TabOrder order;
  const auto first = TabId::from_parts(4, 1);
  const auto second = TabId::from_parts(1, 3);
  const auto third = TabId::from_parts(9, 2);

  ASSERT_TRUE(order.append(first));
  ASSERT_TRUE(order.append(second));
  ASSERT_TRUE(order.append(third));
  EXPECT_FALSE(order.append(second));
  EXPECT_EQ(order.at(0), first);
  EXPECT_EQ(order.at(1), second);
  EXPECT_EQ(order.at(2), third);

  EXPECT_TRUE(order.place_before(third, first));
  EXPECT_EQ(order.at(0), third);
  EXPECT_EQ(order.at(1), first);
  EXPECT_EQ(order.at(2), second);
  EXPECT_FALSE(order.place_before(third, first));
  EXPECT_TRUE(order.place_before(first, std::nullopt));
  EXPECT_EQ(order.at(0), third);
  EXPECT_EQ(order.at(1), second);
  EXPECT_EQ(order.at(2), first);

  EXPECT_TRUE(order.erase(second));
  EXPECT_EQ(order.size(), 2U);
  EXPECT_EQ(order.at(0), third);
  EXPECT_EQ(order.at(1), first);
  EXPECT_FALSE(order.position_of(second).has_value());
}

TEST(SessionModelTest, PaneCommitsProcessOutcomeOnlyUnderExplicitHoldPolicy) {
  Pane closing{.id = PaneId::from_parts(0, 1),
               .tab = TabId::from_parts(0, 1),
               .rectangle = {},
               .launch_intent = nullptr,
               .process_exit = std::nullopt,
               .exit_policy = PaneExitPolicy::close};
  EXPECT_FALSE(closing.commit_process_exit({.kind = ProcessExitKind::exited, .value = 7}));
  EXPECT_FALSE(closing.process_exit.has_value());

  Pane held{.id = PaneId::from_parts(1, 1),
            .tab = TabId::from_parts(0, 1),
            .rectangle = {},
            .launch_intent = nullptr,
            .process_exit = std::nullopt,
            .exit_policy = PaneExitPolicy::hold};
  EXPECT_TRUE(held.commit_process_exit({.kind = ProcessExitKind::signaled, .value = 15}));
  const auto outcome = held.process_exit.value_or(core::ProcessExit{});
  EXPECT_EQ(outcome.kind, ProcessExitKind::signaled);
  EXPECT_EQ(outcome.value, 15U);
  EXPECT_FALSE(held.commit_process_exit({.kind = ProcessExitKind::exited, .value = 0}));
  EXPECT_EQ(held.process_exit.value_or(core::ProcessExit{}).kind, ProcessExitKind::signaled);
}

TEST(SessionModelTest, RenameAndTabTitleValuesAreBoundedAndValidated) {
  Session session("before", {}, {}, LaunchEnvironmentMode::inherit);
  EXPECT_TRUE(session.rename("after_2"));
  EXPECT_EQ(session.session_name(), "after_2");
  EXPECT_FALSE(session.rename("contains space"));
  EXPECT_EQ(session.session_name(), "after_2");

  Tab tab(TabId::from_parts(0, 1), PaneId::from_parts(0, 1));
  EXPECT_TRUE(tab.set_title_override("build logs"));
  EXPECT_EQ(tab.title_override(), "build logs");
  EXPECT_FALSE(tab.set_title_override(std::string_view{"bad\x1btitle", 9}));
  EXPECT_EQ(tab.title_override(), "build logs");
  EXPECT_TRUE(tab.set_title_override({}));
  EXPECT_TRUE(tab.title_override().empty());

  EXPECT_TRUE(SessionNameValue::create("valid-name").has_value());
  EXPECT_FALSE(SessionNameValue::create("-option-like").has_value());
  EXPECT_FALSE(SessionNameValue::create("invalid name").has_value());
  EXPECT_TRUE(TabTitleValue::create("").has_value());
  EXPECT_FALSE(TabTitleValue::create(std::string_view{"bad\n", 4}).has_value());
}

TEST(CopyModeCoreTest, KeepsSearchMatchesInCentralSafeZoneAndCentersOutsideIt) {
  EXPECT_EQ(core::copy_search_viewport_offset(45, 40, 20, 100), 40U);
  EXPECT_EQ(core::copy_search_viewport_offset(54, 40, 20, 100), 40U);
  EXPECT_EQ(core::copy_search_viewport_offset(44, 40, 20, 100), 34U);
  EXPECT_EQ(core::copy_search_viewport_offset(55, 40, 20, 100), 45U);
  EXPECT_EQ(core::copy_search_viewport_offset(2, 40, 20, 100), 0U);
  EXPECT_EQ(core::copy_search_viewport_offset(99, 0, 20, 100), 80U);
  EXPECT_EQ(core::copy_search_viewport_offset(7, 4, 20, 10), 0U);
}

TEST(GenerationalIdTest, InvalidUntilCreatedFromValidParts) {
  const SessionId invalid;
  const auto session = SessionId::from_parts(7, 3);

  EXPECT_FALSE(invalid.is_valid());
  EXPECT_FALSE(SessionId::try_from_parts(7, 0).has_value());
  EXPECT_FALSE(SessionId::try_from_parts(std::numeric_limits<std::uint32_t>::max(), 3).has_value());
  EXPECT_TRUE(session.is_valid());
  EXPECT_EQ(session.slot(), 7U);
  EXPECT_EQ(session.generation(), 3U);
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(BoundedGenerationalStoreTest, RejectsStaleIdsAndReportsCapacity) {
  struct Value final {
    int number{0};
  };
  BoundedGenerationalStore<Value, SessionId, 2> store;

  const auto first = store.insert(std::make_unique<Value>(Value{.number = 7}));
  const auto second = store.insert(std::make_unique<Value>(Value{.number = 9}));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  const auto first_id = first.value_or(SessionId{});
  const auto second_id = second.value_or(SessionId{});
  EXPECT_EQ(store.size(), 2U);
  EXPECT_EQ(store.get(first_id)->number, 7);
  EXPECT_EQ(store.get(second_id)->number, 9);
  EXPECT_FALSE(store.insert(std::make_unique<Value>()).has_value());

  ASSERT_TRUE(store.erase(first_id));
  EXPECT_FALSE(store.contains(first_id));
  EXPECT_EQ(store.get(first_id), nullptr);
  const auto replacement = store.insert(std::make_unique<Value>(Value{.number = 11}));
  ASSERT_TRUE(replacement.has_value());
  const auto replacement_id = replacement.value_or(SessionId{});
  EXPECT_EQ(replacement_id.slot(), first_id.slot());
  EXPECT_NE(replacement_id.generation(), first_id.generation());
  EXPECT_EQ(store.get(replacement_id)->number, 11);
  EXPECT_FALSE(store.erase(first_id));
}

} // namespace
} // namespace lemma
