#include "input/input_router.hpp"

#include "frame/assert.hpp"
#include "frame/limits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace frame::input {
namespace {

[[nodiscard]] constexpr auto chord_valid(const InputChord chord) noexcept -> bool {
  if ((chord.modifiers & ~key_modifiers_all) != 0) {
    return false;
  }
  switch (chord.kind) {
  case ChordKind::byte:
    return chord.code <= 0xFFU;
  case ChordKind::key:
    return chord.code < static_cast<std::uint16_t>(PhysicalKey::count);
  }
  return false;
}

[[nodiscard]] constexpr auto context_options_valid(const ContextOptions options) noexcept -> bool {
  const bool lifetime_valid = options.lifetime == ContextLifetime::persistent ||
                              options.lifetime == ContextLifetime::one_shot;
  const bool unbound_valid = options.unbound == UnboundBehavior::forward ||
                             options.unbound == UnboundBehavior::replay_deferred ||
                             options.unbound == UnboundBehavior::consume ||
                             (options.unbound == UnboundBehavior::retry_base &&
                              options.lifetime == ContextLifetime::one_shot);
  const bool label_valid = std::ranges::all_of(options.label, [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte >= 0x20U && byte <= 0x7EU;
  });
  return lifetime_valid && unbound_valid && label_valid;
}

[[nodiscard]] constexpr auto command_valid(const InputCommand command) noexcept -> bool {
  return static_cast<std::uint8_t>(command) < static_cast<std::uint8_t>(InputCommand::count);
}

[[nodiscard]] constexpr auto command_context_valid(const CommandContextDisposition context) noexcept
    -> bool {
  return context == CommandContextDisposition::retain || context == CommandContextDisposition::base;
}

[[nodiscard]] constexpr auto encode_as_valid(const EncodeAsBinding encoded) noexcept -> bool {
  return encoded.key != PhysicalKey::unidentified && encoded.key < PhysicalKey::count &&
         (encoded.modifiers & ~key_modifiers_all) == 0;
}

[[nodiscard]] constexpr auto encode_as_chord_valid(const InputChord chord) noexcept -> bool {
  return chord.kind == ChordKind::key && chord.modifiers != 0;
}

[[nodiscard]] constexpr auto chord_less(const InputChord left, const InputChord right) noexcept
    -> bool {
  return std::tuple{left.kind, left.code, left.modifiers} <
         std::tuple{right.kind, right.code, right.modifiers};
}

[[nodiscard]] constexpr auto command_modifiers(const std::uint16_t modifiers) noexcept
    -> std::uint16_t {
  return static_cast<std::uint16_t>(modifiers & ~(key_modifier_caps_lock | key_modifier_num_lock));
}

[[nodiscard]] constexpr auto legacy_chord(const std::byte byte) noexcept -> InputChord {
  const auto value = std::to_integer<std::uint8_t>(byte);
  if (value == 0x0DU || value == 0x09U || value == 0x7FU) {
    return InputChord::byte(value);
  }
  if (value >= 1U && value <= 26U) {
    return InputChord::byte(static_cast<std::uint8_t>('a' + value - 1U), key_modifier_control);
  }
  return InputChord::byte(value);
}

[[nodiscard]] constexpr auto legacy_trigger_byte(const InputChord chord) noexcept
    -> std::optional<std::uint8_t> {
  if ((chord.modifiers & key_modifier_alt) != 0U) {
    return 0x1BU;
  }
  if (chord.kind == ChordKind::key) {
    const auto key = static_cast<PhysicalKey>(chord.code);
    const bool escape_sequence = key == PhysicalKey::arrow_up || key == PhysicalKey::arrow_down ||
                                 key == PhysicalKey::arrow_left ||
                                 key == PhysicalKey::arrow_right || key == PhysicalKey::home ||
                                 key == PhysicalKey::end;
    return escape_sequence ? std::optional<std::uint8_t>{0x1BU} : std::nullopt;
  }
  if (chord.modifiers == 0U) {
    return static_cast<std::uint8_t>(chord.code);
  }
  if (chord.modifiers == key_modifier_control && chord.code >= 'a' && chord.code <= 'z') {
    return static_cast<std::uint8_t>(chord.code - 'a' + 1U);
  }
  return std::nullopt;
}

[[nodiscard]] constexpr auto legacy_triggered(const std::array<std::uint64_t, 4>& triggers,
                                              const std::byte byte) noexcept -> bool {
  const auto value = std::to_integer<std::uint8_t>(byte);
  const auto word = static_cast<std::size_t>(value / 64U);
  const auto bit = static_cast<unsigned>(value % 64U);
  return (std::span(triggers).subspan(word, 1).front() & (std::uint64_t{1} << bit)) != 0U;
}

[[nodiscard]] auto key_chord(const KeyEvent& event) noexcept -> InputChord {
  const auto modifiers = command_modifiers(event.modifiers);
  if (event.key == PhysicalKey::enter) {
    return InputChord::byte(0x0DU, modifiers);
  }
  if (event.key == PhysicalKey::tab) {
    return InputChord::byte(0x09U, modifiers);
  }
  if (event.key == PhysicalKey::backspace) {
    return InputChord::byte(0x7FU, modifiers);
  }
  if (event.key == PhysicalKey::escape) {
    return InputChord::byte(0x1BU, modifiers);
  }
  const bool text_ascii =
      event.text.size() == 1U && std::to_integer<std::uint8_t>(event.text.front()) <= 0x7FU;
  const bool unshifted_ascii = event.unshifted_codepoint > 0U && event.unshifted_codepoint <= 0x7FU;
  const auto non_shift_modifiers = static_cast<std::uint16_t>(modifiers & ~key_modifier_shift);
  if ((text_ascii || unshifted_ascii) && non_shift_modifiers == 0) {
    const auto value = text_ascii ? std::to_integer<std::uint8_t>(event.text.front())
                                  : static_cast<std::uint8_t>(event.unshifted_codepoint);
    return InputChord::byte(value);
  }
  if (unshifted_ascii) {
    return InputChord::byte(static_cast<std::uint8_t>(event.unshifted_codepoint), modifiers);
  }
  if (text_ascii) {
    return InputChord::byte(std::to_integer<std::uint8_t>(event.text.front()), modifiers);
  }
  return InputChord::key(event.key, modifiers);
}

[[nodiscard]] constexpr auto legacy_special_key(const std::span<const std::byte> input) noexcept
    -> std::optional<PhysicalKey> {
  if (input.size() < 3U || input.front() != std::byte{0x1B} ||
      (input.subspan(1, 1).front() != std::byte{'['} &&
       input.subspan(1, 1).front() != std::byte{'O'})) {
    return std::nullopt;
  }
  switch (input.subspan(2, 1).front()) {
  case std::byte{'A'}:
    return PhysicalKey::arrow_up;
  case std::byte{'B'}:
    return PhysicalKey::arrow_down;
  case std::byte{'C'}:
    return PhysicalKey::arrow_right;
  case std::byte{'D'}:
    return PhysicalKey::arrow_left;
  case std::byte{'H'}:
    return PhysicalKey::home;
  case std::byte{'F'}:
    return PhysicalKey::end;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] constexpr auto key_bit(const PhysicalKey key) noexcept -> std::uint64_t {
  const auto index = static_cast<std::uint8_t>(key);
  return index < 64U ? std::uint64_t{1} << index : 0U;
}

struct EncodedPrefix final {
  std::array<std::byte, deferred_input_bytes_max> bytes{};
  std::uint8_t size{0};
};

// Prefix size is checked against fixed storage before each access.
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] constexpr auto encode_prefix(const InputChord prefix) noexcept
    -> std::optional<EncodedPrefix> {
  if (!chord_valid(prefix) || prefix.kind != ChordKind::byte ||
      (prefix.modifiers & (key_modifier_shift | key_modifier_super | key_modifier_caps_lock |
                           key_modifier_num_lock)) != 0U) {
    return std::nullopt;
  }
  EncodedPrefix encoded;
  if ((prefix.modifiers & key_modifier_alt) != 0U) {
    encoded.bytes.at(encoded.size++) = std::byte{0x1B};
  }
  auto value = static_cast<std::uint8_t>(prefix.code);
  if ((prefix.modifiers & key_modifier_control) != 0U) {
    if (value < static_cast<std::uint8_t>('a') || value > static_cast<std::uint8_t>('z')) {
      return std::nullopt;
    }
    value = static_cast<std::uint8_t>(value - static_cast<std::uint8_t>('a') + 1U);
  }
  encoded.bytes.at(encoded.size++) = static_cast<std::byte>(value);
  return encoded;
}

} // namespace

auto InputMapDraft::add_context(const ContextOptions options) noexcept
    -> std::expected<InputContextId, InputMapError> {
  if (context_count_ >= contexts_.size()) {
    return std::unexpected(InputMapError::capacity);
  }
  if (options.label.size() > input_context_label_bytes_max || !context_options_valid(options)) {
    return std::unexpected(InputMapError::invalid_options);
  }
  if (context_count_ == 0U &&
      (options.lifetime != ContextLifetime::persistent ||
       options.unbound != UnboundBehavior::forward || options.preempts_interaction)) {
    return std::unexpected(InputMapError::invalid_options);
  }
  auto& context = std::span(contexts_).subspan(context_count_, 1).front();
  if (!options.label.empty()) {
    std::memcpy(context.label.data(), options.label.data(), options.label.size());
  }
  context.label_size = static_cast<std::uint8_t>(options.label.size());
  context.lifetime = options.lifetime;
  context.unbound = options.unbound;
  context.preempts_interaction = options.preempts_interaction;
  const auto result = InputContextId(context_count_);
  ++context_count_;
  return result;
}

auto InputMapDraft::bind(const InputContextId context, const InputChord chord,
                         BindingAction action) noexcept -> bool {
  if (!context.valid() || context.slot_ >= context_count_ || !chord_valid(chord) ||
      binding_count_ >= bindings_.size()) {
    return false;
  }
  auto& binding = std::span(bindings_).subspan(binding_count_, 1).front();
  binding = {.context = context, .chord = chord, .action = action};
  ++binding_count_;
  return true;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
auto InputMapDraft::set(const InputContextId context, const InputChord chord,
                        BindingAction action) noexcept -> bool {
  if (!context.valid() || context.slot_ >= context_count_ || !chord_valid(chord)) {
    return false;
  }
  for (std::size_t index = 0; index < binding_count_; ++index) {
    auto& binding = std::span(bindings_).subspan(index, 1).front();
    if (binding.context == context && binding.chord == chord) {
      binding.action = action;
      return true;
    }
  }
  return bind(context, chord, action);
}

auto InputMapDraft::unbind(const InputContextId context, const InputChord chord) noexcept -> bool {
  if (!context.valid() || context.slot_ >= context_count_ || !chord_valid(chord)) {
    return false;
  }
  for (std::size_t index = 0; index < binding_count_; ++index) {
    const auto& binding = std::span(bindings_).subspan(index, 1).front();
    if (binding.context != context || binding.chord != chord) {
      continue;
    }
    for (std::size_t shifted = index + 1U; shifted < binding_count_; ++shifted) {
      std::span(bindings_).subspan(shifted - 1U, 1).front() =
          std::span(bindings_).subspan(shifted, 1).front();
    }
    --binding_count_;
    return true;
  }
  return true;
}

// Candidate validation is exhaustive and runs only at generation publication.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto InputMapDraft::compile() const noexcept -> std::expected<CompiledInputMap, InputMapError> {
  if (context_count_ == 0U) {
    return std::unexpected(InputMapError::missing_base);
  }
  CompiledInputMap result;
  result.context_count_ = context_count_;
  result.binding_count_ = binding_count_;
  for (std::size_t index = 0; index < context_count_; ++index) {
    const auto& source = std::span(contexts_).subspan(index, 1).front();
    auto& target = std::span(result.contexts_).subspan(index, 1).front();
    target.label = source.label;
    target.label_size = source.label_size;
    target.lifetime = source.lifetime;
    target.unbound = source.unbound;
    target.preempts_interaction = source.preempts_interaction;
    target.legacy_checkpoint_required =
        source.lifetime == ContextLifetime::one_shot && source.unbound != UnboundBehavior::consume;
  }
  for (std::size_t index = 0; index < binding_count_; ++index) {
    const auto& source = std::span(bindings_).subspan(index, 1).front();
    if (!source.context.valid() || source.context.slot_ >= context_count_) {
      return std::unexpected(InputMapError::invalid_context);
    }
    if (!chord_valid(source.chord)) {
      return std::unexpected(InputMapError::invalid_chord);
    }
    if (const auto* const command = std::get_if<CommandBinding>(&source.action);
        command != nullptr &&
        (!command_valid(command->command) || !command_context_valid(command->context))) {
      return std::unexpected(InputMapError::invalid_action);
    }
    if (const auto* const hosted = std::get_if<HostedCommandBinding>(&source.action);
        hosted != nullptr && (hosted->index >= limits::hosted_commands_hard_max ||
                              !command_context_valid(hosted->context))) {
      return std::unexpected(InputMapError::invalid_action);
    }
    if (const auto* const encoded = std::get_if<EncodeAsBinding>(&source.action);
        encoded != nullptr &&
        (!encode_as_valid(*encoded) || !encode_as_chord_valid(source.chord))) {
      return std::unexpected(InputMapError::invalid_action);
    }
    if (const auto* const pushed = std::get_if<PushContextBinding>(&source.action);
        pushed != nullptr && (!pushed->context.valid() || pushed->context.slot_ >= context_count_ ||
                              pushed->deferred_size > pushed->deferred.size())) {
      return std::unexpected(InputMapError::invalid_context);
    }
    std::span(result.bindings_).subspan(index, 1).front() = {
        .context = source.context, .chord = source.chord, .action = source.action};
  }
  const auto exceeds_context_depth = [&](const auto& self, const std::uint8_t current,
                                         const std::uint16_t visited,
                                         const std::size_t depth) noexcept -> bool {
    if (depth > input_context_stack_max) {
      return true;
    }
    for (const auto& candidate : std::span(bindings_).first(binding_count_)) {
      if (candidate.context.slot_ != current) {
        continue;
      }
      const auto* const pushed = std::get_if<PushContextBinding>(&candidate.action);
      if (pushed == nullptr) {
        continue;
      }
      const auto bit = static_cast<std::uint16_t>(std::uint16_t{1} << pushed->context.slot_);
      if ((visited & bit) == 0U && self(self, pushed->context.slot_,
                                        static_cast<std::uint16_t>(visited | bit), depth + 1U)) {
        return true;
      }
    }
    return false;
  };
  for (std::uint8_t root = 0; root < context_count_; ++root) {
    const auto visited = static_cast<std::uint16_t>(std::uint16_t{1} << root);
    if (exceeds_context_depth(exceeds_context_depth, root, visited, 1U)) {
      return std::unexpected(InputMapError::context_depth);
    }
  }

  auto bindings = std::span(result.bindings_).first(binding_count_);
  std::ranges::sort(bindings, [](const auto& left, const auto& right) {
    if (left.context.slot_ != right.context.slot_) {
      return left.context.slot_ < right.context.slot_;
    }
    return chord_less(left.chord, right.chord);
  });
  for (std::size_t index = 1; index < bindings.size(); ++index) {
    const auto& previous = bindings.subspan(index - 1U, 1).front();
    const auto& current = bindings.subspan(index, 1).front();
    if (previous.context == current.context && previous.chord == current.chord) {
      return std::unexpected(InputMapError::duplicate_binding);
    }
  }
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    auto& context = std::span(result.contexts_)
                        .subspan(bindings.subspan(index, 1).front().context.slot_, 1)
                        .front();
    if (context.binding_count == 0U) {
      context.binding_begin = static_cast<std::uint8_t>(index);
    }
    ++context.binding_count;
    const auto& binding = bindings.subspan(index, 1).front();
    if (context.lifetime == ContextLifetime::one_shot &&
        std::holds_alternative<ForwardDeferredBinding>(binding.action)) {
      context.legacy_checkpoint_required = true;
    }
    if (const auto trigger = legacy_trigger_byte(binding.chord); trigger.has_value()) {
      const auto word = static_cast<std::size_t>(*trigger / 64U);
      const auto bit = static_cast<unsigned>(*trigger % 64U);
      std::span(context.legacy_trigger_bytes).subspan(word, 1).front() |= std::uint64_t{1} << bit;
    }
  }
  return result;
}

auto push_context(const InputContextId context, const std::span<const std::byte> deferred) noexcept
    -> std::expected<BindingAction, InputMapError> {
  if (!context.valid() || deferred.size() > deferred_input_bytes_max) {
    return std::unexpected(InputMapError::invalid_context);
  }
  PushContextBinding result{.context = context,
                            .deferred_size = static_cast<std::uint8_t>(deferred.size())};
  std::ranges::copy(deferred, result.deferred.begin());
  return BindingAction{result};
}

InputRouter::InputRouter(const CompiledInputMap& map) noexcept : map_(&map) { reset(); }

void InputRouter::reset() noexcept {
  FRAME_ASSERT(map_ != nullptr && map_->context_count_ > 0U);
  stack_ = {};
  captured_contexts_ = {};
  stack_.front().context = InputContextId(0);
  depth_ = 1;
  encoded_hold_from_ = PhysicalKey::unidentified;
  encoded_hold_to_ = PhysicalKey::unidentified;
  encoded_hold_modifiers_ = 0;
  captured_keys_ = 0;
  forwarded_keys_ = 0;
}

void InputRouter::reconfigure(const CompiledInputMap& map) noexcept {
  map_ = &map;
  stack_ = {};
  stack_.front().context = InputContextId(0);
  depth_ = 1;
  // Captured repeats from the old policy cannot invoke new commands before their release.
  captured_contexts_.fill(0xFFU);
}

void InputRouter::select_base(const ConfiguredInputContext selected) noexcept {
  const auto slot = static_cast<std::uint8_t>(selected);
  FRAME_ASSERT(selected < ConfiguredInputContext::count && slot < map_->context_count_);
  const auto context_id = InputContextId(slot);
  if (stack_.front().context == context_id) {
    return;
  }
  stack_.front() = {.context = context_id};
  for (std::size_t index = 1; index < depth_; ++index) {
    std::span(stack_).subspan(index, 1).front() = {};
  }
  depth_ = 1;
}

auto InputRouter::active_frame() noexcept -> ContextFrame& {
  FRAME_ASSERT(depth_ > 0U && depth_ <= stack_.size());
  return std::span(stack_).subspan(depth_ - 1U, 1).front();
}

auto InputRouter::active_frame() const noexcept -> const ContextFrame& {
  FRAME_ASSERT(depth_ > 0U && depth_ <= stack_.size());
  return std::span(stack_).subspan(depth_ - 1U, 1).front();
}

auto InputRouter::context(const InputContextId id) const noexcept
    -> const CompiledInputMap::Context& {
  FRAME_ASSERT(map_ != nullptr && id.valid() && id.slot_ < map_->context_count_);
  return std::span(map_->contexts_).subspan(id.slot_, 1).front();
}

auto InputRouter::binding(const InputContextId context_id, const InputChord chord) const noexcept
    -> const CompiledInputMap::Binding* {
  const auto& metadata = context(context_id);
  const auto bindings =
      std::span(map_->bindings_).subspan(metadata.binding_begin, metadata.binding_count);
  const auto found =
      std::ranges::lower_bound(bindings, chord, &chord_less, &CompiledInputMap::Binding::chord);
  return found != bindings.end() && found->chord == chord ? std::to_address(found) : nullptr;
}

auto InputRouter::push(const InputContextId context_id,
                       const std::span<const std::byte> deferred) noexcept -> bool {
  if (!context_id.valid() || context_id.slot_ >= map_->context_count_ ||
      deferred.size() > deferred_input_bytes_max) {
    return false;
  }
  for (std::size_t index = 0; index < depth_; ++index) {
    if (std::span(stack_).subspan(index, 1).front().context == context_id) {
      depth_ = static_cast<std::uint8_t>(index + 1U);
      auto& frame = active_frame();
      frame.deferred = {};
      frame.deferred_size = static_cast<std::uint8_t>(deferred.size());
      std::ranges::copy(deferred, frame.deferred.begin());
      return true;
    }
  }
  if (depth_ >= stack_.size()) {
    return false;
  }
  auto& frame = std::span(stack_).subspan(depth_, 1).front();
  frame = {.context = context_id, .deferred_size = static_cast<std::uint8_t>(deferred.size())};
  std::ranges::copy(deferred, frame.deferred.begin());
  ++depth_;
  return true;
}

void InputRouter::pop() noexcept {
  if (depth_ <= 1U) {
    return;
  }
  --depth_;
  std::span(stack_).subspan(depth_, 1).front() = {};
}

void InputRouter::return_to_base() noexcept {
  while (depth_ > 1U) {
    pop();
  }
}

auto InputRouter::active_label() const noexcept -> std::string_view {
  const auto& metadata = context(active_frame().context);
  return {metadata.label.data(), metadata.label_size};
}

auto InputRouter::visible_context_changed(const InputContextId before) const noexcept -> bool {
  const auto& previous = context(before);
  const auto& current = context(active_frame().context);
  return std::string_view(previous.label.data(), previous.label_size) !=
         std::string_view(current.label.data(), current.label_size);
}

auto InputRouter::captured(const PhysicalKey key) const noexcept -> bool {
  return (captured_keys_ & key_bit(key)) != 0U;
}

auto InputRouter::forwarded(const PhysicalKey key) const noexcept -> bool {
  return (forwarded_keys_ & key_bit(key)) != 0U;
}

auto InputRouter::captured_context(const PhysicalKey key) const noexcept -> InputContextId {
  const auto index = static_cast<std::size_t>(key);
  FRAME_ASSERT(index < static_cast<std::size_t>(PhysicalKey::count) && captured(key));
  const auto packed = std::span(captured_contexts_).subspan(index / 2U, 1).front();
  const auto slot = index % 2U == 0U ? static_cast<std::uint8_t>(packed & 0x0FU)
                                     : static_cast<std::uint8_t>(packed >> 4U);
  return InputContextId(slot);
}

void InputRouter::capture(const PhysicalKey key, const InputContextId context_id) noexcept {
  const auto bit = key_bit(key);
  const auto index = static_cast<std::size_t>(key);
  if (bit == 0U || index >= static_cast<std::size_t>(PhysicalKey::count)) {
    return;
  }
  FRAME_ASSERT(context_id.valid() && context_id.slot_ < input_contexts_max && !forwarded(key));
  captured_keys_ |= bit;
  auto& packed = std::span(captured_contexts_).subspan(index / 2U, 1).front();
  if (index % 2U == 0U) {
    packed = static_cast<std::uint8_t>((packed & 0xF0U) | context_id.slot_);
  } else {
    const auto shifted = static_cast<std::uint8_t>(context_id.slot_ << 4U);
    packed = static_cast<std::uint8_t>((packed & 0x0FU) | shifted);
  }
}

void InputRouter::forward(const PhysicalKey key) noexcept {
  const auto bit = key_bit(key);
  if (bit == 0U) {
    return;
  }
  FRAME_ASSERT(!captured(key));
  forwarded_keys_ |= bit;
}

void InputRouter::release(const PhysicalKey key) noexcept {
  const auto bit = key_bit(key);
  captured_keys_ &= ~bit;
  forwarded_keys_ &= ~bit;
  const auto index = static_cast<std::size_t>(key);
  if (index >= static_cast<std::size_t>(PhysicalKey::count)) {
    return;
  }
  auto& packed = std::span(captured_contexts_).subspan(index / 2U, 1).front();
  packed = index % 2U == 0U ? static_cast<std::uint8_t>(packed & 0xF0U)
                            : static_cast<std::uint8_t>(packed & 0x0FU);
}

// Routing mutates only the bounded Attachment-owned context state. Mux mutation remains a typed
// command effect interpreted by the daemon's Core bridge.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
auto InputRouter::route_legacy(const std::span<const std::byte> input,
                               const std::size_t forward_limit) noexcept -> LegacyRouteResult {
  FRAME_ASSERT(!input.empty() && forward_limit > 0U);
  const auto before = active_frame().context;
  const auto& metadata = context(before);

  std::size_t chord_bytes = 1;
  const CompiledInputMap::Binding* matched = nullptr;
  if (legacy_triggered(metadata.legacy_trigger_bytes, input.front())) {
    const auto chord = legacy_chord(input.front());
    if (const auto special = legacy_special_key(input); special.has_value()) {
      const auto special_chord = InputChord::key(*special);
      if (const auto* const candidate = binding(before, special_chord); candidate != nullptr) {
        chord_bytes = 3;
        matched = candidate;
      }
    }
    if (matched == nullptr) {
      matched = binding(before, chord);
    }
    if (matched == nullptr && input.front() == std::byte{0x1B} && input.size() > 1U) {
      const auto next = legacy_chord(input.subspan(1, 1).front());
      const auto alt =
          InputChord{.code = next.code,
                     .modifiers = static_cast<std::uint16_t>(next.modifiers | key_modifier_alt),
                     .kind = next.kind};
      if (const auto* const candidate = binding(before, alt); candidate != nullptr) {
        chord_bytes = 2;
        matched = candidate;
      }
    }
  }

  if (matched == nullptr && metadata.unbound == UnboundBehavior::retry_base) {
    FRAME_ASSERT(metadata.lifetime == ContextLifetime::one_shot);
    pop();
    auto retried = route_legacy(input, forward_limit);
    retried.presentation_changed = retried.presentation_changed || visible_context_changed(before);
    return retried;
  }

  if (matched == nullptr && metadata.unbound == UnboundBehavior::forward) {
    std::size_t bytes = 1;
    const auto limit = std::min(input.size(), forward_limit);
    if (metadata.lifetime == ContextLifetime::persistent) {
      while (bytes < limit &&
             !legacy_triggered(metadata.legacy_trigger_bytes, input.subspan(bytes, 1).front())) {
        ++bytes;
      }
    } else {
      pop();
    }
    return {.effect = ForwardLegacyInput{.current = input.first(bytes)},
            .consumed = bytes,
            .presentation_changed = visible_context_changed(before)};
  }

  std::array<std::byte, deferred_input_bytes_max> deferred{};
  auto deferred_size = active_frame().deferred_size;
  std::ranges::copy(std::span(active_frame().deferred).first(deferred_size), deferred.begin());
  if (matched == nullptr) {
    if (metadata.lifetime == ContextLifetime::one_shot) {
      pop();
    }
    if (metadata.unbound == UnboundBehavior::replay_deferred) {
      return {.effect = ForwardLegacyInput{.prefix = deferred,
                                           .prefix_size = deferred_size,
                                           .current = input.first(1)},
              .consumed = 1,
              .presentation_changed = visible_context_changed(before)};
    }
    return {.effect = ConsumedInput{},
            .consumed = 1,
            .presentation_changed = visible_context_changed(before)};
  }

  if (metadata.lifetime == ContextLifetime::one_shot) {
    pop();
  }
  bool preempt_interaction = false;
  LegacyRouteEffect effect = ConsumedInput{};
  std::visit(
      [&](const auto& action) {
        using Action = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<Action, CommandBinding>) {
          if (action.context == CommandContextDisposition::base) {
            return_to_base();
          }
          effect = RoutedCommand{.command = action.command};
        } else if constexpr (std::is_same_v<Action, PushContextBinding>) {
          const auto pushed =
              push(action.context, std::span(action.deferred).first(action.deferred_size));
          FRAME_ASSERT(pushed);
          preempt_interaction = context(action.context).preempts_interaction;
        } else if constexpr (std::is_same_v<Action, PopContextBinding>) {
          pop();
        } else if constexpr (std::is_same_v<Action, ForwardDeferredBinding>) {
          if (deferred_size > 0U) {
            effect =
                ForwardLegacyInput{.prefix = deferred, .prefix_size = deferred_size, .current = {}};
          }
        } else if constexpr (std::is_same_v<Action, EncodeAsBinding>) {
          // Structured-key translation is not a legacy-byte effect.
        } else if constexpr (std::is_same_v<Action, HostedCommandBinding>) {
          if (action.context == CommandContextDisposition::base) {
            return_to_base();
          }
          effect = RoutedHostedCommand{.index = action.index};
        }
      },
      matched->action);
  return {.effect = effect,
          .consumed = chord_bytes,
          .presentation_changed = visible_context_changed(before),
          .interaction_preemption_requested = preempt_interaction};
}

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
auto InputRouter::route_key(const KeyEvent& event) noexcept -> KeyRouteResult {
  if (event.action == KeyAction::release) {
    const bool encoded_release =
        encoded_hold_from_ == event.key && event.key != PhysicalKey::unidentified;
    const auto encoded = EncodeAsKey{.key = encoded_hold_to_, .modifiers = encoded_hold_modifiers_};
    if (encoded_release) {
      encoded_hold_from_ = PhysicalKey::unidentified;
      encoded_hold_to_ = PhysicalKey::unidentified;
      encoded_hold_modifiers_ = 0;
    }
    const bool consumed = captured(event.key);
    release(event.key);
    if (encoded_release) {
      return {.effect = encoded};
    }
    return {.effect =
                consumed ? KeyRouteEffect{ConsumedInput{}} : KeyRouteEffect{ForwardCurrentKey{}}};
  }

  const auto before = active_frame().context;
  const bool key_captured = captured(event.key);
  if (forwarded(event.key)) {
    return {.effect = ForwardCurrentKey{}};
  }
  if (key_captured && captured_context(event.key) != before) {
    return {.effect = ConsumedInput{}};
  }

  const auto& metadata = context(before);
  const auto* const matched = binding(before, key_chord(event));
  if (matched == nullptr && metadata.unbound == UnboundBehavior::retry_base) {
    FRAME_ASSERT(metadata.lifetime == ContextLifetime::one_shot);
    pop();
    auto retried = route_key(event);
    retried.presentation_changed = retried.presentation_changed || visible_context_changed(before);
    return retried;
  }
  if (matched == nullptr && metadata.unbound == UnboundBehavior::forward) {
    if (key_captured) {
      return {.effect = ConsumedInput{}};
    }
    forward(event.key);
    if (metadata.lifetime == ContextLifetime::one_shot) {
      pop();
    }
    return {.effect = ForwardCurrentKey{}, .presentation_changed = visible_context_changed(before)};
  }

  std::array<std::byte, deferred_input_bytes_max> deferred{};
  const auto deferred_size = active_frame().deferred_size;
  std::ranges::copy(std::span(active_frame().deferred).first(deferred_size), deferred.begin());
  if (matched == nullptr) {
    if (metadata.unbound == UnboundBehavior::replay_deferred) {
      if (!key_captured) {
        forward(event.key);
      }
      if (metadata.lifetime == ContextLifetime::one_shot) {
        pop();
      }
      if (deferred_size == 0U) {
        return {.effect = ForwardCurrentKey{},
                .presentation_changed = visible_context_changed(before)};
      }
      return {.effect = ForwardBytesThenCurrentKey{.bytes = deferred, .size = deferred_size},
              .presentation_changed = visible_context_changed(before)};
    }
    if (!key_captured) {
      capture(event.key, before);
    }
    if (metadata.lifetime == ContextLifetime::one_shot) {
      pop();
    }
    return {.effect = ConsumedInput{}, .presentation_changed = visible_context_changed(before)};
  }

  if (!key_captured) {
    capture(event.key, before);
  }
  if (metadata.lifetime == ContextLifetime::one_shot) {
    pop();
  }
  bool preempt_interaction = false;
  KeyRouteEffect effect = ConsumedInput{};
  std::visit(
      // Exhaustive native policy dispatch; the hosted branch only emits a bounded index.
      // NOLINTNEXTLINE(readability-function-cognitive-complexity)
      [&](const auto& action) {
        using Action = std::decay_t<decltype(action)>;
        if constexpr (std::is_same_v<Action, CommandBinding>) {
          if (action.context == CommandContextDisposition::base) {
            return_to_base();
          }
          effect = RoutedCommand{.command = action.command};
        } else if constexpr (std::is_same_v<Action, PushContextBinding>) {
          const auto pushed =
              push(action.context, std::span(action.deferred).first(action.deferred_size));
          FRAME_ASSERT(pushed);
          preempt_interaction = context(action.context).preempts_interaction;
        } else if constexpr (std::is_same_v<Action, PopContextBinding>) {
          pop();
        } else if constexpr (std::is_same_v<Action, ForwardDeferredBinding>) {
          if (deferred_size > 0U) {
            effect = ForwardBytes{.bytes = deferred, .size = deferred_size};
          }
        } else if constexpr (std::is_same_v<Action, EncodeAsBinding>) {
          if (encoded_hold_from_ == PhysicalKey::unidentified) {
            encoded_hold_from_ = event.key;
            encoded_hold_to_ = action.key;
            encoded_hold_modifiers_ = action.modifiers;
          }
          effect = EncodeAsKey{.key = action.key, .modifiers = action.modifiers};
        } else if constexpr (std::is_same_v<Action, HostedCommandBinding>) {
          if (action.context == CommandContextDisposition::base) {
            return_to_base();
          }
          effect = RoutedHostedCommand{.index = action.index};
        }
      },
      matched->action);
  return {.effect = effect,
          .presentation_changed = visible_context_changed(before),
          .interaction_preemption_requested = preempt_interaction};
}

InputMapConfiguration::InputMapConfiguration() noexcept { reset(InputMapPreset::defaults); }

// The context enum and label bounds are validated before fixed-storage access.
// NOLINTNEXTLINE(bugprone-exception-escape)
auto InputMapConfiguration::set_context(const ConfiguredInputContext selected,
                                        const ContextOptions options) noexcept -> bool {
  const auto index = static_cast<std::size_t>(selected);
  if (selected >= ConfiguredInputContext::count || index >= contexts.size() ||
      options.label.size() > input_context_label_bytes_max ||
      (options.unbound == UnboundBehavior::retry_base &&
       options.lifetime != ContextLifetime::one_shot)) {
    return false;
  }
  auto& configured_context = contexts.at(index);
  configured_context = {.label_size = static_cast<std::uint8_t>(options.label.size()),
                        .lifetime = options.lifetime,
                        .unbound = options.unbound,
                        .preempts_interaction = options.preempts_interaction};
  std::ranges::copy(options.label, configured_context.label.begin());
  return true;
}

auto InputMapConfiguration::set_action(const ConfiguredInputContext context, const InputChord chord,
                                       const ConfiguredBindingAction action) noexcept -> bool {
  for (std::size_t index = 0; index < binding_count; ++index) {
    auto& configured = std::span(bindings).subspan(index, 1).front();
    if (configured.context == context && configured.chord == chord) {
      configured.action = action;
      return true;
    }
  }
  if (binding_count >= bindings.size()) {
    return false;
  }
  std::span(bindings).subspan(binding_count++, 1).front() = {
      .context = context, .chord = chord, .action = action};
  return true;
}

auto InputMapConfiguration::set(const ConfiguredInputContext context, const InputChord chord,
                                const InputCommand command,
                                const CommandContextDisposition disposition) noexcept -> bool {
  return set_action(
      context, chord,
      {.kind = ConfiguredBindingKind::command, .command = command, .disposition = disposition});
}

auto InputMapConfiguration::push(const ConfiguredInputContext context, const InputChord chord,
                                 const ConfiguredInputContext target,
                                 const bool defer_chord) noexcept -> bool {
  return set_action(
      context, chord,
      {.kind = ConfiguredBindingKind::push_context, .target = target, .defer_chord = defer_chord});
}

auto InputMapConfiguration::pop(const ConfiguredInputContext context,
                                const InputChord chord) noexcept -> bool {
  return set_action(context, chord, {.kind = ConfiguredBindingKind::pop_context});
}

auto InputMapConfiguration::replay(const ConfiguredInputContext context,
                                   const InputChord chord) noexcept -> bool {
  return set_action(context, chord, {.kind = ConfiguredBindingKind::replay_deferred});
}

auto InputMapConfiguration::send(const ConfiguredInputContext context, const InputChord chord,
                                 const PhysicalKey key, const std::uint16_t modifiers) noexcept
    -> bool {
  return set_action(context, chord,
                    {.kind = ConfiguredBindingKind::send_key,
                     .encoded_key = key,
                     .encoded_modifiers = modifiers});
}

// Binding count is maintained within the fixed binding array.
// NOLINTNEXTLINE(bugprone-exception-escape)
auto InputMapConfiguration::unbind(const ConfiguredInputContext context,
                                   const InputChord chord) noexcept -> bool {
  for (std::size_t index = 0; index < binding_count; ++index) {
    if (bindings.at(index).context != context || bindings.at(index).chord != chord) {
      continue;
    }
    for (std::size_t moving = index + 1U; moving < binding_count; ++moving) {
      bindings.at(moving - 1U) = bindings.at(moving);
    }
    --binding_count;
    bindings.at(binding_count) = {};
    return true;
  }
  return true;
}

auto InputMapConfiguration::set_prefix(const std::optional<InputChord> chord) noexcept -> bool {
  const auto remove = [this](const InputChord value) noexcept {
    static_cast<void>(unbind(ConfiguredInputContext::normal, value));
    static_cast<void>(unbind(ConfiguredInputContext::resize, value));
    static_cast<void>(unbind(ConfiguredInputContext::prefix, value));
    if (value.kind == ChordKind::byte && (value.modifiers & key_modifier_control) != 0U) {
      const auto shifted =
          InputChord{.code = value.code,
                     .modifiers = static_cast<std::uint16_t>(value.modifiers | key_modifier_shift),
                     .kind = value.kind};
      static_cast<void>(unbind(ConfiguredInputContext::normal, shifted));
      static_cast<void>(unbind(ConfiguredInputContext::resize, shifted));
      static_cast<void>(unbind(ConfiguredInputContext::prefix, shifted));
    }
  };
  if (prefix.has_value()) {
    remove(*prefix);
  }
  prefix = chord;
  if (!prefix.has_value()) {
    return true;
  }
  const auto add = [this](const InputChord value) noexcept {
    return push(ConfiguredInputContext::normal, value, ConfiguredInputContext::prefix, true) &&
           push(ConfiguredInputContext::resize, value, ConfiguredInputContext::prefix, true) &&
           replay(ConfiguredInputContext::prefix, value);
  };
  if (!add(*prefix)) {
    return false;
  }
  if (prefix->kind == ChordKind::byte && (prefix->modifiers & key_modifier_control) != 0U) {
    const auto shifted =
        InputChord{.code = prefix->code,
                   .modifiers = static_cast<std::uint16_t>(prefix->modifiers | key_modifier_shift),
                   .kind = prefix->kind};
    return add(shifted);
  }
  return true;
}

// The shipped policy is ordinary bounded configuration data. The compiler below has no preset or
// built-in binding branch, so an equivalent user draft produces the same native map.
// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
void InputMapConfiguration::reset(const InputMapPreset selected) noexcept {
  bindings = {};
  contexts = {};
  prefix.reset();
  binding_count = 0;
  preset = selected;
#include "input/default_keymap.inc"
}

// NOLINTNEXTLINE(bugprone-exception-escape,readability-function-cognitive-complexity)
auto compile_input_map(const InputMapConfiguration& configuration) noexcept
    -> std::expected<CompiledInputMap, InputMapError> {
  if (configuration.binding_count > configuration.bindings.size() ||
      (configuration.preset != InputMapPreset::defaults &&
       configuration.preset != InputMapPreset::none)) {
    return std::unexpected(InputMapError::invalid_options);
  }
  InputMapDraft draft;
  std::array<InputContextId, static_cast<std::size_t>(ConfiguredInputContext::count)> contexts{};
  for (std::size_t index = 0; index < contexts.size(); ++index) {
    const auto& configured_context = configuration.contexts.at(index);
    const auto added = draft.add_context(
        {.label = std::string_view(configured_context.label.data(), configured_context.label_size),
         .lifetime = configured_context.lifetime,
         .unbound = configured_context.unbound,
         .preempts_interaction = configured_context.preempts_interaction});
    if (!added.has_value()) {
      return std::unexpected(added.error());
    }
    contexts.at(index) = *added;
  }
  for (const auto& configured :
       std::span(configuration.bindings).first(configuration.binding_count)) {
    if (configured.context >= ConfiguredInputContext::count ||
        configured.action.kind > ConfiguredBindingKind::hosted_command) {
      return std::unexpected(InputMapError::invalid_options);
    }
    BindingAction action;
    switch (configured.action.kind) {
    case ConfiguredBindingKind::command:
      if (configured.action.command >= InputCommand::count ||
          (configured.action.disposition != CommandContextDisposition::retain &&
           configured.action.disposition != CommandContextDisposition::base)) {
        return std::unexpected(InputMapError::invalid_action);
      }
      action = invoke(configured.action.command, configured.action.disposition);
      break;
    case ConfiguredBindingKind::hosted_command:
      if (configured.action.hosted_command >= limits::hosted_commands_hard_max ||
          !command_context_valid(configured.action.disposition)) {
        return std::unexpected(InputMapError::invalid_action);
      }
      action = HostedCommandBinding{.index = configured.action.hosted_command,
                                    .context = configured.action.disposition};
      break;
    case ConfiguredBindingKind::push_context: {
      if (configured.action.target >= ConfiguredInputContext::count) {
        return std::unexpected(InputMapError::invalid_context);
      }
      std::span<const std::byte> deferred;
      std::optional<EncodedPrefix> encoded;
      if (configured.action.defer_chord) {
        encoded = encode_prefix(configured.chord);
        if (encoded.has_value()) {
          deferred = std::span(encoded->bytes).first(encoded->size);
        }
      }
      auto pushed =
          push_context(contexts.at(static_cast<std::size_t>(configured.action.target)), deferred);
      if (!pushed.has_value()) {
        return std::unexpected(pushed.error());
      }
      action = *pushed;
      break;
    }
    case ConfiguredBindingKind::pop_context:
      action = pop_context();
      break;
    case ConfiguredBindingKind::replay_deferred:
      action = forward_deferred();
      break;
    case ConfiguredBindingKind::send_key:
      if (configured.action.encoded_key >= PhysicalKey::count ||
          (configured.action.encoded_modifiers & ~key_modifiers_all) != 0U) {
        return std::unexpected(InputMapError::invalid_action);
      }
      action = encode_as(configured.action.encoded_key, configured.action.encoded_modifiers);
      break;
    }
    if (!draft.set(contexts.at(static_cast<std::size_t>(configured.context)), configured.chord,
                   action)) {
      return std::unexpected(InputMapError::capacity);
    }
  }
  return draft.compile();
}

auto default_input_map() noexcept -> const CompiledInputMap& {
  static const CompiledInputMap map = [] {
    auto compiled = compile_input_map(InputMapConfiguration{});
    FRAME_ASSERT(compiled.has_value());
    return std::move(*compiled);
  }();
  return map;
}

static_assert(std::is_trivially_copyable_v<InputRouter>);
static_assert(sizeof(InputRouter) <= 80U);
static_assert(static_cast<std::uint8_t>(PhysicalKey::count) <= 64U);
static_assert(input_contexts_max <= 16U);
static_assert(input_contexts_max <= 0xFFU);
static_assert(input_bindings_max <= 0xFFU);
static_assert(input_context_stack_max <= 0xFFU);
static_assert(input_context_label_bytes_max <= 0xFFU);
static_assert(deferred_input_bytes_max <= 0xFFU);

} // namespace frame::input
