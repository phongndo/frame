#ifndef FRAME_GENERATIONAL_STORE_HPP
#define FRAME_GENERATIONAL_STORE_HPP

#include "frame/assert.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace frame {

// A fixed-capacity owning slot map. IDs remain stable while a value is alive and stale IDs are
// rejected after erase/reuse. The store itself performs no allocation; callers choose how values
// are allocated before insertion.
template <typename Value, typename Id, std::size_t Capacity> class BoundedGenerationalStore final {
  static_assert(Capacity > 0);
  static_assert(Capacity <= std::numeric_limits<std::uint32_t>::max());

  struct Slot final {
    std::unique_ptr<Value> value;
    std::uint32_t generation{0};
  };

  template <bool Constant> class ValueIterator final {
    using SlotIterator =
        std::conditional_t<Constant, typename std::array<Slot, Capacity>::const_iterator,
                           typename std::array<Slot, Capacity>::iterator>;

  public:
    using difference_type = std::ptrdiff_t;
    using value_type = std::unique_ptr<Value>;

    constexpr explicit ValueIterator(const SlotIterator iterator) noexcept : iterator_(iterator) {}

    [[nodiscard]] constexpr decltype(auto) operator*() const noexcept { return (iterator_->value); }

    constexpr auto operator++() noexcept -> ValueIterator& {
      // Standard contiguous-iterator advancement remains inside this bounded store adapter.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      ++iterator_;
      return *this;
    }

    friend constexpr bool operator==(const ValueIterator&, const ValueIterator&) noexcept = default;

  private:
    SlotIterator iterator_;
  };

public:
  using iterator = ValueIterator<false>;
  using const_iterator = ValueIterator<true>;

  BoundedGenerationalStore() noexcept = default;
  BoundedGenerationalStore(const BoundedGenerationalStore&) = delete;
  auto operator=(const BoundedGenerationalStore&) -> BoundedGenerationalStore& = delete;
  BoundedGenerationalStore(BoundedGenerationalStore&&) = delete;
  auto operator=(BoundedGenerationalStore&&) -> BoundedGenerationalStore& = delete;
  ~BoundedGenerationalStore() = default;

  [[nodiscard]] auto insert(std::unique_ptr<Value> value) noexcept -> std::optional<Id> {
    if (value == nullptr) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      // The loop proves index is within this fixed array.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      auto& slot = slots_[index];
      // An exhausted empty slot is permanently retired so no stale generation can become valid.
      if (slot.value != nullptr || slot.generation == std::numeric_limits<std::uint32_t>::max()) {
        continue;
      }
      ++slot.generation;
      slot.value = std::move(value);
      ++size_;
      return Id::from_parts(static_cast<std::uint32_t>(index), slot.generation);
    }
    return std::nullopt;
  }

  [[nodiscard]] auto get(const Id id) noexcept -> Value* {
    if (!contains(id)) {
      return nullptr;
    }
    // contains proved the ID slot is in range.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return slots_[id.slot()].value.get();
  }

  [[nodiscard]] auto get(const Id id) const noexcept -> const Value* {
    if (!contains(id)) {
      return nullptr;
    }
    // contains proved the ID slot is in range.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return slots_[id.slot()].value.get();
  }

  [[nodiscard]] auto contains(const Id id) const noexcept -> bool {
    if (!id.is_valid() || id.slot() >= slots_.size()) {
      return false;
    }
    // The preceding range check proves the slot is in range.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto& slot = slots_[id.slot()];
    return slot.value != nullptr && slot.generation == id.generation();
  }

  [[nodiscard]] auto erase(const Id id) noexcept -> bool {
    if (!contains(id)) {
      return false;
    }
    // contains proved the ID slot is in range.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    slots_[id.slot()].value.reset();
    FRAME_ASSERT(size_ > 0);
    --size_;
    return true;
  }

  [[nodiscard]] constexpr auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] static constexpr auto capacity() noexcept -> std::size_t { return Capacity; }
  [[nodiscard]] constexpr auto empty() const noexcept -> bool { return size_ == 0; }

  [[nodiscard]] constexpr auto begin() noexcept -> iterator { return iterator(slots_.begin()); }
  [[nodiscard]] constexpr auto end() noexcept -> iterator { return iterator(slots_.end()); }
  [[nodiscard]] constexpr auto begin() const noexcept -> const_iterator {
    return const_iterator(slots_.begin());
  }
  [[nodiscard]] constexpr auto end() const noexcept -> const_iterator {
    return const_iterator(slots_.end());
  }

private:
  std::array<Slot, Capacity> slots_{};
  std::size_t size_{0};
};

} // namespace frame

#endif // FRAME_GENERATIONAL_STORE_HPP
