#include "buffer_registry.hpp"

#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace t3render::ir {

BufferRegistry::BufferRegistry(std::size_t byte_budget, std::size_t max_resources) noexcept
    : byte_budget_(byte_budget), max_resources_(max_resources) {}

RegistryResult BufferRegistry::Find(BufferToken token, const Entry*& entry) const noexcept {
    const auto it = entries_.find(token.identity);
    if (it == entries_.end()) return RegistryResult::UnknownResource;
    if (!token.generation || it->second.generation != token.generation) return RegistryResult::StaleGeneration;
    entry = &it->second;
    return RegistryResult::Ok;
}

RegistryResult BufferRegistry::Find(BufferToken token, Entry*& entry) noexcept {
    const Entry* found = nullptr;
    const auto result = static_cast<const BufferRegistry*>(this)->Find(token, found);
    if (result == RegistryResult::Ok) entry = const_cast<Entry*>(found);
    return result;
}

RegistryResult BufferRegistry::Register(std::uintptr_t identity, std::size_t size, BufferToken& token,
                                        BufferAvailability& availability) noexcept {
    if (!identity) return RegistryResult::InvalidIdentity;
    const auto previous = entries_.find(identity);
    if (previous != entries_.end()) {
        AbandonEntry(previous->second);
        return RegistryResult::AlreadyTracked;
    }
    if (entries_.size() >= max_resources_) return RegistryResult::CapacityExceeded;
    if (!next_generation_) return RegistryResult::GenerationExhausted;
    Entry entry;
    entry.generation = next_generation_;
    entry.size = size;
    entry.shadow.reset(new (std::nothrow) BufferShadow(size, byte_budget_ - used_bytes_));
    if (entry.shadow) {
        switch (entry.shadow->status()) {
        case ShadowStatus::Ready: entry.availability = BufferAvailability::Tracked; break;
        case ShadowStatus::TooLarge: entry.availability = BufferAvailability::TooLarge; break;
        case ShadowStatus::BudgetExceeded: entry.availability = BufferAvailability::BudgetExceeded; break;
        case ShadowStatus::AllocationFailed: entry.availability = BufferAvailability::AllocationFailed; break;
        }
        if (entry.availability != BufferAvailability::Tracked) entry.shadow.reset();
    }
    const std::size_t storage = entry.shadow ? entry.shadow->storage_bytes() : 0;
    const auto status = entry.availability;
    try {
        const auto inserted = entries_.emplace(identity, std::move(entry));
        if (!inserted.second) return RegistryResult::AlreadyTracked;
    } catch (...) {
        return RegistryResult::AllocationFailed;
    }
    used_bytes_ += storage;
    token = {identity, next_generation_};
    availability = status;
    next_generation_ = next_generation_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_generation_ + 1;
    return RegistryResult::Ok;
}

void BufferRegistry::ClearStaging(Entry& entry) noexcept {
    used_bytes_ -= entry.staging_size;
    entry.staging.reset();
    entry.staging_size = 0;
}

void BufferRegistry::AbandonEntry(Entry& entry) noexcept {
    ClearStaging(entry);
    if (entry.shadow) {
        used_bytes_ -= entry.shadow->storage_bytes();
        entry.shadow.reset();
    }
    entry.availability = BufferAvailability::Abandoned;
    entry.locked = false;
}

RegistryResult BufferRegistry::Release(BufferToken token) noexcept {
    Entry* entry = nullptr;
    const auto result = Find(token, entry);
    if (result != RegistryResult::Ok) return result;
    ClearStaging(*entry);
    if (entry->shadow) used_bytes_ -= entry->shadow->storage_bytes();
    entries_.erase(token.identity);
    return RegistryResult::Ok;
}

RegistryResult BufferRegistry::Availability(BufferToken token, BufferAvailability& availability) const noexcept {
    const Entry* entry = nullptr;
    const auto result = Find(token, entry);
    if (result == RegistryResult::Ok) availability = entry->availability;
    return result;
}

RegistryResult BufferRegistry::BeginLock(BufferToken token, std::size_t offset, std::size_t length,
                                         BufferLock kind) noexcept {
    Entry* entry = nullptr;
    const auto result = Find(token, entry);
    if (result != RegistryResult::Ok) return result;
    if (entry->availability == BufferAvailability::Abandoned) return RegistryResult::Unavailable;
    if (entry->locked) {
        AbandonEntry(*entry);
        return RegistryResult::OverlappingLocks;
    }
    if (kind != BufferLock::ReadOnly && kind != BufferLock::Write && kind != BufferLock::Discard) {
        AbandonEntry(*entry);
        return RegistryResult::InvalidLock;
    }
    if (!length || offset > entry->size || length > entry->size - offset) {
        AbandonEntry(*entry);
        return RegistryResult::InvalidRange;
    }
    if (entry->shadow) {
        if (kind == BufferLock::Write) entry->shadow->Invalidate(offset, length);
        if (kind == BufferLock::Discard) entry->shadow->Discard();
    }
    entry->locked = true;
    entry->offset = offset;
    entry->length = length;
    entry->kind = kind;
    return RegistryResult::Ok;
}

RegistryResult BufferRegistry::StageUnlock(BufferToken token, const void* data, std::size_t length) noexcept {
    Entry* entry = nullptr;
    const auto result = Find(token, entry);
    if (result != RegistryResult::Ok) return result;
    if (!entry->locked) return RegistryResult::NoActiveLock;
    if (entry->kind == BufferLock::ReadOnly) return RegistryResult::InvalidOperation;
    ClearStaging(*entry);
    if (length != entry->length) return RegistryResult::InvalidRange;
    if (!data) return RegistryResult::NullPointer;
    if (!entry->shadow) return RegistryResult::Unavailable;
    if (length > byte_budget_ - used_bytes_) return RegistryResult::BudgetExceeded;
    try {
        auto copy = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[length]);
        if (!copy) return RegistryResult::AllocationFailed;
        std::memcpy(copy.get(), data, length);
        entry->staging = std::move(copy);
        entry->staging_size = length;
        used_bytes_ += length;
        return RegistryResult::Ok;
    } catch (...) {
        return RegistryResult::AllocationFailed;
    }
}

RegistryResult BufferRegistry::EndUnlock(BufferToken token, bool succeeded) noexcept {
    Entry* entry = nullptr;
    const auto result = Find(token, entry);
    if (result != RegistryResult::Ok) return result;
    if (!entry->locked) return RegistryResult::NoActiveLock;
    if (!succeeded) {
        AbandonEntry(*entry);
        return RegistryResult::Ok;
    }
    RegistryResult outcome = RegistryResult::Ok;
    if (entry->kind != BufferLock::ReadOnly) {
        if (!entry->shadow) outcome = RegistryResult::Unavailable;
        else if (!entry->staging) outcome = RegistryResult::DataNotStaged;
        else if (entry->shadow->Write(entry->offset, entry->staging.get(), entry->length) != ShadowResult::Ok) {
            AbandonEntry(*entry);
            return RegistryResult::Unavailable;
        }
    }
    ClearStaging(*entry);
    entry->locked = false;
    return outcome;
}

RegistryResult BufferRegistry::Abandon(BufferToken token) noexcept {
    Entry* entry = nullptr;
    const auto result = Find(token, entry);
    if (result == RegistryResult::Ok) AbandonEntry(*entry);
    return result;
}

RegistryResult BufferRegistry::Read(BufferToken token, std::size_t offset, void* destination,
                                    std::size_t length) const noexcept {
    const Entry* entry = nullptr;
    const auto result = Find(token, entry);
    if (result != RegistryResult::Ok) return result;
    if (offset > entry->size || length > entry->size - offset) return RegistryResult::InvalidRange;
    if (length && !destination) return RegistryResult::NullPointer;
    if (entry->locked) return RegistryResult::Busy;
    if (!entry->shadow) return RegistryResult::Unavailable;
    switch (entry->shadow->Read(offset, destination, length)) {
    case ShadowResult::Ok: return RegistryResult::Ok;
    case ShadowResult::UnknownData: return RegistryResult::UnknownData;
    case ShadowResult::OutOfBounds: return RegistryResult::InvalidRange;
    case ShadowResult::NullPointer: return RegistryResult::NullPointer;
    case ShadowResult::Unavailable: return RegistryResult::Unavailable;
    }
    return RegistryResult::Unavailable;
}

}  // namespace t3render::ir
