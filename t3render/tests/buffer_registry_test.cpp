#include "../ir/buffer_registry.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace t3render::ir;

namespace {
using R = RegistryResult;
using A = BufferAvailability;
using L = BufferLock;

struct ReaderContext {
    const std::uint8_t* bytes;
    int calls = 0;
    bool partial = false;
    bool throws = false;
};

bool SnapshotReader(void* context, void* destination, std::size_t length) {
    auto& reader = *static_cast<ReaderContext*>(context);
    ++reader.calls;
    if (reader.throws) throw std::runtime_error("reader failed");
    std::memcpy(destination, reader.bytes, reader.partial ? length / 2 : length);
    return !reader.partial;
}

BufferToken Register(BufferRegistry& registry, std::uintptr_t identity, std::size_t size, A expected = A::Tracked) {
    BufferToken token{};
    A availability = A::Abandoned;
    assert(registry.Register(identity, size, token, availability) == R::Ok);
    assert(token.identity == identity && token.generation != 0 && availability == expected);
    return token;
}

void Write(BufferRegistry& registry, BufferToken token, std::size_t offset,
           const void* bytes, std::size_t length, L mode = L::Write) {
    assert(registry.BeginLock(token, offset, length, mode) == R::Ok);
    assert(registry.StageUnlock(token, bytes, length) == R::Ok);
    assert(registry.EndUnlock(token, true) == R::Ok);
}

void GenerationsAndLimits() {
    BufferRegistry registry(128, 2);
    BufferToken unchanged{7, 9};
    A availability = A::Abandoned;
    assert(registry.Register(0, 1, unchanged, availability) == R::InvalidIdentity);
    assert(unchanged.identity == 7 && unchanged.generation == 9 && availability == A::Abandoned);
    const auto first = Register(registry, 42, 8);
    assert(registry.used_bytes() == 9 && registry.resource_count() == 1);
    assert(registry.Register(42, 8, unchanged, availability) == R::AlreadyTracked);
    assert(registry.Availability(first, availability) == R::Ok && availability == A::Abandoned);
    assert(registry.used_bytes() == 0);
    const auto second = Register(registry, 43, 8);
    assert(registry.Register(44, 8, unchanged, availability) == R::CapacityExceeded);
    assert(registry.Release(first) == R::Ok && registry.used_bytes() == 9);
    const auto reused = Register(registry, 42, 8);
    assert(reused.generation != first.generation && registry.used_bytes() == 18);
    std::uint8_t out = 0;
    assert(registry.Read(first, 0, &out, 1) == R::StaleGeneration);
    assert(registry.Release(first) == R::StaleGeneration);
    assert(registry.Availability(first, availability) == R::StaleGeneration);
    assert(registry.Release(reused) == R::Ok);
    assert(registry.Read(reused, 0, &out, 1) == R::UnknownResource);
    assert(registry.Release(second) == R::Ok && registry.used_bytes() == 0 && registry.resource_count() == 0);
}

void PartialWritesAndStaging() {
    BufferRegistry registry(128, 2);
    const auto token = Register(registry, 1, 8);
    std::array<std::uint8_t, 8> out{};
    out.fill(0xee);
    assert(registry.Read(token, 0, out.data(), out.size()) == R::UnknownData);
    for (auto value : out) assert(value == 0xee);
    std::array<std::uint8_t, 4> source{1, 2, 3, 4};
    assert(registry.BeginLock(token, 2, 4, L::Write) == R::Ok);
    assert(registry.Read(token, 0, out.data(), 1) == R::Busy);
    assert(registry.StageUnlock(token, source.data(), source.size()) == R::Ok);
    assert(registry.used_bytes() == 13);
    source.fill(99);  // The caller's copy can change after staging.
    assert(registry.EndUnlock(token, true) == R::Ok && registry.used_bytes() == 9);
    assert(registry.Read(token, 2, out.data(), 4) == R::Ok);
    assert((std::array<std::uint8_t, 4>{out[0], out[1], out[2], out[3]} ==
            std::array<std::uint8_t, 4>{1, 2, 3, 4}));
    out.fill(0xee);
    assert(registry.Read(token, 0, out.data(), out.size()) == R::UnknownData);
    for (auto value : out) assert(value == 0xee);

    const std::array<std::uint8_t, 2> left{5, 6}, right{7, 8};
    Write(registry, token, 0, left.data(), left.size());
    Write(registry, token, 6, right.data(), right.size());
    assert(registry.Read(token, 0, out.data(), out.size()) == R::Ok);
    assert((out == std::array<std::uint8_t, 8>{5, 6, 1, 2, 3, 4, 7, 8}));

    assert(registry.BeginLock(token, 3, 2, L::Write) == R::Ok);
    assert(registry.StageUnlock(token, left.data(), 2) == R::Ok);
    // Failed re-staging must not accidentally commit stale staged bytes.
    assert(registry.StageUnlock(token, left.data(), 1) == R::InvalidRange);
    assert(registry.used_bytes() == 9);
    assert(registry.EndUnlock(token, true) == R::DataNotStaged);
    assert(registry.Read(token, 0, out.data(), 3) == R::Ok);
    assert(registry.Read(token, 3, out.data(), 2) == R::UnknownData);
    assert(registry.Read(token, 5, out.data(), 3) == R::Ok);
    assert(registry.StageUnlock(token, left.data(), 2) == R::NoActiveLock);
    assert(registry.EndUnlock(token, true) == R::NoActiveLock);
    assert(registry.Read(token, 7, out.data(), 2) == R::InvalidRange);
    assert(registry.Read(token, std::numeric_limits<std::size_t>::max(), out.data(), 1) == R::InvalidRange);
    assert(registry.Read(token, 0, nullptr, 1) == R::NullPointer);
    assert(registry.BeginLock(token, 0, 2, L::Write) == R::Ok);
    assert(registry.StageUnlock(token, left.data(), 2) == R::Ok);
    assert(registry.StageUnlock(token, nullptr, 2) == R::NullPointer);
    assert(registry.EndUnlock(token, true) == R::DataNotStaged);
    assert(registry.Read(token, 0, out.data(), 2) == R::UnknownData);
}

void DiscardReadOnlyAndFailures() {
    BufferRegistry registry(128, 4);
    const auto token = Register(registry, 1, 8);
    const std::array<std::uint8_t, 8> original{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<std::uint8_t, 8> out{};
    Write(registry, token, 0, original.data(), original.size());
    assert(registry.BeginLock(token, 0, 8, L::ReadOnly) == R::Ok);
    assert(registry.StageUnlock(token, original.data(), 8) == R::InvalidOperation);
    assert(registry.EndUnlock(token, true) == R::Ok);
    assert(registry.Read(token, 0, out.data(), 8) == R::Ok && out == original);

    const std::array<std::uint8_t, 2> replacement{77, 88};
    Write(registry, token, 3, replacement.data(), 2, L::Discard);
    assert(registry.Read(token, 0, out.data(), 1) == R::UnknownData);
    assert(registry.Read(token, 3, out.data(), 2) == R::Ok && out[0] == 77 && out[1] == 88);
    assert(registry.Read(token, 5, out.data(), 1) == R::UnknownData);

    assert(registry.BeginLock(token, 3, 2, L::Write) == R::Ok);
    assert(registry.StageUnlock(token, replacement.data(), 2) == R::Ok);
    assert(registry.EndUnlock(token, false) == R::Ok);
    A state = A::Tracked;
    assert(registry.Availability(token, state) == R::Ok && state == A::Abandoned);
    assert(registry.Read(token, 3, out.data(), 2) == R::Unavailable);
    assert(registry.used_bytes() == 0);
    assert(registry.BeginLock(token, 0, 1, L::Write) == R::Unavailable);

    const auto overlap = Register(registry, 2, 8);
    Write(registry, overlap, 0, original.data(), original.size());
    assert(registry.BeginLock(overlap, 0, 2, L::Write) == R::Ok);
    assert(registry.BeginLock(overlap, 4, 2, L::Write) == R::OverlappingLocks);
    assert(registry.Read(overlap, 0, out.data(), 1) == R::Unavailable);
    assert(registry.used_bytes() == 0);
    const auto invalid = Register(registry, 3, 8);
    assert(registry.BeginLock(invalid, 7, 2, L::Write) == R::InvalidRange);
    assert(registry.Availability(invalid, state) == R::Ok && state == A::Abandoned);
    const auto unknown = Register(registry, 4, 8);
    Write(registry, unknown, 0, original.data(), original.size());
    assert(registry.Abandon(unknown) == R::Ok);
    assert(registry.Read(unknown, 0, out.data(), 1) == R::Unavailable);
    assert(registry.used_bytes() == 0);
}

void GlobalBudgetAndOverflow() {
    BufferRegistry registry(18, 4);
    const auto first = Register(registry, 1, 8);
    const auto second = Register(registry, 2, 8);
    const auto unavailable = Register(registry, 3, 1, A::BudgetExceeded);
    const auto huge = Register(registry, 4, std::numeric_limits<std::size_t>::max(), A::TooLarge);
    assert(registry.used_bytes() == 18);
    const std::array<std::uint8_t, 8> source{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<std::uint8_t, 8> out{};
    assert(registry.BeginLock(first, 0, 8, L::Write) == R::Ok);
    assert(registry.StageUnlock(first, source.data(), 8) == R::BudgetExceeded);
    ReaderContext reader{source.data()};
    assert(registry.StageUnlockFromReader(first, SnapshotReader, &reader) == R::BudgetExceeded);
    assert(reader.calls == 0);  // The global budget is checked before copying.
    assert(registry.EndUnlock(first, true) == R::DataNotStaged);
    assert(registry.Read(first, 0, out.data(), 8) == R::UnknownData);
    assert(registry.Release(second) == R::Ok && registry.used_bytes() == 9);
    assert(registry.BeginLock(first, 0, 8, L::Write) == R::Ok);
    assert(registry.StageUnlock(first, source.data(), 8) == R::Ok && registry.used_bytes() == 17);
    assert(registry.Release(first) == R::Ok && registry.used_bytes() == 0);
    assert(registry.Read(first, 0, out.data(), 1) == R::UnknownResource);
    // A resource registered without budget must remain unavailable, even if
    // budget becomes available later; no prior writes have been observed.
    assert(registry.BeginLock(unavailable, 0, 1, L::Write) == R::Ok);
    assert(registry.StageUnlock(unavailable, source.data(), 1) == R::Unavailable);
    assert(registry.EndUnlock(unavailable, true) == R::Unavailable);
    assert(registry.Read(unavailable, 0, out.data(), 1) == R::Unavailable);
    assert(registry.Release(unavailable) == R::Ok);
    assert(registry.Release(huge) == R::Ok && registry.used_bytes() == 0);
}

void FaultSafeReader() {
    BufferRegistry registry(32, 1);
    const auto token = Register(registry, 9, 8);
    std::array<std::uint8_t, 8> source{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<std::uint8_t, 8> out{};
    ReaderContext reader{source.data()};
    assert(registry.BeginLock(token, 0, 8, L::Write) == R::Ok);
    assert(registry.StageUnlockFromReader(token, SnapshotReader, &reader) == R::Ok);
    assert(reader.calls == 1 && registry.used_bytes() == 17);
    // A failed replacement snapshot must drop the previous successful copy.
    reader.partial = true;
    assert(registry.StageUnlockFromReader(token, SnapshotReader, &reader) == R::ReadFailed);
    assert(reader.calls == 2 && registry.used_bytes() == 9);
    assert(registry.EndUnlock(token, true) == R::DataNotStaged);
    assert(registry.Read(token, 0, out.data(), 8) == R::UnknownData);
    assert(registry.BeginLock(token, 0, 8, L::Write) == R::Ok);
    reader.throws = true;
    assert(registry.StageUnlockFromReader(token, SnapshotReader, &reader) == R::ReadFailed);
    assert(registry.EndUnlock(token, true) == R::DataNotStaged);
    assert(registry.used_bytes() == 9);
    assert(registry.BeginLock(token, 0, 8, L::Write) == R::Ok);
    assert(registry.StageUnlockFromReader(token, nullptr, nullptr) == R::NullPointer);
    assert(registry.EndUnlock(token, true) == R::DataNotStaged);
    assert(registry.BeginLock(token, 0, 8, L::Write) == R::Ok);
    reader.partial = false;
    reader.throws = false;
    assert(registry.StageUnlockFromReader(token, SnapshotReader, &reader) == R::Ok);
    source.fill(99);
    assert(registry.EndUnlock(token, true) == R::Ok);
    assert(registry.Read(token, 0, out.data(), 8) == R::Ok);
    assert((out == std::array<std::uint8_t, 8>{1, 2, 3, 4, 5, 6, 7, 8}));
}
}  // namespace

int main() {
    GenerationsAndLimits();
    PartialWritesAndStaging();
    DiscardReadOnlyAndFailures();
    GlobalBudgetAndOverflow();
    FaultSafeReader();
}
