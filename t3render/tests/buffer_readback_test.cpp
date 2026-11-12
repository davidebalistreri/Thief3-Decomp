#include "d3d8_buffer_readback.hpp"
#include "d3d8_buffer_snapshot.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>

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
    std::array<unsigned char, 3> verified{};
    const auto verify = [&] {
        return d3d8_fixture::VerifyBufferRange(&buffer, 2, 3, expected, readonly, "fixture", ReadSnapshot, &reader,
                                               verified.data(), verified.size());
    };
    assert(verify());
    assert((verified == std::array<unsigned char, 3>{2, 3, 4}));
    verified.fill(0xee);
    assert(buffer.locks == 1 && buffer.unlocks == 1 && buffer.offset == 2 && buffer.length == 3 && buffer.flags == readonly);
    buffer.lock_result = -1;
    assert(!verify());
    assert((verified == std::array<unsigned char, 3>{0xee, 0xee, 0xee}));
    assert(buffer.locks == 2 && buffer.unlocks == 1);
    buffer.lock_result = 0; buffer.null_mapping = true;
    assert(!verify());
    assert((verified == std::array<unsigned char, 3>{0xee, 0xee, 0xee}));
    assert(buffer.unlocks == 2);
    buffer.null_mapping = false; buffer.bytes[3] = 99;
    assert(!verify());
    assert((verified == std::array<unsigned char, 3>{0xee, 0xee, 0xee}));
    assert(buffer.unlocks == 3);
    buffer.bytes[3] = 3; buffer.unlock_result = -1;
    assert(!verify());
    assert((verified == std::array<unsigned char, 3>{0xee, 0xee, 0xee}));
    assert(buffer.unlocks == 4);
    assert(!d3d8_fixture::VerifyBufferRange(&buffer, 2, 0, expected, readonly, "fixture", ReadSnapshot, &reader));
    assert(!d3d8_fixture::VerifyBufferRange(&buffer, 0, 257, expected, readonly, "fixture", ReadSnapshot, &reader));
    assert(buffer.locks == 5 && buffer.unlocks == 4);
    buffer.unlock_result = 0;
    reader.fail = true;
    assert(!verify() && buffer.unlocks == 5);
    assert((verified == std::array<unsigned char, 3>{0xee, 0xee, 0xee}));
    reader.fail = false; reader.partial = true;
    assert(!verify() && buffer.unlocks == 6);
    assert((verified == std::array<unsigned char, 3>{0xee, 0xee, 0xee}));
    reader.partial = false; reader.throws = true;
    assert(!verify() && buffer.unlocks == 7);
    assert((verified == std::array<unsigned char, 3>{0xee, 0xee, 0xee}));
    reader.throws = false;
    assert(verify() && buffer.unlocks == 8);
    assert((verified == std::array<unsigned char, 3>{2, 3, 4}));
    assert(!d3d8_fixture::VerifyBufferRange(&buffer, 2, 3, expected, readonly, "fixture", ReadSnapshot, &reader,
                                             verified.data(), 2));
    assert(buffer.locks == 9 && buffer.unlocks == 8);

    std::array<std::uint8_t, d3d8_fixture::kVertexRangeBytes> vertex{};
    vertex[1] = 0xff; vertex[2] = 0xa5; vertex.back() = 0x12;
    const std::array<std::uint8_t, d3d8_fixture::kIndexRangeBytes> index{{0, 1, 0xfe, 0xff, 0x12, 0x34}};
    d3d8_fixture::BufferPacket packet{};
    packet.fill('x');
    const auto original_packet = packet;
    assert(!d3d8_fixture::EncodeBufferPacket(vertex.data(), vertex.size() - 1, index.data(), index.size(), packet));
    assert(packet == original_packet);
    assert(d3d8_fixture::EncodeBufferPacket(vertex.data(), vertex.size(), index.data(), index.size(), packet));
    std::string vertex_hex(vertex.size() * 2, '0');
    vertex_hex.replace(2, 2, "ff"); vertex_hex.replace(4, 2, "a5"); vertex_hex.replace(158, 2, "12");
    assert(std::string(packet.begin(), packet.end()) ==
           std::string(d3d8_fixture::kBufferHeader) + vertex_hex + "\n0001feff1234\n");
}
