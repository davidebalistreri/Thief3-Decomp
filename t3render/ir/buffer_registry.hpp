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
// First observed cause only; this is diagnostic and never restores bytes.
enum class BufferAbandonCause { Unknown, RepeatedUnlock };
enum class BufferLock { ReadOnly, Write, Discard };
enum class RegistryResult {
    Ok, InvalidIdentity, AlreadyTracked, CapacityExceeded, GenerationExhausted,
    AllocationFailed, UnknownResource, StaleGeneration, InvalidRange, InvalidLock,
    OverlappingLocks, NoActiveLock, InvalidOperation, NullPointer, BudgetExceeded,
    Unavailable, UnknownData, Busy, DataNotStaged, ReadFailed
};

// Return true only if every requested byte was copied into destination. The
// callback must not retain the destination or re-enter this registry.
using BufferSnapshotReader = bool (*)(void* context, void* destination, std::size_t length);

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

    // Supply a normalized nonempty byte range BEFORE invoking the real Lock.
    // A writable attempt immediately invalidates that range; Discard
    // invalidates the whole resource. ReadOnly does not invalidate. On a
    // failed real Lock call EndUnlock(token, false), which abandons tracking.
    // An overlap or invalid range also abandons tracking conservatively.
    RegistryResult BeginLock(BufferToken token, std::size_t offset, std::size_t length,
                             BufferLock kind) noexcept;
    // Before calling the real Unlock, supply an exact copy from trusted,
    // fault-safe staging. This class does not dereference a D3D lock pointer.
    // Failed or missing staging leaves the range unknown.
    RegistryResult StageUnlock(BufferToken token, const void* data, std::size_t length) noexcept;
    // As above, but stage directly through a fault-safe reader into registry-
    // owned memory. Its allocation counts toward the global budget; failed or
    // partial reads are discarded. This does not make a racing source stable.
    RegistryResult StageUnlockFromReader(BufferToken token, BufferSnapshotReader reader, void* context) noexcept;
    // Commit staged bytes only when the real Unlock succeeded. Failed Unlock
    // abandons the object because its lock state is then unknown.
    RegistryResult EndUnlock(BufferToken token, bool succeeded) noexcept;
    // Use after unknown aliases, external writes or any event which makes
    // future tracking unreliable. Abandon is permanent for this generation.
    RegistryResult Abandon(BufferToken token, BufferAbandonCause cause = BufferAbandonCause::Unknown) noexcept;
    RegistryResult AbandonCause(BufferToken token, BufferAbandonCause& cause) const noexcept;
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
        BufferAbandonCause abandon_cause = BufferAbandonCause::Unknown;
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
    void AbandonEntry(Entry& entry, BufferAbandonCause cause = BufferAbandonCause::Unknown) noexcept;

    const std::size_t byte_budget_;
    const std::size_t max_resources_;
    std::size_t used_bytes_ = 0;
    std::uint64_t next_generation_ = 1;
    std::unordered_map<std::uintptr_t, Entry> entries_;
};

}  // namespace t3render::ir
