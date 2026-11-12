#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace t3render::com {

// A preliminary, COM-independent identity table for a future facade. The
// caller supplies an address obtained from canonical IUnknown and keeps the
// underlying native object alive while acquiring. A stored proxy/control must
// keep its own native reference alive. This table does not call QueryInterface,
// implement COM AddRef/Release, or make independently obtained raw pointers
// safe. Its generation is local to this registry instance.
//
// Entries are weak: a Handle pins the object, but the registry does not extend
// its lifetime. A facade must still coordinate its external COM refcount and
// native lifetime, including a final Release racing a new getter. A pin alone
// is not a COM AddRef. Never use an address as a persistent asset identity.
enum class IdentityStatus {
    Inserted,
    Existing,
    NotFound,
    InvalidIdentity,
    InvalidCandidate,
    CapacityExceeded,
    GenerationExhausted,
    Failure,
};

template <typename T>
class IdentityRegistry final {
public:
    struct Handle {
        std::shared_ptr<T> object;
        std::uint64_t generation = 0;
        IdentityStatus status = IdentityStatus::NotFound;

        explicit operator bool() const noexcept { return static_cast<bool>(object); }
    };

    explicit IdentityRegistry(std::size_t maximum, std::uint64_t first_generation = 1) noexcept
        : maximum_(maximum), next_generation_(first_generation) {}
    IdentityRegistry(const IdentityRegistry&) = delete;
    IdentityRegistry& operator=(const IdentityRegistry&) = delete;

    // Candidate construction and any canonical-IUnknown query occur before
    // entering this function. A losing candidate is destroyed after unlocking.
    // All calls on the same identity must refer to the same live native COM
    // object. A candidate cannot be shared between distinct identities.
    Handle Acquire(std::uintptr_t identity, std::shared_ptr<T> candidate) noexcept {
        Handle result;
        if (!identity) { result.status = IdentityStatus::InvalidIdentity; return result; }
        if (!candidate) { result.status = IdentityStatus::InvalidCandidate; return result; }

        // Extracted nodes die outside the lock: destroying their last weak
        // control-block reference can itself invoke a user-supplied allocator.
        typename Entries::node_type expired;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(identity);
            if (it != entries_.end()) {
                auto pinned = it->second.object.lock();
                if (pinned) {
                    result.object = std::move(pinned);
                    result.generation = it->second.generation;
                    result.status = IdentityStatus::Existing;
                    return result;
                }
            }
            if (!next_generation_) { result.status = IdentityStatus::GenerationExhausted; return result; }

            if (it != entries_.end()) expired = entries_.extract(it);
            if (entries_.size() >= maximum_) {
                auto stale = entries_.end();
                for (auto current = entries_.begin(); current != entries_.end(); ++current) {
                    if (current->second.object.expired()) { stale = current; break; }
                }
                if (stale == entries_.end()) { result.status = IdentityStatus::CapacityExceeded; return result; }
                // Only one node can need extraction here. If the requested
                // identity was stale, its removal already made capacity.
                expired = entries_.extract(stale);
            }

            const std::uint64_t generation = next_generation_;
            entries_.emplace(identity, Entry{generation, candidate});
            next_generation_ = generation == std::numeric_limits<std::uint64_t>::max() ? 0 : generation + 1;
            result.object = std::move(candidate);
            result.generation = generation;
            result.status = IdentityStatus::Inserted;
        } catch (...) {
            result.status = IdentityStatus::Failure;
        }
        return result;
    }

    Handle Find(std::uintptr_t identity) noexcept {
        Handle result;
        if (!identity) { result.status = IdentityStatus::InvalidIdentity; return result; }
        typename Entries::node_type expired;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(identity);
            if (it == entries_.end()) return result;
            auto pinned = it->second.object.lock();
            if (!pinned) {
                expired = entries_.extract(it);
            } else {
                result.object = std::move(pinned);
                result.generation = it->second.generation;
                result.status = IdentityStatus::Existing;
            }
        } catch (...) {
            result.status = IdentityStatus::Failure;
        }
        return result;
    }

    // A delayed retirement may remove only the same expired generation. It
    // cannot erase a live wrapper or a later object reusing the same address.
    bool ForgetExpired(std::uintptr_t identity, std::uint64_t generation) noexcept {
        if (!identity || !generation) return false;
        typename Entries::node_type expired;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(identity);
            if (it != entries_.end() && it->second.generation == generation && it->second.object.expired())
                expired = entries_.extract(it);
        } catch (...) {
            return false;
        }
        return !expired.empty();
    }

    // Remove an exact generation even if internal handles still pin it. The
    // facade MUST first atomically transition that control's external COM
    // refcount to zero and make every future TryAddRef fail. Violating that
    // precondition permits two live COM identities for the same native object.
    // A getter racing final Release must TryAddRef before exposing a pointer;
    // on failure it may retire this generation and acquire a new control.
    // This table neither checks nor enforces those COM refcount obligations.
    bool Retire(std::uintptr_t identity, std::uint64_t generation) noexcept {
        if (!identity || !generation) return false;
        typename Entries::node_type retired;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = entries_.find(identity);
            if (it != entries_.end() && it->second.generation == generation)
                retired = entries_.extract(it);
        } catch (...) {
            return false;
        }
        return !retired.empty();
    }

private:
    struct Entry {
        std::uint64_t generation;
        std::weak_ptr<T> object;
    };
    using Entries = std::unordered_map<std::uintptr_t, Entry>;

    const std::size_t maximum_;
    std::uint64_t next_generation_;
    std::mutex mutex_;
    Entries entries_;
};

}  // namespace t3render::com
