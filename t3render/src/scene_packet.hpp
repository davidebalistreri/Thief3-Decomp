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

struct Vertex { float x, y, z, u, v, nx, ny, nz; };
static_assert(sizeof(Vertex) == 32, "unexpected vertex layout");
struct Mip { std::uint32_t width, height; std::vector<std::uint8_t> rgba; };
struct Texture { std::vector<Mip> mips; };
// API-neutral render-state vocabulary. Compare: never, less, equal, less/equal,
// greater, not equal, greater/equal, always (1..8); alpha compare 0 is disabled.
// Blend factors: zero, one, source color, inverse source color, source alpha,
// inverse source alpha, destination color, inverse destination color,
// destination alpha, inverse destination alpha (0..9). Blend operations: add,
// subtract, reverse subtract, min, max (0..4). Cull: none, back, front (0..2).
struct DrawStyle {
    std::uint32_t depth_enable = 1, depth_write = 1, depth_compare = 2, cull = 1;
    std::uint32_t blend_enable = 0, blend_src = 1, blend_dst = 0, blend_op = 0;
    std::uint32_t alpha_compare = 0, alpha_ref = 0, color_mask = 15;
    float mip_lod_bias = 0.0f;
    bool same_pipeline(const DrawStyle& rhs) const {
        // Alpha compare/reference and LOD bias are shader constants, not PSO state.
        return depth_enable == rhs.depth_enable && depth_write == rhs.depth_write && depth_compare == rhs.depth_compare &&
               cull == rhs.cull && blend_enable == rhs.blend_enable && blend_src == rhs.blend_src &&
               blend_dst == rhs.blend_dst && blend_op == rhs.blend_op && color_mask == rhs.color_mask;
    }
};
struct Draw {
    std::uint32_t texture, first_vertex, vertex_count;
    DrawStyle state;
    std::uint32_t shading = 0;
    float ambient_color[4]{};
    float light_position[3]{}, light_inverse_radius = 0;
    float light_color[3]{}, light_energy = 0;
    float spot_projector[3][4]{};
    std::uint32_t projector_texture = 0;
};
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
    const char v1[8] = {'T', '3', 'S', 'C', 'N', '0', '1', '\0'};
    const char v2[8] = {'T', '3', 'S', 'C', 'N', '0', '2', '\0'};
    const char v3[8] = {'T', '3', 'S', 'C', 'N', '0', '3', '\0'};
    const char v4[8] = {'T', '3', 'S', 'C', 'N', '0', '4', '\0'};
    const char v5[8] = {'T', '3', 'S', 'C', 'N', '0', '5', '\0'};
    const char v6[8] = {'T', '3', 'S', 'C', 'N', '0', '6', '\0'};
    const char v7[8] = {'T', '3', 'S', 'C', 'N', '0', '7', '\0'};
    const char v8[8] = {'T', '3', 'S', 'C', 'N', '0', '8', '\0'};
    Frame frame;
    if (!reader.bytes(magic, sizeof(magic)) ||
        (std::memcmp(magic, v1, sizeof(magic)) && std::memcmp(magic, v2, sizeof(magic)) &&
         std::memcmp(magic, v3, sizeof(magic)) && std::memcmp(magic, v4, sizeof(magic)) &&
         std::memcmp(magic, v5, sizeof(magic)) && std::memcmp(magic, v6, sizeof(magic)) &&
         std::memcmp(magic, v7, sizeof(magic)) && std::memcmp(magic, v8, sizeof(magic))) ||
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
            count > kMaxTriangles * 3 - frame.vertices.size()) return false;
        Draw draw{texture, static_cast<std::uint32_t>(frame.vertices.size()), count, {}};
        if (std::memcmp(magic, v1, sizeof(magic))) {
            auto& s = draw.state;
            if (!reader.u32(s.depth_enable) || !reader.u32(s.depth_write) || !reader.u32(s.depth_compare) ||
                !reader.u32(s.cull) || !reader.u32(s.blend_enable) || !reader.u32(s.blend_src) ||
                !reader.u32(s.blend_dst) || !reader.u32(s.blend_op) || !reader.u32(s.alpha_compare) ||
                !reader.u32(s.alpha_ref) || !reader.u32(s.color_mask) || s.depth_enable > 1 || s.depth_write > 1 ||
                !s.depth_compare || s.depth_compare > 8 || s.cull > 2 || s.blend_enable > 1 ||
                s.blend_src > 9 || s.blend_dst > 9 || s.blend_op > 4 || s.alpha_compare > 8 ||
                s.alpha_ref > 255 || s.color_mask > 15) return false;
            if ((!std::memcmp(magic, v3, sizeof(magic)) || !std::memcmp(magic, v4, sizeof(magic)) ||
                 !std::memcmp(magic, v5, sizeof(magic)) || !std::memcmp(magic, v6, sizeof(magic)) ||
                 !std::memcmp(magic, v7, sizeof(magic)) || !std::memcmp(magic, v8, sizeof(magic))) &&
                (!reader.f32(s.mip_lod_bias, 16.0f) || s.mip_lod_bias >= 16.0f)) return false;
            if (!std::memcmp(magic, v4, sizeof(magic)) || !std::memcmp(magic, v5, sizeof(magic)) ||
                !std::memcmp(magic, v6, sizeof(magic)) || !std::memcmp(magic, v7, sizeof(magic)) ||
                !std::memcmp(magic, v8, sizeof(magic))) {
                const std::uint32_t max_shading = !std::memcmp(magic, v8, sizeof(magic)) ? 6u :
                                                   !std::memcmp(magic, v7, sizeof(magic)) ? 5u :
                                                   !std::memcmp(magic, v6, sizeof(magic)) ? 4u :
                                                   !std::memcmp(magic, v5, sizeof(magic)) ? 3u : 1u;
                if (!reader.u32(draw.shading) || draw.shading > max_shading) return false;
                for (float& value : draw.ambient_color)
                    if (!reader.f32(value, 1.0f) || value < 0) return false;
            }
            if (!std::memcmp(magic, v5, sizeof(magic)) || !std::memcmp(magic, v6, sizeof(magic)) ||
                !std::memcmp(magic, v7, sizeof(magic)) || !std::memcmp(magic, v8, sizeof(magic))) {
                for (float& value : draw.light_position) if (!reader.f32(value)) return false;
                if (!reader.f32(draw.light_inverse_radius, 1.0f) || draw.light_inverse_radius < 0 ||
                    ((draw.shading == 2 || draw.shading == 4 || draw.shading == 5) && draw.light_inverse_radius == 0)) return false;
                for (float& value : draw.light_color)
                    if (!reader.f32(value, 1.0f) || value < 0) return false;
                if (!reader.f32(draw.light_energy, 4.0f) || draw.light_energy < 0) return false;
            }
            if (!std::memcmp(magic, v6, sizeof(magic)) || !std::memcmp(magic, v7, sizeof(magic)) ||
                !std::memcmp(magic, v8, sizeof(magic))) {
                for (auto& row : draw.spot_projector) for (float& value : row) if (!reader.f32(value)) return false;
                const auto& axis = draw.spot_projector[2];
                if ((draw.shading == 4 || draw.shading == 5) && axis[0]*axis[0] + axis[1]*axis[1] + axis[2]*axis[2] < 1e-12f) return false;
            }
            if ((!std::memcmp(magic, v7, sizeof(magic)) || !std::memcmp(magic, v8, sizeof(magic))) &&
                (!reader.u32(draw.projector_texture) || draw.projector_texture >= texture_count)) return false;
            if (draw.shading == 6 &&
                (s.depth_enable != 1 || s.depth_write != 0 || s.blend_enable != 1 ||
                 s.blend_src != 4 || s.blend_dst != 5 || s.blend_op != 0 ||
                 s.color_mask != 7 || s.alpha_compare != 0)) return false;
        }
        const bool direct = !std::memcmp(magic, v5, sizeof(magic)) || !std::memcmp(magic, v6, sizeof(magic)) ||
                            !std::memcmp(magic, v7, sizeof(magic)) || !std::memcmp(magic, v8, sizeof(magic));
        if (std::size_t(count) * (direct ? 32 : 20) > reader.remaining()) return false;
        for (std::uint32_t n = 0; n < count; ++n) {
            Vertex vertex{};
            if (!reader.f32(vertex.x) || !reader.f32(vertex.y) || !reader.f32(vertex.z) ||
                !reader.f32(vertex.u) || !reader.f32(vertex.v)) return false;
            if (direct) {
                if (!reader.f32(vertex.nx, draw.shading == 6 ? 1000000.0f : 1.0f) ||
                    !reader.f32(vertex.ny, 1.0f) || !reader.f32(vertex.nz, 1.0f) ||
                    (draw.shading == 6 && (vertex.ny != 0 || vertex.nz != 0))) return false;
                const float length_squared = vertex.nx*vertex.nx + vertex.ny*vertex.ny + vertex.nz*vertex.nz;
                if ((draw.shading == 2 || draw.shading == 4 || draw.shading == 5) &&
                    (length_squared < .98f || length_squared > 1.02f)) return false;
            }
            frame.vertices.push_back(vertex);
        }
        frame.draws.push_back(draw);
    }
    if (!reader.end()) return false;
    output = std::move(frame);
    return true;
}
} // namespace t3render::scene
