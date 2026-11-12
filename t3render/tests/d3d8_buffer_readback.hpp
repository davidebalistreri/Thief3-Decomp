#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace d3d8_fixture {

using SnapshotReader = bool (*)(void* context, const void* source, void* destination, std::size_t length);
constexpr std::size_t kMaxReadbackBytes = 256;

// Fixture-only diagnostic: Lock has real driver side effects and is not a
// passive observation. Always balance a successful Lock, even on mismatch.
template <class Buffer>
bool VerifyBufferRange(Buffer* buffer, unsigned offset, unsigned length, const void* expected,
                       unsigned read_only_flag, const char* name, SnapshotReader reader, void* context,
                       unsigned char* verified = nullptr, std::size_t verified_capacity = 0) {
    if (!buffer || !expected || !length || length > kMaxReadbackBytes || !reader ||
        (verified ? verified_capacity < length : verified_capacity != 0)) {
        std::fprintf(stderr, "%s: invalid readback arguments\n", name);
        return false;
    }
    unsigned char* mapped = nullptr;
    const auto lock_hr = buffer->Lock(offset, length, &mapped, read_only_flag);
    if (lock_hr < 0) {
        std::fprintf(stderr, "%s: READONLY Lock failed: HRESULT=0x%08lx\n", name,
                     static_cast<unsigned long>(static_cast<std::uint32_t>(lock_hr)));
        return false;
    }
    std::array<unsigned char, kMaxReadbackBytes> snapshot{};
    bool copied = false;
    if (mapped) {
        try { copied = reader(context, mapped, snapshot.data(), length); }
        catch (...) { copied = false; }
    }
    const bool matches = copied && std::memcmp(snapshot.data(), expected, length) == 0;
    const auto unlock_hr = buffer->Unlock();
    if (unlock_hr < 0) {
        std::fprintf(stderr, "%s: READONLY Unlock failed: HRESULT=0x%08lx\n", name,
                     static_cast<unsigned long>(static_cast<std::uint32_t>(unlock_hr)));
        return false;
    }
    if (!mapped) std::fprintf(stderr, "%s: READONLY Lock returned null\n", name);
    else if (!copied) std::fprintf(stderr, "%s: READONLY snapshot failed or was partial\n", name);
    else if (!matches) std::fprintf(stderr, "%s: READONLY bytes differ from fixture data\n", name);
    if (matches && verified) std::memcpy(verified, snapshot.data(), length);
    return matches;
}

}  // namespace d3d8_fixture
