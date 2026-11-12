#include "d3d8_buffer_readback.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>

struct FakeBuffer {
    std::array<unsigned char, 8> bytes{{0, 1, 2, 3, 4, 5, 6, 7}};
    std::int32_t lock_result = 0, unlock_result = 0;
    bool null_mapping = false;
    int locks = 0, unlocks = 0;
    unsigned offset = 0, length = 0, flags = 0;
    std::int32_t Lock(unsigned o, unsigned n, unsigned char** out, unsigned f) {
        ++locks; offset = o; length = n; flags = f;
        if (lock_result >= 0) *out = null_mapping ? nullptr : bytes.data() + o;
        return lock_result;
    }
    std::int32_t Unlock() { ++unlocks; return unlock_result; }
};

struct FakeReader { bool fail = false, partial = false, throws = false; int calls = 0; };

bool ReadSnapshot(void* context, const void* source, void* destination, std::size_t length) {
    auto& reader = *static_cast<FakeReader*>(context);
    ++reader.calls;
    if (reader.throws) throw std::runtime_error("read failed");
    if (reader.fail) return false;
    std::memcpy(destination, source, reader.partial ? length - 1 : length);
    return !reader.partial;
}

int main() {
    constexpr unsigned readonly = 0x10;
    constexpr unsigned char expected[]{2, 3, 4};
    FakeBuffer buffer;
    FakeReader reader;
    const auto verify = [&] {
        return d3d8_fixture::VerifyBufferRange(&buffer, 2, 3, expected, readonly, "fixture", ReadSnapshot, &reader);
    };
    assert(verify());
    assert(buffer.locks == 1 && buffer.unlocks == 1 && buffer.offset == 2 && buffer.length == 3 && buffer.flags == readonly);
    buffer.lock_result = -1;
    assert(!verify());
    assert(buffer.locks == 2 && buffer.unlocks == 1);
    buffer.lock_result = 0; buffer.null_mapping = true;
    assert(!verify());
    assert(buffer.unlocks == 2);
    buffer.null_mapping = false; buffer.bytes[3] = 99;
    assert(!verify());
    assert(buffer.unlocks == 3);
    buffer.bytes[3] = 3; buffer.unlock_result = -1;
    assert(!verify());
    assert(buffer.unlocks == 4);
    assert(!d3d8_fixture::VerifyBufferRange(&buffer, 2, 0, expected, readonly, "fixture", ReadSnapshot, &reader));
    assert(!d3d8_fixture::VerifyBufferRange(&buffer, 0, 257, expected, readonly, "fixture", ReadSnapshot, &reader));
    assert(buffer.locks == 5 && buffer.unlocks == 4);
    buffer.unlock_result = 0;
    reader.fail = true;
    assert(!verify() && buffer.unlocks == 5);
    reader.fail = false; reader.partial = true;
    assert(!verify() && buffer.unlocks == 6);
    reader.partial = false; reader.throws = true;
    assert(!verify() && buffer.unlocks == 7);
    reader.throws = false;
    assert(verify() && buffer.unlocks == 8);
}
