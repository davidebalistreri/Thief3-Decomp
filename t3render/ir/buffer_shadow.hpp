#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace t3render::ir {

// The budget covers only the two owned byte arrays: N bytes of contents and
// ceil(N/8) bytes of validity bits. It is per instance; allocator metadata,
// the object itself, and caller-owned input/output buffers are not counted.
enum class ShadowStatus { Ready, TooLarge, BudgetExceeded, AllocationFailed };
enum class ShadowResult { Ok, Unavailable, OutOfBounds, NullPointer, UnknownData };

// CPU-side evidence about one logical buffer. Initially no byte is known.
// This class does not read GPU memory, track resource lifetime/generation IDs,
// or synchronize concurrent calls. The caller must provide valid CPU memory
// for each nonempty read or write and serialize access from multiple threads.
class BufferShadow final {
public:
    BufferShadow(std::size_t size, std::size_t budget) noexcept;
    BufferShadow(const BufferShadow&) = delete;
    BufferShadow& operator=(const BufferShadow&) = delete;
    BufferShadow(BufferShadow&&) = delete;
    BufferShadow& operator=(BufferShadow&&) = delete;

    ShadowStatus status() const noexcept { return status_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t budget() const noexcept { return budget_; }
    std::size_t storage_bytes() const noexcept { return storage_bytes_; }

    // Out-of-range/null/unknown failures do not change stored bytes, validity,
    // or the destination. A zero-length operation accepts null at any valid
    // offset (including exactly size()).
    ShadowResult Write(std::size_t offset, const void* source, std::size_t length) noexcept;
    ShadowResult Read(std::size_t offset, void* destination, std::size_t length) const noexcept;
    ShadowResult Invalidate(std::size_t offset, std::size_t length) noexcept;
    ShadowResult Discard() noexcept;
    ShadowResult IsKnown(std::size_t offset, std::size_t length, bool& known) const noexcept;

private:
    bool InBounds(std::size_t offset, std::size_t length) const noexcept;
    bool Known(std::size_t offset, std::size_t length) const noexcept;
    void SetKnown(std::size_t offset, std::size_t length, bool known) noexcept;

    const std::size_t size_;
    const std::size_t budget_;
    std::size_t validity_bytes_ = 0;
    std::size_t storage_bytes_ = 0;
    ShadowStatus status_ = ShadowStatus::AllocationFailed;
    std::unique_ptr<std::uint8_t[]> data_;
    std::unique_ptr<std::uint8_t[]> validity_;
};

}  // namespace t3render::ir
