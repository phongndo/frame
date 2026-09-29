#ifndef FRAME_INPUT_INPUT_ROUTER_HPP
#define FRAME_INPUT_INPUT_ROUTER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace frame::input {

inline constexpr std::size_t input_contexts_max = 16;
inline constexpr std::size_t input_bindings_max = 240;
inline constexpr std::size_t input_context_stack_max = 4;
inline constexpr std::size_t input_context_label_bytes_max = 16;
inline constexpr std::size_t deferred_input_bytes_max = 4;

enum class PhysicalKey : std::uint8_t {
  unidentified,
  a,
  b,
  c,
  d,
  e,
  f,
  g,
  h,
  i,
  j,
  k,
  l,
  m,
  n,
  o,
  p,
  q,
  r,
  s,
  t,
  u,
  v,
  w,
  x,
  y,
  z,
  enter,
  tab,
  backspace,
  escape,
  space,
  arrow_up,
  arrow_down,
  arrow_left,
  arrow_right,
  home,
  end,
  insert,
  delete_key,
  page_up,
  page_down,
  f1,
  f2,
  f3,
  f4,
  f5,
  f6,
  f7,
  f8,
  f9,
  f10,
  f11,
  f12,
  count,
};

enum class KeyAction : std::uint8_t {
  release,
  press,
  repeat,
};

inline constexpr std::uint16_t key_modifier_shift = 1U << 0U;
inline constexpr std::uint16_t key_modifier_control = 1U << 1U;
inline constexpr std::uint16_t key_modifier_alt = 1U << 2U;
inline constexpr std::uint16_t key_modifier_super = 1U << 3U;
inline constexpr std::uint16_t key_modifier_caps_lock = 1U << 4U;
inline constexpr std::uint16_t key_modifier_num_lock = 1U << 5U;
inline constexpr std::uint16_t key_modifiers_all = key_modifier_shift | key_modifier_control |
                                                   key_modifier_alt | key_modifier_super |
                                                   key_modifier_caps_lock | key_modifier_num_lock;

struct KeyEvent final {
  KeyAction action{KeyAction::press};
  PhysicalKey key{PhysicalKey::unidentified};
  std::uint16_t modifiers{0};
  std::uint32_t unshifted_codepoint{0};
  std::span<const std::byte> text;
};

enum class InputCommand : std::uint8_t {
  detach,
  split_left_right,
  split_top_bottom,
  resize_left,
  resize_right,
  resize_up,
  resize_down,
  focus_left,
  focus_right,
  focus_up,
  focus_down,
  focus_next,
  focus_previous,
  close_pane,
  toggle_zoom,
  enter_copy_mode,
  enter_copy_search_forward,
  enter_copy_search_backward,
  copy_selection,
  create_tab,
  next_tab,
  previous_tab,
  begin_rename_session,
  begin_rename_tab,
  begin_command_line,
  show_messages,
  message_view_leave,
  message_view_previous,
  message_view_next,
  message_view_page_previous,
  message_view_page_next,
  message_view_history_start,
  message_view_history_end,
  move_tab_left,
  move_tab_right,
  swap_pane_left,
  swap_pane_right,
  swap_pane_up,
  swap_pane_down,
  close_tab,
  select_tab_0,
  select_tab_1,
  select_tab_2,
  select_tab_3,
  select_tab_4,
  select_tab_5,
  select_tab_6,
  select_tab_7,
  select_tab_8,
  select_tab_9,
  copy_cancel_or_leave,
  copy_leave,
  copy_cancel_selection,
  copy_move_left,
  copy_move_down,
  copy_move_up,
  copy_move_right,
  copy_word_left,
  copy_word_right,
  copy_word_end,
  copy_line_start,
  copy_line_first_nonblank,
  copy_line_end,
  copy_history_top,
  copy_history_bottom,
  copy_viewport_top,
  copy_viewport_middle,
  copy_viewport_bottom,
  copy_half_page_up,
  copy_half_page_down,
  copy_page_up,
  copy_page_down,
  copy_visual_character,
  copy_visual_line,
  copy_visual_block,
  copy_swap_endpoint,
  copy_repeat_search,
  copy_reverse_search,
  copy_cancel_search,
  copy_commit_search,
  copy_query_backspace,
  rename_cancel,
  rename_commit,
  rename_backspace,
  rename_delete,
  rename_cursor_left,
  rename_cursor_right,
  rename_cursor_home,
  rename_cursor_end,
  rename_clear,
  rename_delete_word,
  command_line_cancel,
  command_line_commit,
  command_line_complete,
  command_line_history_previous,
  command_line_history_next,
  command_line_backspace,
  command_line_delete,
  command_line_cursor_left,
  command_line_cursor_right,
  command_line_cursor_home,
  command_line_cursor_end,
  command_line_clear,
  command_line_delete_word,
  count,
};

class InputContextId final {
public:
  constexpr InputContextId() noexcept = default;
  [[nodiscard]] constexpr auto valid() const noexcept -> bool { return slot_ != invalid_slot; }
  [[nodiscard]] constexpr auto operator==(const InputContextId&) const noexcept -> bool = default;

private:
  static constexpr std::uint8_t invalid_slot = 0xFFU;
  explicit constexpr InputContextId(const std::uint8_t slot) noexcept : slot_(slot) {}

  std::uint8_t slot_{invalid_slot};

  friend class CompiledInputMap;
  friend class InputMapDraft;
  friend class InputRouter;
};

enum class ContextLifetime : std::uint8_t {
  persistent,
  one_shot,
};

enum class UnboundBehavior : std::uint8_t {
  forward,
  replay_deferred,
  consume,
  retry_base,
};

struct ContextOptions final {
  std::string_view label;
  ContextLifetime lifetime{ContextLifetime::persistent};
  UnboundBehavior unbound{UnboundBehavior::forward};
  bool preempts_interaction{false};
};

enum class ChordKind : std::uint8_t {
  byte,
  key,
};

struct InputChord final {
  [[nodiscard]] static constexpr auto byte(const std::uint8_t value,
                                           const std::uint16_t modifiers = 0) noexcept
      -> InputChord {
    return {.code = value, .modifiers = modifiers, .kind = ChordKind::byte};
  }
  [[nodiscard]] static constexpr auto key(const PhysicalKey value,
                                          const std::uint16_t modifiers = 0) noexcept
      -> InputChord {
    return {
        .code = static_cast<std::uint16_t>(value), .modifiers = modifiers, .kind = ChordKind::key};
  }

  std::uint16_t code{0};
  std::uint16_t modifiers{0};
  ChordKind kind{ChordKind::byte};

  [[nodiscard]] constexpr auto operator<=>(const InputChord&) const noexcept = default;
};

enum class CommandContextDisposition : std::uint8_t {
  retain,
  base,
};

struct CommandBinding final {
  InputCommand command{InputCommand::detach};
  CommandContextDisposition context{CommandContextDisposition::retain};
};

struct HostedCommandBinding final {
  std::uint8_t index{0};
  CommandContextDisposition context{CommandContextDisposition::retain};
};

struct PushContextBinding final {
  InputContextId context;
  std::array<std::byte, deferred_input_bytes_max> deferred{};
  std::uint8_t deferred_size{0};
};

struct PopContextBinding final {};
struct ForwardDeferredBinding final {};
struct EncodeAsBinding final {
  PhysicalKey key{PhysicalKey::unidentified};
  std::uint16_t modifiers{0};
};

using BindingAction = std::variant<CommandBinding, PushContextBinding, PopContextBinding,
                                   ForwardDeferredBinding, EncodeAsBinding, HostedCommandBinding>;

enum class InputMapError : std::uint8_t {
  capacity,
  invalid_context,
  invalid_chord,
  invalid_action,
  invalid_options,
  duplicate_binding,
  context_depth,
  missing_base,
};

class CompiledInputMap final {
public:
  CompiledInputMap(const CompiledInputMap&) = delete;
  CompiledInputMap(CompiledInputMap&&) noexcept = default;
  auto operator=(const CompiledInputMap&) -> CompiledInputMap& = delete;
  auto operator=(CompiledInputMap&&) noexcept -> CompiledInputMap& = default;
  ~CompiledInputMap() = default;

private:
  struct Context final {
    std::array<char, input_context_label_bytes_max> label{};
    std::uint8_t label_size{0};
    std::uint8_t binding_begin{0};
    std::uint8_t binding_count{0};
    ContextLifetime lifetime{ContextLifetime::persistent};
    UnboundBehavior unbound{UnboundBehavior::forward};
    std::array<std::uint64_t, 4> legacy_trigger_bytes{};
    bool preempts_interaction{false};
    bool legacy_checkpoint_required{false};
  };

  struct Binding final {
    InputContextId context;
    InputChord chord;
    BindingAction action;
  };

  CompiledInputMap() noexcept = default;

  std::array<Context, input_contexts_max> contexts_{};
  std::array<Binding, input_bindings_max> bindings_{};
  std::uint8_t context_count_{0};
  std::uint8_t binding_count_{0};

  friend class InputMapDraft;
  friend class InputRouter;
};

class InputMapDraft final {
public:
  [[nodiscard]] auto add_context(ContextOptions options) noexcept
      -> std::expected<InputContextId, InputMapError>;
  [[nodiscard]] auto bind(InputContextId context, InputChord chord, BindingAction action) noexcept
      -> bool;
  // Configuration generations use replacement semantics explicitly. bind() remains append-only so
  // duplicate declarations are rejected when a draft is compiled.
  [[nodiscard]] auto set(InputContextId context, InputChord chord, BindingAction action) noexcept
      -> bool;
  [[nodiscard]] auto unbind(InputContextId context, InputChord chord) noexcept -> bool;
  [[nodiscard]] auto compile() const noexcept -> std::expected<CompiledInputMap, InputMapError>;

private:
  struct DraftContext final {
    std::array<char, input_context_label_bytes_max> label{};
    std::uint8_t label_size{0};
    ContextLifetime lifetime{ContextLifetime::persistent};
    UnboundBehavior unbound{UnboundBehavior::forward};
    bool preempts_interaction{false};
  };

  struct DraftBinding final {
    InputContextId context;
    InputChord chord;
    BindingAction action;
  };

  std::array<DraftContext, input_contexts_max> contexts_{};
  std::array<DraftBinding, input_bindings_max> bindings_{};
  std::uint8_t context_count_{0};
  std::uint8_t binding_count_{0};
};

[[nodiscard]] auto push_context(InputContextId context,
                                std::span<const std::byte> deferred = {}) noexcept
    -> std::expected<BindingAction, InputMapError>;
[[nodiscard]] constexpr auto
invoke(const InputCommand command,
       const CommandContextDisposition context = CommandContextDisposition::retain) noexcept
    -> BindingAction {
  return CommandBinding{.command = command, .context = context};
}
[[nodiscard]] constexpr auto pop_context() noexcept -> BindingAction { return PopContextBinding{}; }
[[nodiscard]] constexpr auto forward_deferred() noexcept -> BindingAction {
  return ForwardDeferredBinding{};
}
[[nodiscard]] constexpr auto encode_as(const PhysicalKey key,
                                       const std::uint16_t modifiers = 0) noexcept
    -> BindingAction {
  return EncodeAsBinding{.key = key, .modifiers = modifiers};
}

struct ConsumedInput final {};
struct RoutedCommand final {
  InputCommand command{InputCommand::detach};
};

struct RoutedHostedCommand final {
  std::uint8_t index{0};
};

struct ForwardLegacyInput final {
  std::array<std::byte, deferred_input_bytes_max> prefix{};
  std::uint8_t prefix_size{0};
  std::span<const std::byte> current;
};

struct ForwardCurrentKey final {};
struct ForwardBytes final {
  std::array<std::byte, deferred_input_bytes_max> bytes{};
  std::uint8_t size{0};
};
struct ForwardBytesThenCurrentKey final {
  std::array<std::byte, deferred_input_bytes_max> bytes{};
  std::uint8_t size{0};
};
struct EncodeAsKey final {
  PhysicalKey key{PhysicalKey::unidentified};
  std::uint16_t modifiers{0};
};

using LegacyRouteEffect =
    std::variant<ConsumedInput, RoutedCommand, ForwardLegacyInput, RoutedHostedCommand>;
using KeyRouteEffect = std::variant<ConsumedInput, RoutedCommand, ForwardCurrentKey, ForwardBytes,
                                    ForwardBytesThenCurrentKey, EncodeAsKey, RoutedHostedCommand>;

struct LegacyRouteResult final {
  LegacyRouteEffect effect;
  std::size_t consumed{0};
  bool presentation_changed{false};
  bool interaction_preemption_requested{false};
};

struct KeyRouteResult final {
  KeyRouteEffect effect;
  bool presentation_changed{false};
  bool interaction_preemption_requested{false};
};

enum class ConfiguredInputContext : std::uint8_t;

class InputRouter final {
public:
  explicit InputRouter(const CompiledInputMap& map) noexcept;

  void reset() noexcept;
  // Drop transient contexts/deferred triggers, but preserve press-time release ownership.
  void reconfigure(const CompiledInputMap& map) noexcept;
  void select_base(ConfiguredInputContext selected) noexcept;
  [[nodiscard]] auto active_label() const noexcept -> std::string_view;
  [[nodiscard]] auto unbound() const noexcept -> UnboundBehavior {
    return context(active_frame().context).unbound;
  }
  [[nodiscard]] auto legacy_route_requires_checkpoint() const noexcept -> bool {
    const auto& frame = std::span(stack_).subspan(depth_ - 1U, 1).front();
    const auto& metadata = std::span(map_->contexts_).subspan(frame.context.slot_, 1).front();
    return metadata.legacy_checkpoint_required;
  }
  [[nodiscard]] auto route_legacy(std::span<const std::byte> input,
                                  std::size_t forward_limit) noexcept -> LegacyRouteResult;
  [[nodiscard]] auto route_key(const KeyEvent& event) noexcept -> KeyRouteResult;

private:
  struct ContextFrame final {
    InputContextId context;
    std::array<std::byte, deferred_input_bytes_max> deferred{};
    std::uint8_t deferred_size{0};
  };

  [[nodiscard]] auto active_frame() noexcept -> ContextFrame&;
  [[nodiscard]] auto active_frame() const noexcept -> const ContextFrame&;
  [[nodiscard]] auto context(InputContextId id) const noexcept -> const CompiledInputMap::Context&;
  [[nodiscard]] auto binding(InputContextId context, InputChord chord) const noexcept
      -> const CompiledInputMap::Binding*;
  [[nodiscard]] auto push(InputContextId context, std::span<const std::byte> deferred) noexcept
      -> bool;
  void pop() noexcept;
  void return_to_base() noexcept;
  [[nodiscard]] auto visible_context_changed(InputContextId before) const noexcept -> bool;
  [[nodiscard]] auto captured(PhysicalKey key) const noexcept -> bool;
  [[nodiscard]] auto forwarded(PhysicalKey key) const noexcept -> bool;
  [[nodiscard]] auto captured_context(PhysicalKey key) const noexcept -> InputContextId;
  void capture(PhysicalKey key, InputContextId context) noexcept;
  void forward(PhysicalKey key) noexcept;
  void release(PhysicalKey key) noexcept;

  const CompiledInputMap* map_;
  std::array<ContextFrame, input_context_stack_max> stack_{};
  std::array<std::uint8_t, (static_cast<std::size_t>(PhysicalKey::count) + 1U) / 2U>
      captured_contexts_{};
  std::uint8_t depth_{0};
  // One press-time rewrite is enough: host line-motion chords are not held together.
  PhysicalKey encoded_hold_from_{PhysicalKey::unidentified};
  PhysicalKey encoded_hold_to_{PhysicalKey::unidentified};
  std::uint16_t encoded_hold_modifiers_{0};
  std::uint64_t captured_keys_{0};
  std::uint64_t forwarded_keys_{0};
};

enum class ConfiguredInputContext : std::uint8_t {
  normal,
  prefix,
  resize,
  copy,
  copy_go,
  copy_search,
  copy_searching,
  rename,
  command_line,
  messages,
  count,
};

enum class InputMapPreset : std::uint8_t {
  defaults,
  none,
};

enum class ConfiguredBindingKind : std::uint8_t {
  command,
  push_context,
  pop_context,
  replay_deferred,
  send_key,
  hosted_command,
};

struct ConfiguredBindingAction final {
  ConfiguredBindingKind kind{ConfiguredBindingKind::command};
  InputCommand command{InputCommand::detach};
  ConfiguredInputContext target{ConfiguredInputContext::normal};
  CommandContextDisposition disposition{CommandContextDisposition::retain};
  PhysicalKey encoded_key{PhysicalKey::unidentified};
  std::uint16_t encoded_modifiers{0};
  bool defer_chord{false};
  std::uint8_t hosted_command{0};
};

struct ConfiguredBinding final {
  ConfiguredInputContext context{ConfiguredInputContext::normal};
  InputChord chord;
  ConfiguredBindingAction action;
};

struct ContextConfiguration final {
  std::array<char, input_context_label_bytes_max> label{};
  std::uint8_t label_size{0};
  ContextLifetime lifetime{ContextLifetime::persistent};
  UnboundBehavior unbound{UnboundBehavior::forward};
  bool preempts_interaction{false};
};

struct InputMapConfiguration final {
  InputMapConfiguration() noexcept;

  std::array<ConfiguredBinding, input_bindings_max> bindings{};
  std::array<ContextConfiguration, static_cast<std::size_t>(ConfiguredInputContext::count)>
      contexts{};
  std::optional<InputChord> prefix;
  InputMapPreset preset{InputMapPreset::defaults};
  std::uint8_t binding_count{0};

  void reset(InputMapPreset selected) noexcept;
  [[nodiscard]] auto set_context(ConfiguredInputContext selected, ContextOptions options) noexcept
      -> bool;
  [[nodiscard]] auto set_prefix(std::optional<InputChord> chord) noexcept -> bool;
  [[nodiscard]] auto set_action(ConfiguredInputContext context, InputChord chord,
                                ConfiguredBindingAction action) noexcept -> bool;
  [[nodiscard]] auto
  set(ConfiguredInputContext context, InputChord chord, InputCommand command,
      CommandContextDisposition disposition = CommandContextDisposition::retain) noexcept -> bool;
  [[nodiscard]] auto push(ConfiguredInputContext context, InputChord chord,
                          ConfiguredInputContext target, bool defer_chord = false) noexcept -> bool;
  [[nodiscard]] auto pop(ConfiguredInputContext context, InputChord chord) noexcept -> bool;
  [[nodiscard]] auto replay(ConfiguredInputContext context, InputChord chord) noexcept -> bool;
  [[nodiscard]] auto send(ConfiguredInputContext context, InputChord chord, PhysicalKey key,
                          std::uint16_t modifiers = 0) noexcept -> bool;
  [[nodiscard]] auto unbind(ConfiguredInputContext context, InputChord chord) noexcept -> bool;
};

// Compiles one complete immutable input-policy generation. Publication owns the returned value for
// longer than every InputRouter borrowing it.
[[nodiscard]] auto compile_input_map(const InputMapConfiguration& configuration) noexcept
    -> std::expected<CompiledInputMap, InputMapError>;
[[nodiscard]] auto default_input_map() noexcept -> const CompiledInputMap&;

} // namespace frame::input

#endif // FRAME_INPUT_INPUT_ROUTER_HPP
