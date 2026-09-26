#include "core/session.hpp"

#include "lemma/assert.hpp"
#include "lemma/id.hpp"
#include "lemma/limits.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace lemma::core {
namespace {

[[nodiscard]] constexpr auto valid_session_name(const std::string_view name) noexcept -> bool {
  return !name.empty() && name.front() != '-' && name.size() <= limits::session_name_bytes_max &&
         std::ranges::all_of(name, [](const char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '_' || character == '-';
         });
}

[[nodiscard]] constexpr auto valid_tab_title(const std::string_view title) noexcept -> bool {
  return title.size() <= limits::tab_title_bytes_max &&
         std::ranges::all_of(title, [](const char character) {
           const auto byte = static_cast<unsigned char>(character);
           return byte >= 0x20U && byte <= 0x7eU;
         });
}

} // namespace

auto TabOrder::position_of(const TabId tab) const noexcept -> std::optional<std::size_t> {
  if (!tab.is_valid()) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < size_; ++index) {
    if (std::span(ids_).subspan(index, 1).front() == tab) {
      return index;
    }
  }
  return std::nullopt;
}

auto TabOrder::at(const std::size_t position) const noexcept -> std::optional<TabId> {
  if (position >= size_) {
    return std::nullopt;
  }
  return std::span(ids_).subspan(position, 1).front();
}

auto TabOrder::append(const TabId tab) noexcept -> bool {
  if (!tab.is_valid() || size_ >= ids_.size() || position_of(tab).has_value()) {
    return false;
  }
  std::span(ids_).subspan(size_, 1).front() = tab;
  ++size_;
  return true;
}

auto TabOrder::erase(const TabId tab) noexcept -> bool {
  const auto position = position_of(tab);
  if (!position.has_value()) {
    return false;
  }
  for (std::size_t index = *position; index + 1U < size_; ++index) {
    std::span(ids_).subspan(index, 1).front() = std::span(ids_).subspan(index + 1U, 1).front();
  }
  --size_;
  std::span(ids_).subspan(size_, 1).front() = {};
  return true;
}

auto TabOrder::place_before(const TabId moving, const std::optional<TabId> anchor) noexcept
    -> bool {
  const auto source = position_of(moving);
  if (!source.has_value() || (anchor.has_value() && *anchor == moving)) {
    return false;
  }
  auto destination = size_;
  if (anchor.has_value()) {
    const auto anchor_position = position_of(*anchor);
    if (!anchor_position.has_value()) {
      return false;
    }
    destination = *anchor_position;
  }
  if (*source < destination) {
    --destination;
  }
  if (*source == destination) {
    return false;
  }
  const auto retained = std::span(ids_).subspan(*source, 1).front();
  if (*source < destination) {
    for (std::size_t index = *source; index < destination; ++index) {
      std::span(ids_).subspan(index, 1).front() = std::span(ids_).subspan(index + 1U, 1).front();
    }
  } else {
    for (std::size_t index = *source; index > destination; --index) {
      std::span(ids_).subspan(index, 1).front() = std::span(ids_).subspan(index - 1U, 1).front();
    }
  }
  std::span(ids_).subspan(destination, 1).front() = retained;
  return true;
}

Tab::Tab(const TabId assigned_id, const PaneId first_pane) noexcept
    : id(assigned_id), layout(first_pane), previous_pane(first_pane), focus(first_pane) {
  LEMMA_ASSERT(id.is_valid() && first_pane.is_valid());
}

auto Tab::float_presentable(const PaneId pane) const noexcept -> bool {
  if (layout_suspended || !floats_visible()) {
    return false;
  }
  const auto placement = floats.placement(pane);
  return placement.has_value() && placement
                                      ->resolve({.column = layout_column,
                                                 .row = layout_row,
                                                 .columns = layout_columns,
                                                 .rows = layout_rows})
                                      .has_value();
}

void Tab::set_tiled_focus(const PaneId pane) noexcept {
  LEMMA_ASSERT(layout.contains(pane));
  focus.tiled_pane_ = pane;
}

void Tab::focus_tiled(const PaneId pane) noexcept {
  set_tiled_focus(pane);
  focus.floating_ = false;
}

auto Tab::focus_float(const PaneId pane) noexcept -> bool {
  if (!float_presentable(pane) || !floats.raise(pane)) {
    return false;
  }
  focus.floating_ = true;
  return true;
}

auto TabTitleOverride::view() const noexcept -> std::string_view { return {bytes_.data(), size_}; }

auto TabTitleOverride::set(const std::string_view title) noexcept -> bool {
  if (!valid_tab_title(title)) {
    return false;
  }
  bytes_ = {};
  if (!title.empty()) {
    std::memcpy(bytes_.data(), title.data(), title.size());
  }
  size_ = title.size();
  return true;
}

auto Tab::title_override() const noexcept -> std::string_view { return title.view(); }

auto Tab::set_title_override(const std::string_view title_value) noexcept -> bool {
  return title.set(title_value);
}

Session::Session(const std::string_view session_name,
                 const std::string_view initial_working_directory,
                 const std::span<const std::byte> initial_environment,
                 const LaunchEnvironmentMode initial_environment_mode) noexcept
    : working_directory_size(initial_working_directory.size()),
      environment_size(initial_environment.size()), environment_mode(initial_environment_mode) {
  LEMMA_ASSERT(initial_working_directory.size() <= limits::working_directory_bytes_max);
  LEMMA_ASSERT(initial_environment.size() <= environment.size());
  const bool named = rename(session_name);
  LEMMA_ASSERT(named);
  if (!initial_working_directory.empty()) {
    std::memcpy(working_directory.data(), initial_working_directory.data(),
                initial_working_directory.size());
  }
  std::ranges::copy(initial_environment, environment.begin());
}

auto Session::session_name() const noexcept -> std::string_view { return {name.data(), name_size}; }

auto Session::rename(const std::string_view session_name) noexcept -> bool {
  if (!valid_session_name(session_name)) {
    return false;
  }
  name = {};
  std::memcpy(name.data(), session_name.data(), session_name.size());
  name_size = session_name.size();
  return true;
}

auto Session::cwd() const noexcept -> std::string_view {
  return {working_directory.data(), working_directory_size};
}

auto Session::launch_environment() const noexcept -> std::span<const std::byte> {
  return std::span(environment).first(environment_size);
}

} // namespace lemma::core
