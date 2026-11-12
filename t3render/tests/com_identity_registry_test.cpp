#include "../src/com_identity_registry.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using t3render::com::IdentityRegistry;
using t3render::com::IdentityStatus;

namespace {
struct Proxy {
    explicit Proxy(std::atomic<unsigned>& destroyed, std::function<void()> on_destroy = {})
        : destroyed_(destroyed), on_destroy_(std::move(on_destroy)) {}
    ~Proxy() {
        ++destroyed_;
        if (on_destroy_) on_destroy_();
    }
    std::atomic<unsigned>& destroyed_;
    std::function<void()> on_destroy_;
};
using Registry = IdentityRegistry<Proxy>;

void PinAndReuseAddress() {
    std::atomic<unsigned> destroyed{0};
    Registry registry(2);
    auto first = registry.Acquire(0x1234, std::make_shared<Proxy>(destroyed));
    assert(first.status == IdentityStatus::Inserted && first.generation == 1 && first);
    auto pinned = registry.Find(0x1234);
    assert(pinned.status == IdentityStatus::Existing && pinned.object == first.object);
    first.object.reset();
    assert(destroyed == 0);
    auto duplicate = registry.Acquire(0x1234, std::make_shared<Proxy>(destroyed));
    assert(duplicate.status == IdentityStatus::Existing && duplicate.object == pinned.object);
    assert(destroyed == 1);  // losing candidate
    assert(!registry.ForgetExpired(0x1234, first.generation));
    pinned.object.reset();
    duplicate.object.reset();
    assert(destroyed == 2);

    auto second = registry.Acquire(0x1234, std::make_shared<Proxy>(destroyed));
    assert(second.status == IdentityStatus::Inserted && second.generation != first.generation);
    assert(!registry.ForgetExpired(0x1234, first.generation));
    assert(!registry.ForgetExpired(0x1234, second.generation));
    const auto generation = second.generation;
    second.object.reset();
    assert(destroyed == 3);
    assert(!registry.ForgetExpired(0x1234, first.generation));
    assert(registry.ForgetExpired(0x1234, generation));
    assert(registry.Find(0x1234).status == IdentityStatus::NotFound);
}

void CapacityAndFailureAreConservative() {
    std::atomic<unsigned> destroyed{0};
    Registry registry(1);
    assert(registry.Acquire(0, std::make_shared<Proxy>(destroyed)).status == IdentityStatus::InvalidIdentity);
    assert(registry.Acquire(1, {}).status == IdentityStatus::InvalidCandidate);
    assert(registry.Find(0).status == IdentityStatus::InvalidIdentity);
    auto first = registry.Acquire(1, std::make_shared<Proxy>(destroyed));
    assert(first.status == IdentityStatus::Inserted);
    auto full = registry.Acquire(2, std::make_shared<Proxy>(destroyed));
    assert(full.status == IdentityStatus::CapacityExceeded && !full);
    assert(registry.Find(1).object == first.object);
    first.object.reset();
    auto replacement = registry.Acquire(2, std::make_shared<Proxy>(destroyed));
    assert(replacement.status == IdentityStatus::Inserted);  // evicts an expired entry
    assert(registry.Find(1).status == IdentityStatus::NotFound);

    Registry empty(0);
    assert(empty.Acquire(3, std::make_shared<Proxy>(destroyed)).status == IdentityStatus::CapacityExceeded);
    Registry exhausted(2, std::numeric_limits<std::uint64_t>::max());
    auto last = exhausted.Acquire(4, std::make_shared<Proxy>(destroyed));
    assert(last.status == IdentityStatus::Inserted);
    assert(exhausted.Acquire(4, std::make_shared<Proxy>(destroyed)).status == IdentityStatus::Existing);
    assert(exhausted.Acquire(5, std::make_shared<Proxy>(destroyed)).status == IdentityStatus::GenerationExhausted);
    last.object.reset();
    assert(exhausted.Acquire(4, std::make_shared<Proxy>(destroyed)).status == IdentityStatus::GenerationExhausted);
}

void CandidateDestructionCanReenter() {
    std::atomic<unsigned> destroyed{0};
    Registry registry(1);
    auto live = registry.Acquire(1, std::make_shared<Proxy>(destroyed));
    bool called = false;
    auto candidate = std::make_shared<Proxy>(destroyed, [&] {
        called = true;
        assert(registry.Find(1).object == live.object);
    });
    auto duplicate = registry.Acquire(1, std::move(candidate));
    assert(called && duplicate.object == live.object);
    called = false;
    auto rejected = std::make_shared<Proxy>(destroyed, [&] {
        called = true;
        assert(registry.Find(1).object == live.object);
    });
    assert(registry.Acquire(2, std::move(rejected)).status == IdentityStatus::CapacityExceeded);
    assert(called);
}

// An expired entry may own the last weak control-block reference. Its custom
// allocator can reenter the table when deallocated; node retirement must happen
// outside the mutex just like destruction of a losing candidate.
template <typename T>
struct ReentrantAllocator {
    using value_type = T;
    Registry* registry;
    std::atomic<unsigned>* deallocated;

    ReentrantAllocator(Registry* r, std::atomic<unsigned>* d) noexcept : registry(r), deallocated(d) {}
    template <typename U>
    ReentrantAllocator(const ReentrantAllocator<U>& other) noexcept
        : registry(other.registry), deallocated(other.deallocated) {}
    T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
    void deallocate(T* value, std::size_t n) noexcept {
        (void)registry->Find(2);
        ++*deallocated;
        std::allocator<T>{}.deallocate(value, n);
    }
    template <typename U> bool operator==(const ReentrantAllocator<U>& other) const noexcept {
        return registry == other.registry && deallocated == other.deallocated;
    }
    template <typename U> bool operator!=(const ReentrantAllocator<U>& other) const noexcept {
        return !(*this == other);
    }
};

void ExpiredNodeDestructionCanReenter() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        std::atomic<unsigned> destroyed{0}, deallocated{0};
        Registry registry(1);
        auto proxy = std::allocate_shared<Proxy>(ReentrantAllocator<Proxy>(&registry, &deallocated), destroyed);
        auto first = registry.Acquire(1, std::move(proxy));
        const auto generation = first.generation;
        first.object.reset();
        assert(destroyed == 1 && deallocated == 0);
        if (mode == 0) {
            auto second = registry.Acquire(2, std::make_shared<Proxy>(destroyed));
            assert(second.status == IdentityStatus::Inserted);
        } else if (mode == 1) {
            auto second = registry.Acquire(1, std::make_shared<Proxy>(destroyed));
            assert(second.status == IdentityStatus::Inserted);
        } else if (mode == 2) {
            assert(registry.Find(1).status == IdentityStatus::NotFound);
        } else if (mode == 3) {
            assert(registry.ForgetExpired(1, generation));
        } else {
            assert(registry.Retire(1, generation));
        }
        assert(deallocated == 1);
    }
}

// Illustrates the facade's obligation; the registry itself does not implement
// or know about COM AddRef, Release, or native-resource lifetime.
struct FakeComControl {
    explicit FakeComControl(std::atomic<unsigned>& destroyed) : destroyed_(destroyed) {}
    ~FakeComControl() { ++destroyed_; }
    bool TryAddRef() {
        unsigned count = external_refs.load(std::memory_order_relaxed);
        while (count) {
            if (external_refs.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel)) return true;
        }
        return false;
    }
    void Release() {
        const unsigned previous = external_refs.fetch_sub(1, std::memory_order_acq_rel);
        assert(previous > 0);
    }
    std::atomic<unsigned> external_refs{1};
    std::atomic<unsigned>& destroyed_;
};

void RetirePinnedZeroRefGeneration() {
    std::atomic<unsigned> destroyed{0};
    IdentityRegistry<FakeComControl> registry(1);
    auto first = registry.Acquire(7, std::make_shared<FakeComControl>(destroyed));
    auto old_pin = registry.Find(7);
    const auto old_generation = first.generation;
    assert(old_pin && old_pin.object == first.object);
    first.object->Release();  // the old control is pinned but its COM refs are zero
    assert(!old_pin.object->TryAddRef());
    assert(!registry.ForgetExpired(7, old_generation));
    assert(registry.Retire(7, old_generation));

    auto replacement = registry.Acquire(7, std::make_shared<FakeComControl>(destroyed));
    assert(replacement.status == IdentityStatus::Inserted && replacement.generation != old_generation);
    assert(replacement.object != old_pin.object && destroyed == 0);
    assert(!registry.Retire(7, old_generation));  // delayed final Release cannot erase the new generation
    assert(!registry.ForgetExpired(7, old_generation));
    auto fresh = registry.Find(7);
    assert(fresh && fresh.object == replacement.object && fresh.object->TryAddRef());
    fresh.object->Release();
    replacement.object->Release();
    assert(!fresh.object->TryAddRef());
    assert(registry.Retire(7, replacement.generation));
    assert(registry.Find(7).status == IdentityStatus::NotFound);
    first.object.reset();
    old_pin.object.reset();
    replacement.object.reset();
    fresh.object.reset();
    assert(destroyed == 2);
}

void DelayedRetireRacesReplacement() {
    std::atomic<unsigned> destroyed{0};
    std::atomic<unsigned> phase{0};
    IdentityRegistry<FakeComControl> registry(1);
    auto first = registry.Acquire(7, std::make_shared<FakeComControl>(destroyed));
    const auto old_generation = first.generation;
    first.object->Release();
    IdentityRegistry<FakeComControl>::Handle replacement;
    std::thread delayed([&] {
        auto stale = registry.Find(7);
        assert(stale && stale.generation == old_generation && !stale.object->TryAddRef());
        phase.store(1, std::memory_order_release);
        while (phase.load(std::memory_order_acquire) != 2) std::this_thread::yield();
        assert(!registry.Retire(7, old_generation));
        auto retried = registry.Acquire(7, std::make_shared<FakeComControl>(destroyed));
        assert(retried.status == IdentityStatus::Existing && retried.object == replacement.object);
        assert(retried.object->TryAddRef());
        retried.object->Release();
    });
    while (phase.load(std::memory_order_acquire) != 1) std::this_thread::yield();
    assert(registry.Retire(7, old_generation));
    replacement = registry.Acquire(7, std::make_shared<FakeComControl>(destroyed));
    assert(replacement.status == IdentityStatus::Inserted && replacement.generation != old_generation);
    phase.store(2, std::memory_order_release);
    delayed.join();
    replacement.object->Release();
    assert(registry.Retire(7, replacement.generation));
}

void ConcurrentDeduplicationAndPins() {
    constexpr unsigned workers = 16;
    constexpr unsigned iterations = 2000;
    std::atomic<unsigned> destroyed{0}, ready{0}, failures{0};
    std::atomic<bool> go{false};
    Registry registry(workers + 1);
    std::vector<Registry::Handle> results(workers);
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i) {
        threads.emplace_back([&, i] {
            auto candidate = std::make_shared<Proxy>(destroyed);
            ++ready;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            results[i] = registry.Acquire(0x10, std::move(candidate));
            if (!results[i]) { ++failures; return; }
            for (unsigned n = 0; n < iterations; ++n) {
                auto pinned = registry.Find(0x10);
                if (!pinned || pinned.generation != results[i].generation || pinned.object != results[i].object)
                    ++failures;
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != workers) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    assert(failures == 0);
    unsigned inserted = 0;
    for (const auto& result : results) {
        assert(result.object == results[0].object && result.generation == results[0].generation);
        inserted += result.status == IdentityStatus::Inserted;
    }
    assert(inserted == 1 && destroyed == workers - 1);
    const auto generation = results[0].generation;
    results.clear();
    assert(destroyed == workers);
    assert(registry.ForgetExpired(0x10, generation));
}

void FindRacesFinalRelease() {
    constexpr unsigned attempts = 300;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        std::atomic<unsigned> destroyed{0}, ready{0};
        std::atomic<bool> go{false};
        Registry registry(1);
        auto owner = registry.Acquire(1, std::make_shared<Proxy>(destroyed));
        const auto generation = owner.generation;
        Registry::Handle result;
        std::thread release([&, held = std::move(owner)]() mutable {
            ++ready;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            held.object.reset();
        });
        std::thread find([&] {
            ++ready;
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            result = registry.Find(1);
        });
        while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
        go.store(true, std::memory_order_release);
        release.join();
        find.join();
        if (result) {
            assert(result.generation == generation && destroyed == 0);
            result.object.reset();
        } else {
            assert(result.status == IdentityStatus::NotFound);
        }
        assert(destroyed == 1);
    }
}
}  // namespace

int main() {
    PinAndReuseAddress();
    CapacityAndFailureAreConservative();
    CandidateDestructionCanReenter();
    ExpiredNodeDestructionCanReenter();
    RetirePinnedZeroRefGeneration();
    DelayedRetireRacesReplacement();
    ConcurrentDeduplicationAndPins();
    FindRacesFinalRelease();
}
