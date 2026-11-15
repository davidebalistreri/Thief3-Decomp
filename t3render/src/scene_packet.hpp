#pragma once

// Local SceneIR backend packet. The format is little-endian and contains only
// copied values; it is not a D3D8 replay or an engine-object serialization.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace t3render::scene {
constexpr std::size_t kMaxPacketBytes = 64u * 1024u * 1024u;
constexpr std::uint32_t kMaxTextures = 64, kMaxDraws = 8192, kMaxTriangles = 100000;

struct Vertex { float x, y, z, u, v; };
static_assert(sizeof(Vertex) == 20, "unexpected vertex layout");
struct Mip { std::uint32_t width, height; std::vector<std::uint8_t> rgba; };
struct Texture { std::vector<Mip> mips; };
struct Draw { std::uint32_t texture, first_vertex, vertex_count; };
struct Frame {
    std::uint32_t width = 0, height = 0;
    float min_depth = 0, max_depth = 1;
    float view[16]{}, projection[16]{};
    std::vector<Texture> textures;
    std::vector<Vertex> vertices;
    std::vector<Draw> draws;
};

class Reader {
    const std::uint8_t* data_;
    std::size_t size_, offset_ = 0;
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    bool bytes(void* dst, std::size_t n) {
        if (n > size_ - offset_) return false;
        std::memcpy(dst, data_ + offset_, n);
        offset_ += n;
        return true;
    }
    bool u32(std::uint32_t& value) {
        std::uint8_t b[4];
        if (!bytes(b, sizeof(b))) return false;
        value = std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) |
                (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
        return true;
    }
    bool f32(float& value, float limit = 1000000.0f) {
        std::uint32_t bits;
        if (!u32(bits)) return false;
        static_assert(sizeof(value) == sizeof(bits), "float must be 32 bits");
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value) && std::fabs(value) <= limit;
    }
    bool end() const { return offset_ == size_; }
    std::size_t remaining() const { return size_ - offset_; }
};

inline bool ParsePacket(const std::uint8_t* bytes, std::size_t size, Frame& output) {
    if (!bytes || size > kMaxPacketBytes) return false;
    Reader reader(bytes, size);
    char magic[8];
    const char expected[8] = {'T', '3', 'S', 'C', 'N', '0', '1', '\0'};
    Frame frame;
    if (!reader.bytes(magic, sizeof(magic)) || std::memcmp(magic, expected, sizeof(magic)) ||
        !reader.u32(frame.width) || !reader.u32(frame.height) || !frame.width || !frame.height ||
        frame.width > 2048 || frame.height > 2048 || !reader.f32(frame.min_depth) ||
        !reader.f32(frame.max_depth) || frame.min_depth < 0 || frame.max_depth > 1 ||
        frame.min_depth > frame.max_depth) return false;
    for (float& x : frame.view) if (!reader.f32(x, 10000.0f)) return false;
    for (float& x : frame.projection) if (!reader.f32(x, 10000.0f)) return false;
    std::uint32_t texture_count, draw_count;
    if (!reader.u32(texture_count) || !reader.u32(draw_count) || !texture_count || !draw_count ||
        texture_count > kMaxTextures || draw_count > kMaxDraws) return false;
    frame.textures.reserve(texture_count);
    for (std::uint32_t i = 0; i < texture_count; ++i) {
        std::uint32_t width, height, count;
        if (!reader.u32(width) || !reader.u32(height) || !reader.u32(count) || !width || !height ||
            width > 2048 || height > 2048 || !count || count > 12) return false;
        Texture texture;
        texture.mips.reserve(count);
        for (std::uint32_t level = 0; level < count; ++level) {
            const std::size_t bytes_count = std::size_t(width) * height * 4;
            if (bytes_count > reader.remaining()) return false;
            Mip mip{width, height, std::vector<std::uint8_t>(bytes_count)};
            if (!reader.bytes(mip.rgba.data(), bytes_count)) return false;
            texture.mips.push_back(std::move(mip));
            if (width == 1 && height == 1 && level + 1 != count) return false;
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
        }
        frame.textures.push_back(std::move(texture));
    }
    frame.draws.reserve(draw_count);
    for (std::uint32_t i = 0; i < draw_count; ++i) {
        std::uint32_t texture, count;
        if (!reader.u32(texture) || !reader.u32(count) || texture >= texture_count || !count || count % 3 ||
            count > kMaxTriangles * 3 - frame.vertices.size() || std::size_t(count) * 20 > reader.remaining()) return false;
        Draw draw{texture, static_cast<std::uint32_t>(frame.vertices.size()), count};
        for (std::uint32_t n = 0; n < count; ++n) {
            Vertex vertex{};
            if (!reader.f32(vertex.x) || !reader.f32(vertex.y) || !reader.f32(vertex.z) ||
                !reader.f32(vertex.u) || !reader.f32(vertex.v)) return false;
            frame.vertices.push_back(vertex);
        }
        frame.draws.push_back(draw);
    }
    if (!reader.end()) return false;
    output = std::move(frame);
    return true;
}
} // namespace t3render::scene
