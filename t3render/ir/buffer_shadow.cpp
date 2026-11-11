#include "buffer_shadow.hpp"

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace t3render::ir {

BufferShadow::BufferShadow(std::size_t size, std::size_t budget) noexcept : size_(size), budget_(budget) {
    // Ensure both pointer arithmetic and the sum of storage sizes are safe.
    if (size > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
        status_ = ShadowStatus::TooLarge;
        return;
    }
    validity_bytes_ = size / 8 + (size % 8 != 0 ? 1 : 0);
    if (validity_bytes_ > budget || size > budget - validity_bytes_) {
        status_ = ShadowStatus::BudgetExceeded;
        return;
    }
    if (!size) {
        status_ = ShadowStatus::Ready;
        return;
    }
    try {
        auto data = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[size]);
        if (!data) return;
        auto validity = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[validity_bytes_]{});
        if (!validity) return;
        data_ = std::move(data);
        validity_ = std::move(validity);
        storage_bytes_ = size + validity_bytes_;
        status_ = ShadowStatus::Ready;
    } catch (...) {
        // Allocation failure, including an implementation's array-size limit,
        // must never escape into a future capture caller.
    }
}

bool BufferShadow::InBounds(std::size_t offset, std::size_t length) const noexcept {
    return offset <= size_ && length <= size_ - offset;
}

bool BufferShadow::Known(std::size_t offset, std::size_t length) const noexcept {
    if (!length) return true;
    const std::size_t first = offset / 8;
    const std::size_t last_bit = offset + length - 1;
    const std::size_t last = last_bit / 8;
    const auto first_mask = static_cast<std::uint8_t>(0xffu << (offset % 8));
    const auto last_mask = static_cast<std::uint8_t>(0xffu >> (7 - (last_bit % 8)));
    if (first == last) {
        const auto mask = static_cast<std::uint8_t>(first_mask & last_mask);
        return (validity_[first] & mask) == mask;
    }
    if ((validity_[first] & first_mask) != first_mask || (validity_[last] & last_mask) != last_mask) return false;
    for (std::size_t i = first + 1; i < last; ++i) if (validity_[i] != 0xffu) return false;
    return true;
}

void BufferShadow::SetKnown(std::size_t offset, std::size_t length, bool known) noexcept {
    if (!length) return;
    const std::size_t first = offset / 8;
    const std::size_t last_bit = offset + length - 1;
    const std::size_t last = last_bit / 8;
    const auto first_mask = static_cast<std::uint8_t>(0xffu << (offset % 8));
    const auto last_mask = static_cast<std::uint8_t>(0xffu >> (7 - (last_bit % 8)));
    auto update = [&](std::size_t index, std::uint8_t mask) {
        if (known) validity_[index] |= mask;
        else validity_[index] &= static_cast<std::uint8_t>(~mask);
    };
    if (first == last) {
        update(first, static_cast<std::uint8_t>(first_mask & last_mask));
        return;
    }
    update(first, first_mask);
    if (last - first > 1) std::memset(validity_.get() + first + 1, known ? 0xff : 0, last - first - 1);
    update(last, last_mask);
}

ShadowResult BufferShadow::Write(std::size_t offset, const void* source, std::size_t length) noexcept {
    if (status_ != ShadowStatus::Ready) return ShadowResult::Unavailable;
    if (!InBounds(offset, length)) return ShadowResult::OutOfBounds;
    if (!length) return ShadowResult::Ok;
    if (!source) return ShadowResult::NullPointer;
    std::memmove(data_.get() + offset, source, length);
    SetKnown(offset, length, true);
    return ShadowResult::Ok;
}

ShadowResult BufferShadow::Read(std::size_t offset, void* destination, std::size_t length) const noexcept {
    if (status_ != ShadowStatus::Ready) return ShadowResult::Unavailable;
    if (!InBounds(offset, length)) return ShadowResult::OutOfBounds;
    if (!length) return ShadowResult::Ok;
    if (!destination) return ShadowResult::NullPointer;
    if (!Known(offset, length)) return ShadowResult::UnknownData;
    std::memmove(destination, data_.get() + offset, length);
    return ShadowResult::Ok;
}

ShadowResult BufferShadow::Invalidate(std::size_t offset, std::size_t length) noexcept {
    if (status_ != ShadowStatus::Ready) return ShadowResult::Unavailable;
    if (!InBounds(offset, length)) return ShadowResult::OutOfBounds;
    SetKnown(offset, length, false);
    return ShadowResult::Ok;
}

ShadowResult BufferShadow::Discard() noexcept {
    if (status_ != ShadowStatus::Ready) return ShadowResult::Unavailable;
    if (validity_bytes_) std::memset(validity_.get(), 0, validity_bytes_);
    return ShadowResult::Ok;
}

ShadowResult BufferShadow::IsKnown(std::size_t offset, std::size_t length, bool& known) const noexcept {
    if (status_ != ShadowStatus::Ready) return ShadowResult::Unavailable;
    if (!InBounds(offset, length)) return ShadowResult::OutOfBounds;
    known = Known(offset, length);
    return ShadowResult::Ok;
}

}  // namespace t3render::ir
