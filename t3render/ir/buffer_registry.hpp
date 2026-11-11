#pragma once

#include "buffer_shadow.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace t3render::ir {

// Opaque process-local identity plus a registry-assigned generation. Neither is
// a persistent asset ID. A retired token cannot address a later object at the
// same address.
struct BufferToken {
    std::uintptr_t identity = 0;
    std::uint64_t generation = 0;
};

enum class BufferAvailability { Tracked, BudgetExceeded, TooLarge, AllocationFailed, Abandoned };
enum class BufferLock { ReadOnly, Write, Discard };
enum class RegistryResult {
    Ok, InvalidIdentity, AlreadyTracked, CapacityExceeded, GenerationExhausted,
    AllocationFailed, UnknownResource, StaleGeneration, InvalidRange, InvalidLock,
    OverlappingLocks, NoActiveLock, InvalidOperation, NullPointer, BudgetExceeded,
    Unavailable, UnknownData, Busy, DataNotStaged
};

// D3D-independent evidence tracker. All methods must be externally serialized.
// Register a resource at creation, before any writes; retire it only on its
// final observed release. Unknown aliases/writes must call Abandon. Tracking an
// object late, or assuming an unobserved release, can otherwise produce false
// known data. This class does not hook D3D or copy from untrusted memory.
//
// The global byte budget covers BufferShadow's byte/validity arrays plus
// in-flight staging arrays. It excludes registry/allocator metadata and
// caller-owned snapshots; max_resources bounds the number of metadata entries.
// Entries remain registered but unavailable when shadow storage is over budget.
class BufferRegistry final {
public:
    explicit BufferRegistry(std::size_t byte_budget, std::size_t max_resources) noexcept;
    BufferRegistry(const BufferRegistry&) = delete;
    BufferRegistry& operator=(const BufferRegistry&) = delete;

    // A duplicate creation identity abandons the existing generation: an
    // unobserved release/address reuse cannot be distinguished from an alias.
    RegistryResult Register(std::uintptr_t identity, std::size_t size, BufferToken& token,
                            BufferAvailability& availability) noexcept;
    RegistryResult Release(BufferToken token) noexcept;
    RegistryResult Availability(BufferToken token, BufferAvailability& availability) const noexcept;

    // The adapter supplies a normalized nonempty byte range. On a successful
    // writable lock call this immediately invalidates that range; Discard
    // invalidates the whole resource. ReadOnly does not invalidate. An overlap
    // or an invalid successful lock abandons tracking conservatively. Call only
    // for a lock the runtime accepted, and serialize capture with game calls.
    RegistryResult BeginLock(BufferToken token, std::size_t offset, std::size_t length,
                             BufferLock kind) noexcept;
    // Before calling the real Unlock, supply an exact copy from trusted,
    // fault-safe staging. This class does not dereference a D3D lock pointer.
    // Failed or missing staging leaves the range unknown.
    RegistryResult StageUnlock(BufferToken token, const void* data, std::size_t length) noexcept;
    // Commit staged bytes only when the real Unlock succeeded. Failed Unlock
    // abandons the object because its lock state is then unknown.
    RegistryResult EndUnlock(BufferToken token, bool succeeded) noexcept;
    // Use after unknown aliases, external writes or any event which makes
    // future tracking unreliable. Abandon is permanent for this generation.
    RegistryResult Abandon(BufferToken token) noexcept;
    RegistryResult Read(BufferToken token, std::size_t offset, void* destination,
                        std::size_t length) const noexcept;

    std::size_t byte_budget() const noexcept { return byte_budget_; }
    std::size_t used_bytes() const noexcept { return used_bytes_; }
    std::size_t resource_count() const noexcept { return entries_.size(); }

private:
    struct Entry {
        std::uint64_t generation = 0;
        std::size_t size = 0;
        BufferAvailability availability = BufferAvailability::AllocationFailed;
        std::unique_ptr<BufferShadow> shadow;
        bool locked = false;
        BufferLock kind = BufferLock::ReadOnly;
        std::size_t offset = 0;
        std::size_t length = 0;
        std::unique_ptr<std::uint8_t[]> staging;
        std::size_t staging_size = 0;
    };
    RegistryResult Find(BufferToken token, Entry*& entry) noexcept;
    RegistryResult Find(BufferToken token, const Entry*& entry) const noexcept;
    void ClearStaging(Entry& entry) noexcept;
    void AbandonEntry(Entry& entry) noexcept;

    const std::size_t byte_budget_;
    const std::size_t max_resources_;
    std::size_t used_bytes_ = 0;
    std::uint64_t next_generation_ = 1;
    std::unordered_map<std::uintptr_t, Entry> entries_;
};

}  // namespace t3render::ir
