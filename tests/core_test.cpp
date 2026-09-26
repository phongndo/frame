#include "core/copy_mode.hpp"
#include "core/float_layer.hpp"
#include "core/session.hpp"
#include "core/session_machine.hpp"
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
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lemma {
namespace {

[[nodiscard]] auto checked_placement(const std::optional<core::FloatPlacement> placement)
    -> core::FloatPlacement {
  if (!placement.has_value()) {
    throw std::logic_error("test requires a valid float placement");
  }
  return *placement;
}

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

// Float tests name only the fields they exercise in Commands and effect options.
#ifdef __clang__
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#endif

// Records Runtime effects and each Pane's committed PTY size, with one-shot rejections.
class FloatRuntime final {
public:
  [[nodiscard]] auto effects() noexcept -> core::SessionRuntimeEffects {
    return {.context = this,
            .spawn = &spawn_callback,
            .resize = &resize_callback,
            .retire = &retire_callback,
            .hold = &hold_callback};
  }

  void reject_next_spawn() noexcept { reject_spawn_ = true; }
  void reject_next_resize() noexcept { reject_resize_ = true; }
  [[nodiscard]] auto size(const PaneId pane) const noexcept -> PaneRectangle {
    return std::span(sizes_).subspan(pane.slot(), 1).front();
  }
  [[nodiscard]] auto live(const PaneId pane) const noexcept -> bool {
    return std::span(live_).subspan(pane.slot(), 1).front();
  }
  [[nodiscard]] constexpr auto resize_batches() const noexcept -> std::size_t {
    return resize_batches_;
  }
  [[nodiscard]] constexpr auto last_batch_size() const noexcept -> std::size_t {
    return last_batch_size_;
  }
  [[nodiscard]] constexpr auto retired() const noexcept -> std::size_t { return retired_; }

private:
  static auto spawn_callback(void* const context, const core::SpawnPaneEffect& effect) noexcept
      -> core::RuntimeEffectStatus {
    auto& runtime = *static_cast<FloatRuntime*>(context);
    if (std::exchange(runtime.reject_spawn_, false)) {
      return core::RuntimeEffectStatus::rejected;
    }
    std::span(runtime.sizes_).subspan(effect.pane.slot(), 1).front() = effect.rectangle;
    std::span(runtime.live_).subspan(effect.pane.slot(), 1).front() = true;
    return core::RuntimeEffectStatus::applied;
  }

  static auto resize_callback(void* const context,
                              const std::span<const core::ResizePaneEffect> effects) noexcept
      -> core::RuntimeEffectStatus {
    auto& runtime = *static_cast<FloatRuntime*>(context);
    ++runtime.resize_batches_;
    runtime.last_batch_size_ = effects.size();
    if (std::exchange(runtime.reject_resize_, false)) {
      return core::RuntimeEffectStatus::rejected;
    }
    for (const auto& effect : effects) {
      std::span(runtime.sizes_).subspan(effect.pane.slot(), 1).front() = effect.target;
    }
    return core::RuntimeEffectStatus::applied;
  }

  static void retire_callback(void* const context, [[maybe_unused]] const SessionId session,
                              const PaneId pane) noexcept {
    auto& runtime = *static_cast<FloatRuntime*>(context);
    std::span(runtime.live_).subspan(pane.slot(), 1).front() = false;
    ++runtime.retired_;
  }

  static void hold_callback([[maybe_unused]] void* const context,
                            [[maybe_unused]] const SessionId session,
                            [[maybe_unused]] const PaneId pane,
                            [[maybe_unused]] const core::ProcessExit process) noexcept {}

  std::array<PaneRectangle, core::panes_per_session_max> sizes_{};
  std::array<bool, core::panes_per_session_max> live_{};
  std::size_t resize_batches_{0};
  std::size_t last_batch_size_{0};
  std::size_t retired_{0};
  bool reject_spawn_{false};
  bool reject_resize_{false};
};

// A 100x30 Attachment with a one-row top dock and one tiled Pane.
class FloatSessionTest : public testing::Test {
public:
  FloatSessionTest() : session_("floats", {}, {}, LaunchEnvironmentMode::inherit) {
    session_.id = SessionId::from_parts(0, 1);
    session_.attachment.id = AttachmentId::from_parts(0, 1);
    session_.attachment.session = session_.id;
    session_.attachment.columns = 100;
    session_.attachment.rows = 30;
    session_.attachment.content_viewport = viewport;
    const auto created = machine().create_tab();
    EXPECT_EQ(created.result.status, CommandStatus::applied);
    tab_id_ = created.created_tab;
    tiled_ = created.created_pane;
  }

  static constexpr PaneRectangle viewport{.column = 0, .row = 1, .columns = 100, .rows = 29};

  [[nodiscard]] auto machine() noexcept -> core::SessionMachine {
    return core::SessionMachine(session_, {.runtime = runtime_.effects()});
  }
  [[nodiscard]] auto tab() noexcept -> Tab& {
    auto& slot = std::span(session_.tabs).subspan(tab_id_.slot(), 1).front();
    return *slot.tab;
  }
  [[nodiscard]] auto pane(const PaneId id) noexcept -> const Pane* {
    const auto& slot = std::span(session_.panes).subspan(id.slot(), 1).front();
    return slot.pane != nullptr && slot.pane->id == id ? slot.pane.get() : nullptr;
  }
  [[nodiscard]] auto open_float(const core::FloatPlacement placement, const bool focus = true)
      -> PaneId {
    const auto created =
        machine().float_pane(tab_id_, {.placement = placement, .focus_created = focus});
    EXPECT_EQ(created.result.status, CommandStatus::applied);
    return created.created_pane;
  }
  [[nodiscard]] auto dispatch(const CommandKind kind, const PaneId target,
                              const CommandPayload payload = {}) -> core::SessionTransition {
    return machine().dispatch({.kind = kind,
                               .origin = CommandOrigin::internal,
                               .target = {.session = session_.id, .tab = tab_id_, .pane = target},
                               .payload = payload});
  }
  void expect_invariants() const {
    EXPECT_EQ(core::check_session_invariants(session_), std::nullopt)
        << core::session_invariant_name(core::check_session_invariants(session_).value_or(
               core::SessionInvariantError::invalid_session_id));
  }

  Session session_;
  FloatRuntime runtime_;
  TabId tab_id_;
  PaneId tiled_;
};

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(FloatSessionTest, FloatsJoinTheTabAboveItsLayoutWithAFramedPty) {
  const auto placement = checked_placement(core::FloatPlacement::centered(40, 12));
  const auto created = machine().float_pane(tab_id_, {.placement = placement});
  ASSERT_EQ(created.result.status, CommandStatus::applied);
  const auto floating = created.created_pane;
  EXPECT_EQ(created.created_tab, tab_id_);
  EXPECT_TRUE(tab().floats.contains(floating));
  EXPECT_FALSE(tab().layout.contains(floating));
  EXPECT_EQ(tab().layout.pane_count(), 1U);
  // The placement names the outer rectangle; the PTY is the inner one inside the native frame.
  constexpr PaneRectangle inner{.column = 31, .row = 10, .columns = 38, .rows = 10};
  ASSERT_NE(pane(floating), nullptr);
  EXPECT_EQ(pane(floating)->rectangle, inner);
  EXPECT_EQ(runtime_.size(floating), inner);
  EXPECT_EQ(tab().focused_pane(), floating);
  EXPECT_EQ(tab().tiled_focus(), tiled_);
  EXPECT_EQ(tab().previous_pane, tiled_);
  expect_invariants();

  // Floats count against the per-Tab layer and the Session Pane limit.
  for (std::size_t count = 1; count < core::floats_per_tab_max; ++count) {
    static_cast<void>(open_float(placement, false));
  }
  EXPECT_EQ(machine().float_pane(tab_id_, {.placement = placement}).result.status,
            CommandStatus::capacity);
  std::size_t panes = core::floats_per_tab_max + 1U;
  while (panes < core::panes_per_session_max) {
    const auto next = machine().create_tab({.activate = false});
    ASSERT_EQ(next.result.status, CommandStatus::applied);
    ++panes;
    for (std::size_t count = 0;
         count < core::floats_per_tab_max && panes < core::panes_per_session_max; ++count) {
      ASSERT_EQ(machine()
                    .float_pane(next.created_tab, {.placement = placement, .focus_created = false})
                    .result.status,
                CommandStatus::applied);
      ++panes;
    }
  }
  const auto last = session_.tab_order.at(session_.tab_order.size() - 1U);
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(machine().float_pane(last.value_or(TabId{}), {.placement = placement}).result.status,
            CommandStatus::capacity);
  expect_invariants();
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(FloatSessionTest, FocusMovesBetweenLayersAndHidingReturnsItToTheTiledLayer) {
  const auto lower = open_float(checked_placement(core::FloatPlacement::absolute(0, 0, 20, 10)));
  const auto upper = open_float(checked_placement(core::FloatPlacement::absolute(40, 0, 20, 10)));
  ASSERT_EQ(tab().floats.top(), upper);

  // Focusing a lower float raises it.
  EXPECT_EQ(dispatch(CommandKind::focus_pane, lower).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), lower);
  EXPECT_EQ(tab().floats.top(), lower);
  EXPECT_EQ(tab().previous_pane, upper);

  // Focusing the tiled layer keeps floats visible.
  EXPECT_EQ(dispatch(CommandKind::focus_pane, tiled_).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), tiled_);
  EXPECT_FALSE(tab().float_focused());
  EXPECT_TRUE(tab().floats_visible());
  expect_invariants();

  EXPECT_EQ(dispatch(CommandKind::focus_pane, upper).result.status, CommandStatus::applied);
  const auto hidden = machine().set_floats_visible(tab_id_, false);
  EXPECT_EQ(hidden.result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), tiled_);
  EXPECT_EQ(machine().set_floats_visible(tab_id_, false).result.status, CommandStatus::no_effect);
  const auto refused = dispatch(CommandKind::focus_pane, lower);
  EXPECT_EQ(refused.result.status, CommandStatus::unavailable);
  EXPECT_EQ(refused.reason, core::TransitionReason::floats_hidden);
  expect_invariants();

  // Showing floats does not take focus back.
  EXPECT_EQ(machine().set_floats_visible(tab_id_, true).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), tiled_);
  expect_invariants();
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(FloatSessionTest, ClosingAFocusedFloatFallsBackToPreviousThenTiledFocus) {
  const auto first = open_float(checked_placement(core::FloatPlacement::centered(20, 8)));
  const auto second = open_float(checked_placement(core::FloatPlacement::centered(30, 8)));
  ASSERT_EQ(tab().previous_pane, first);

  // The previous Pane is an eligible float, so it takes focus and is raised.
  EXPECT_EQ(dispatch(CommandKind::close_pane, second).result.status, CommandStatus::applied);
  EXPECT_EQ(pane(second), nullptr);
  EXPECT_FALSE(runtime_.live(second));
  EXPECT_EQ(tab().focused_pane(), first);
  EXPECT_EQ(tab().previous_pane, first);
  expect_invariants();

  // With no eligible previous Pane, focus returns to the tiled focus.
  EXPECT_EQ(dispatch(CommandKind::close_pane, first).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), tiled_);
  EXPECT_TRUE(tab().floats.empty());
  expect_invariants();

  // A previous tiled Pane regains focus when a float opened from it closes.
  const auto split = machine().split_pane(tab_id_, tiled_, core::SplitAxis::left_right);
  ASSERT_EQ(split.result.status, CommandStatus::applied);
  ASSERT_EQ(dispatch(CommandKind::focus_pane, tiled_).result.status, CommandStatus::applied);
  const auto third = open_float(checked_placement(core::FloatPlacement::centered(20, 8)));
  ASSERT_EQ(dispatch(CommandKind::focus_pane, split.created_pane).result.status,
            CommandStatus::applied);
  ASSERT_EQ(dispatch(CommandKind::focus_pane, third).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().previous_pane, split.created_pane);
  // An exited float with the close policy is removed like a closed one.
  const auto exited = machine().runtime_failed(third, {}, true);
  EXPECT_EQ(exited.result.status, CommandStatus::applied);
  EXPECT_EQ(pane(third), nullptr);
  EXPECT_EQ(tab().focused_pane(), split.created_pane);
  EXPECT_EQ(tab().layout.pane_count(), 2U);
  expect_invariants();
}

TEST_F(FloatSessionTest, ClosingTheLastTiledPaneClosesTheTabWithItsFloats) {
  const auto second_tab = machine().create_tab();
  ASSERT_EQ(second_tab.result.status, CommandStatus::applied);
  ASSERT_EQ(dispatch(CommandKind::select_tab, {}).result.status, CommandStatus::applied);
  const auto floating = open_float(checked_placement(core::FloatPlacement::relative(50, 50)));
  const auto retired = runtime_.retired();

  EXPECT_EQ(dispatch(CommandKind::close_pane, tiled_).result.status, CommandStatus::applied);
  EXPECT_EQ(std::span(session_.tabs).subspan(tab_id_.slot(), 1).front().tab, nullptr);
  EXPECT_EQ(pane(floating), nullptr);
  EXPECT_FALSE(runtime_.live(floating));
  EXPECT_EQ(runtime_.retired(), retired + 2U);
  EXPECT_EQ(session_.active_tab, second_tab.created_tab);
  expect_invariants();
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(FloatSessionTest, RejectedEffectsPublishNoFloatState) {
  const auto before = core::session_state_hash(session_);
  runtime_.reject_next_spawn();
  const auto rejected = machine().float_pane(
      tab_id_, {.placement = checked_placement(core::FloatPlacement::centered(10, 5))});
  EXPECT_EQ(rejected.result.status, CommandStatus::unavailable);
  EXPECT_TRUE(tab().floats.empty());
  EXPECT_EQ(core::session_state_hash(session_), before);

  const auto placement = checked_placement(core::FloatPlacement::centered(10, 5));
  const auto floating = open_float(placement);
  const auto placed = pane(floating)->rectangle;
  runtime_.reject_next_resize();
  const auto moved = machine().place_float(
      tab_id_, floating, checked_placement(core::FloatPlacement::absolute(0, 0, 20, 20)));
  EXPECT_EQ(moved.result.status, CommandStatus::unavailable);
  EXPECT_EQ(tab().floats.placement(floating), placement);
  EXPECT_EQ(pane(floating)->rectangle, placed);

  const auto geometry = core::session_state_hash(session_);
  runtime_.reject_next_resize();
  EXPECT_EQ(machine().resize_attachment(120, 40).result.status, CommandStatus::unavailable);
  EXPECT_EQ(core::session_state_hash(session_), geometry);
  EXPECT_EQ(runtime_.size(floating), placed);
  expect_invariants();
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(FloatSessionTest, ViewportChangesResizeFloatsInTheSameBatchOrSuspendThem) {
  const auto fixed =
      open_float(checked_placement(core::FloatPlacement::absolute(50, 10, 40, 15)), false);
  const auto relative = open_float(checked_placement(core::FloatPlacement::relative(50, 50)));
  const auto batches = runtime_.resize_batches();

  // Growing the Attachment resizes the tiled layout and both floats in one Runtime batch.
  ASSERT_EQ(machine().resize_attachment(120, 40).result.status, CommandStatus::applied);
  EXPECT_EQ(runtime_.resize_batches(), batches + 1U);
  EXPECT_EQ(runtime_.last_batch_size(), 3U);
  EXPECT_EQ(runtime_.size(relative),
            (PaneRectangle{.column = 31, .row = 11, .columns = 58, .rows = 18}));
  expect_invariants();

  // A float that no longer fits is suspended: it keeps its PTY size and loses focus.
  ASSERT_EQ(dispatch(CommandKind::focus_pane, fixed).result.status, CommandStatus::applied);
  const auto fixed_size = runtime_.size(fixed);
  ASSERT_EQ(machine().resize_attachment(60, 20).result.status, CommandStatus::applied);
  EXPECT_EQ(runtime_.size(fixed), fixed_size);
  EXPECT_EQ(pane(fixed)->rectangle, fixed_size);
  EXPECT_EQ(tab().focused_pane(), tiled_);
  EXPECT_TRUE(tab().floats.contains(fixed));
  const auto refused = dispatch(CommandKind::focus_pane, fixed);
  EXPECT_EQ(refused.reason, core::TransitionReason::float_suspended);
  expect_invariants();

  // It reappears when it fits again without retaking focus.
  ASSERT_EQ(machine().resize_attachment(120, 40).result.status, CommandStatus::applied);
  EXPECT_EQ(runtime_.size(fixed),
            (PaneRectangle{.column = 51, .row = 11, .columns = 38, .rows = 13}));
  EXPECT_EQ(tab().focused_pane(), tiled_);
  expect_invariants();
}

TEST_F(FloatSessionTest, SuspendedTabsReturnFloatFocusToTilesUntilExplicitlyRefocused) {
  ASSERT_EQ(machine().split_pane(tab_id_, tiled_, core::SplitAxis::left_right).result.status,
            CommandStatus::applied);
  ASSERT_EQ(dispatch(CommandKind::focus_pane, tiled_).result.status, CommandStatus::applied);
  const auto first = open_float(checked_placement(core::FloatPlacement::relative(50, 50)));
  const auto second = open_float(checked_placement(core::FloatPlacement::centered(20, 8)));
  const auto previous_size = runtime_.size(second);

  ASSERT_EQ(machine().resize_attachment(1, 1).result.status, CommandStatus::applied);
  EXPECT_TRUE(tab().layout_suspended);
  EXPECT_EQ(tab().focused_pane(), tiled_);
  EXPECT_FALSE(tab().float_focused());
  EXPECT_EQ(runtime_.size(second), previous_size);
  EXPECT_EQ(dispatch(CommandKind::focus_pane, first).reason,
            core::TransitionReason::float_suspended);
  expect_invariants();

  ASSERT_EQ(machine().resize_attachment(100, 30).result.status, CommandStatus::applied);
  EXPECT_FALSE(tab().layout_suspended);
  EXPECT_EQ(tab().focused_pane(), tiled_);
  EXPECT_EQ(dispatch(CommandKind::focus_pane, first).result.status, CommandStatus::applied);
  expect_invariants();
}

TEST_F(FloatSessionTest, SelectingATabThatCannotFitSuspendsItsFloats) {
  ASSERT_EQ(machine().split_pane(tab_id_, tiled_, core::SplitAxis::left_right).result.status,
            CommandStatus::applied);
  const auto tile = tab().tiled_focus();
  const auto floating = open_float(checked_placement(core::FloatPlacement::relative(50, 50)));
  ASSERT_EQ(machine().create_tab().result.status, CommandStatus::applied);
  ASSERT_EQ(machine().resize_attachment(1, 1).result.status, CommandStatus::applied);
  ASSERT_EQ(dispatch(CommandKind::select_tab, {}).result.status, CommandStatus::applied);
  EXPECT_TRUE(tab().layout_suspended);
  EXPECT_EQ(tab().focused_pane(), tile);
  EXPECT_EQ(dispatch(CommandKind::focus_pane, floating).reason,
            core::TransitionReason::float_suspended);
  expect_invariants();

  ASSERT_EQ(machine().resize_attachment(100, 30).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), tile);
  expect_invariants();
}

TEST_F(FloatSessionTest, RejectedReflowAfterClosingATileSuspendsItsFloats) {
  const auto split = machine().split_pane(tab_id_, tiled_, core::SplitAxis::left_right);
  ASSERT_EQ(split.result.status, CommandStatus::applied);
  const auto floating = open_float(checked_placement(core::FloatPlacement::relative(50, 50)));
  runtime_.reject_next_resize();
  ASSERT_EQ(dispatch(CommandKind::close_pane, tiled_).result.status, CommandStatus::applied);
  EXPECT_TRUE(tab().layout_suspended);
  EXPECT_EQ(tab().focused_pane(), split.created_pane);
  EXPECT_EQ(dispatch(CommandKind::focus_pane, floating).reason,
            core::TransitionReason::float_suspended);
  expect_invariants();
}

TEST_F(FloatSessionTest, PlacingAFloatWithoutRuntimeEffectsDoesNotPublishState) {
  const auto floating = open_float(checked_placement(core::FloatPlacement::centered(20, 8)));
  const auto before = core::session_state_hash(session_);
  core::SessionMachine without_runtime(session_, {});
  const auto placed = without_runtime.place_float(
      tab_id_, floating, checked_placement(core::FloatPlacement::centered(30, 10)));
  EXPECT_EQ(placed.result.status, CommandStatus::unavailable);
  EXPECT_EQ(core::session_state_hash(session_), before);
  expect_invariants();
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(FloatSessionTest, TiledLayoutCommandsDoNotApplyToFloats) {
  const auto floating = open_float(checked_placement(core::FloatPlacement::centered(20, 8)), false);
  const auto split = machine().split_pane(tab_id_, floating, core::SplitAxis::left_right);
  EXPECT_EQ(split.result.status, CommandStatus::unavailable);
  EXPECT_EQ(split.reason, core::TransitionReason::floating_pane);
  for (const auto kind :
       {CommandKind::toggle_zoom, CommandKind::resize_left, CommandKind::resize_down}) {
    const auto rejected = dispatch(kind, floating);
    EXPECT_EQ(rejected.result.status, CommandStatus::unavailable);
    EXPECT_EQ(rejected.reason, core::TransitionReason::floating_pane);
  }
  const auto swapped =
      dispatch(CommandKind::swap_panes, tiled_, PaneSwapCommand{.other = floating});
  EXPECT_EQ(swapped.reason, core::TransitionReason::floating_pane);
  const auto divider = machine().dispatch(
      {.kind = CommandKind::resize_left_right_divider,
       .origin = CommandOrigin::internal,
       .target = {.session = session_.id, .tab = tab_id_, .pane = tiled_, .peer_pane = floating},
       .payload = CommandCoordinate{.value = 10}});
  EXPECT_EQ(divider.reason, core::TransitionReason::floating_pane);
  const auto placed = machine().place_float(
      tab_id_, tiled_, checked_placement(core::FloatPlacement::centered(20, 8)));
  EXPECT_EQ(placed.reason, core::TransitionReason::tiled_pane);
  EXPECT_EQ(
      machine()
          .place_float(tab_id_, floating, checked_placement(core::FloatPlacement::centered(20, 8)))
          .result.status,
      CommandStatus::no_effect);
  EXPECT_EQ(
      machine()
          .place_float(tab_id_, floating, checked_placement(core::FloatPlacement::centered(200, 8)))
          .reason,
      core::TransitionReason::float_suspended);
  EXPECT_EQ(machine()
                .float_pane(tab_id_, {.placement = checked_placement(
                                          core::FloatPlacement::absolute(90, 0, 20, 8))})
                .reason,
            core::TransitionReason::float_suspended);
  expect_invariants();
}

// GoogleTest assertions inflate the measured branch count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(FloatSessionTest, CyclingAndDirectionalFocusStayWithinTheFocusedLayer) {
  const auto right = machine().split_pane(tab_id_, tiled_, core::SplitAxis::left_right);
  ASSERT_EQ(right.result.status, CommandStatus::applied);
  const auto west = open_float(checked_placement(core::FloatPlacement::absolute(0, 5, 20, 8)));
  const auto east = open_float(checked_placement(core::FloatPlacement::absolute(70, 5, 20, 8)));
  const auto middle = open_float(checked_placement(core::FloatPlacement::absolute(35, 5, 20, 8)));
  ASSERT_EQ(tab().focused_pane(), middle);

  EXPECT_EQ(core::pane_in_direction(session_, tab(), middle, core::PaneDirection::left), west);
  EXPECT_EQ(core::pane_in_direction(session_, tab(), middle, core::PaneDirection::right), east);
  EXPECT_EQ(core::pane_in_direction(session_, tab(), right.created_pane, core::PaneDirection::left),
            tiled_);
  EXPECT_FALSE(
      core::pane_in_direction(session_, tab(), tiled_, core::PaneDirection::up).has_value());

  // Cycling from the top float focuses the bottom one; raising it makes the cycle visit all.
  EXPECT_EQ(dispatch(CommandKind::focus_next, {}).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), west);
  EXPECT_EQ(dispatch(CommandKind::focus_next, {}).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), east);
  EXPECT_EQ(dispatch(CommandKind::focus_next, {}).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), middle);

  // The tiled layer cycles among layout Panes only.
  ASSERT_EQ(dispatch(CommandKind::focus_pane, tiled_).result.status, CommandStatus::applied);
  EXPECT_EQ(dispatch(CommandKind::focus_next, {}).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), right.created_pane);
  EXPECT_EQ(dispatch(CommandKind::focus_next, {}).result.status, CommandStatus::applied);
  EXPECT_EQ(tab().focused_pane(), tiled_);
  expect_invariants();
}

#ifdef __clang__
#if __has_warning("-Wmissing-designated-field-initializers")
#pragma clang diagnostic pop
#endif
#endif

} // namespace
} // namespace lemma
