#ifndef LEMMA_API_COMMAND_HPP
#define LEMMA_API_COMMAND_HPP

#include "api/json.hpp"
#include "core/float_layer.hpp"
#include "lemma/id.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lemma::api {

inline constexpr std::string_view command_result_schema = "lemma.command-result/v1";
inline constexpr std::string_view events_schema = "lemma.events/v1";
inline constexpr std::string_view event_schema = "lemma.event/v1";

enum class CommandKind : std::uint8_t {
  daemon_inspect,
  config_reload,
  attachment_switch,
  session_list,
  session_inspect,
  session_start,
  session_rename,
  session_kill,
  tab_list,
  tab_inspect,
  tab_new,
  tab_select,
  tab_move,
  tab_rename,
  tab_kill,
  tab_floats,
  pane_list,
  pane_inspect,
  pane_split,
  pane_float,
  pane_place,
  pane_focus,
  pane_swap,
  pane_resize,
  pane_zoom,
  pane_send,
  pane_input,
  pane_paste_image,
  pane_capture,
  pane_wait,
  pane_kill,
  surface_create,
  surface_configure,
  surface_focus,
  surface_close,
};

enum class Direction : std::uint8_t {
  none,
  left,
  right,
  up,
  down,
};

enum class FocusPolicy : std::uint8_t {
  created,
  preserve,
};

enum class CaptureSource : std::uint8_t {
  visible,
  recent,
  last_command,
};

enum class CaptureFormat : std::uint8_t {
  plain,
  ansi,
};

enum class CaptureWrap : std::uint8_t {
  rendered,
  logical,
};

enum class WaitCondition : std::uint8_t {
  process_exit,
  exit_code,
  signal,
  contains,
  prompt,
  command,
};

inline constexpr std::uint32_t wait_timeout_default_milliseconds = 30'000;
inline constexpr std::uint32_t wait_timeout_max_milliseconds = 600'000;

[[nodiscard]] auto wait_condition_name(WaitCondition condition) noexcept -> std::string_view;

enum class InputEventKind : std::uint8_t {
  text,
  paste,
  key,
};

enum class InputKeyPhase : std::uint8_t {
  press,
  repeat,
  release,
};

enum class InputKey : std::uint8_t {
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
};

inline constexpr std::size_t input_events_max = 64;
inline constexpr std::uint16_t input_modifier_shift = 1U << 0U;
inline constexpr std::uint16_t input_modifier_control = 1U << 1U;
inline constexpr std::uint16_t input_modifier_alt = 1U << 2U;
inline constexpr std::uint16_t input_modifier_super = 1U << 3U;

struct InputEvent final {
  InputEventKind kind{InputEventKind::text};
  std::string text;
  InputKey key{InputKey::a};
  std::uint16_t modifiers{0};
  InputKeyPhase phase{InputKeyPhase::press};
};

struct SessionSelector final {
  SessionId id;
  std::string name;

  [[nodiscard]] auto valid() const noexcept -> bool { return id.is_valid() || !name.empty(); }
};

struct TabSelector final {
  TabId id;
  std::uint16_t position{0};

  [[nodiscard]] auto valid() const noexcept -> bool { return id.is_valid() || position > 0; }
};

struct PaneSelector final {
  PaneId id;

  [[nodiscard]] auto valid() const noexcept -> bool { return id.is_valid(); }
};

struct SurfaceSelector final {
  SurfaceId id;

  [[nodiscard]] auto valid() const noexcept -> bool { return id.is_valid(); }
};

enum class SurfacePlacementKind : std::uint8_t {
  dock_left,
  dock_right,
  dock_top,
  dock_bottom,
  float_surface,
  overlay,
};

struct SurfacePlacement final {
  SurfacePlacementKind kind{SurfacePlacementKind::dock_right};
  std::uint16_t column{0};
  std::uint16_t row{0};
  std::uint16_t columns{0};
  std::uint16_t rows{0};
};

// One concrete public Command. Command-specific decoding guarantees that only the fields belonging
// to kind are populated before this value crosses the daemon trust boundary.
struct Command final {
  CommandKind kind{CommandKind::session_list};
  SessionSelector session;
  ConnectionId connection;
  TabSelector tab;
  PaneSelector pane;
  PaneSelector other;
  SurfaceSelector surface;
  SurfacePlacement surface_placement;
  // Validated by the Core factories at decode time.
  std::optional<core::FloatPlacement> float_placement;
  std::optional<bool> configured_focusable;
  std::string name;
  std::string working_directory;
  std::string title;
  std::string text;
  std::string contains;
  std::vector<std::string> arguments;
  std::vector<std::string> environment;
  std::vector<InputEvent> input_events;
  Direction direction{Direction::none};
  FocusPolicy focus{FocusPolicy::created};
  CaptureSource capture_source{CaptureSource::visible};
  CaptureFormat capture_format{CaptureFormat::plain};
  CaptureWrap capture_wrap{CaptureWrap::rendered};
  WaitCondition wait_condition{WaitCondition::process_exit};
  std::uint64_t after_terminal_generation{0};
  std::optional<std::uint64_t> after_commands;
  std::uint32_t wait_value{0};
  std::uint32_t wait_timeout_milliseconds{wait_timeout_default_milliseconds};
  std::uint16_t amount{0};
  std::uint16_t lines{0};
  std::uint16_t to_position{0};
  std::optional<std::uint64_t> expected_session_revision;
  bool hold{false};
  bool enabled{false};
  bool visible{false};
  bool environment_set{false};
  bool focusable{true};
  bool opaque{true};
};

inline constexpr std::size_t float_placement_json_bytes_max = 96;

// The closed public JSON object for a float placement, as accepted by pane.float and pane.place.
[[nodiscard]] auto
format_float_placement(core::FloatPlacement placement,
                       std::span<char, float_placement_json_bytes_max> output) noexcept
    -> std::string_view;

[[nodiscard]] auto parse_input_key_name(std::string_view value) noexcept -> std::optional<InputKey>;
[[nodiscard]] auto input_key_name(InputKey key) noexcept -> std::string_view;
[[nodiscard]] auto append_capture(std::string& output, CaptureSource source, CaptureFormat format,
                                  CaptureWrap wrap, std::uint64_t terminal_generation,
                                  bool truncated, std::string_view text) -> bool;

struct CommandDecodeError final {
  std::string_view reason;
  std::string_view field;
};

struct CommandDecodeResult final {
  std::optional<Command> command;
  CommandDecodeError error;
};

[[nodiscard]] auto decode_command(const JsonValue& document) -> CommandDecodeResult;
[[nodiscard]] auto encode_command(const Command& command) -> std::optional<std::string>;
[[nodiscard]] auto command_name(CommandKind kind) noexcept -> std::string_view;

inline constexpr std::size_t event_panes_max = 8;

struct EventSubscription final {
  std::optional<SessionSelector> session;
  std::vector<PaneSelector> panes;
  bool screen{false};
  bool presentation{false};
  bool signals{false};
};

struct EventSubscriptionDecodeResult final {
  std::optional<EventSubscription> subscription;
  CommandDecodeError error;
};

[[nodiscard]] auto decode_event_subscription(const JsonValue& document)
    -> EventSubscriptionDecodeResult;

} // namespace lemma::api

#endif // LEMMA_API_COMMAND_HPP
