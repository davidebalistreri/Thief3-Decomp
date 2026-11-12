#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace d3d8_fixture {
constexpr std::size_t kVertexRangeBytes = 80;
constexpr std::size_t kIndexRangeBytes = 6;
constexpr char kBufferHeader[] = "T3R_BUFFER_V1\n256 256\n4278190080\n1 1 3 2\n20 20 80\n4 6\n";
constexpr std::size_t kBufferPacketBytes = sizeof(kBufferHeader) - 1 + kVertexRangeBytes * 2 + 1 + kIndexRangeBytes * 2 + 1;
using BufferPacket = std::array<char, kBufferPacketBytes>;

// Convert only successfully read-back fixture ranges. Sizes are fixed by the
// V1 fixture contract; invalid arguments leave the destination unchanged.
inline bool EncodeBufferPacket(const std::uint8_t* vertex, std::size_t vertex_size,
                               const std::uint8_t* index, std::size_t index_size, BufferPacket& output) {
    if (!vertex || vertex_size != kVertexRangeBytes || !index || index_size != kIndexRangeBytes) return false;
    constexpr char digits[] = "0123456789abcdef";
    std::size_t position = 0;
    std::memcpy(output.data(), kBufferHeader, sizeof(kBufferHeader) - 1);
    position += sizeof(kBufferHeader) - 1;
    for (std::size_t i = 0; i < vertex_size; ++i) {
        output[position++] = digits[vertex[i] >> 4];
        output[position++] = digits[vertex[i] & 15];
    }
    output[position++] = '\n';
    for (std::size_t i = 0; i < index_size; ++i) {
        output[position++] = digits[index[i] >> 4];
        output[position++] = digits[index[i] & 15];
    }
    output[position] = '\n';
    return true;
}
}  // namespace d3d8_fixture
