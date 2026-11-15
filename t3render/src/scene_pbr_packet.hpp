#pragma once

// API-neutral, bounded input for the independent modern SceneIR renderer.
#include "scene_packet.hpp"

namespace t3render::pbr {
constexpr std::uint32_t kMaxLights = 16;
using scene::Mip;
using scene::Texture;
using scene::Vertex;

struct Light {
    float position[3]{}, radius_m = 0;
    float color[3]{}, intensity = 0;
};
struct Draw {
    std::uint32_t textures[4]{}, first_vertex = 0, vertex_count = 0;
};
struct Frame {
    std::uint32_t width = 0, height = 0;
    float min_depth = 0, max_depth = 1, view[16]{}, projection[16]{};
    float camera[3]{}, meters_per_unit = 0, ambient[3]{}, exposure = 0;
    std::vector<Light> lights;
    std::vector<Texture> textures;
    std::vector<Vertex> vertices;
    std::vector<Draw> draws;
};

inline bool ParsePacket(const std::uint8_t* bytes, std::size_t size, Frame& output) {
    if (!bytes || size > scene::kMaxPacketBytes) return false;
    scene::Reader reader(bytes, size);
    Frame frame;
    char magic[8];
    if (!reader.bytes(magic, sizeof(magic)) || std::memcmp(magic, "T3PBR01\0", 8) ||
        !reader.u32(frame.width) || !reader.u32(frame.height) || !frame.width || !frame.height ||
        frame.width > 2048 || frame.height > 2048 || !reader.f32(frame.min_depth) || !reader.f32(frame.max_depth) ||
        frame.min_depth < 0 || frame.max_depth > 1 || frame.min_depth > frame.max_depth) return false;
    for (float& x : frame.view) if (!reader.f32(x, 10000)) return false;
    for (float& x : frame.projection) if (!reader.f32(x, 10000)) return false;
    for (float& x : frame.camera) if (!reader.f32(x)) return false;
    if (!reader.f32(frame.meters_per_unit, 1) || frame.meters_per_unit < .00001f) return false;
    for (float& x : frame.ambient) if (!reader.f32(x, 4) || x < 0) return false;
    if (!reader.f32(frame.exposure, 32) || frame.exposure < .01f) return false;
    std::uint32_t texture_count, draw_count, light_count;
    if (!reader.u32(texture_count) || !reader.u32(draw_count) || !reader.u32(light_count) ||
        !texture_count || texture_count > scene::kMaxTextures || !draw_count || draw_count > scene::kMaxDraws ||
        !light_count || light_count > kMaxLights) return false;
    frame.lights.reserve(light_count);
    for (std::uint32_t i = 0; i < light_count; ++i) {
        Light light;
        for (float& x : light.position) if (!reader.f32(x)) return false;
        if (!reader.f32(light.radius_m, 20) || light.radius_m < .01f) return false;
        for (float& x : light.color) if (!reader.f32(x, 1) || x < 0) return false;
        if (!reader.f32(light.intensity, 10000) || light.intensity < 0) return false;
        frame.lights.push_back(light);
    }
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
        Draw draw;
        for (auto& index : draw.textures) if (!reader.u32(index) || index >= texture_count) return false;
        if (!reader.u32(draw.vertex_count) || !draw.vertex_count || draw.vertex_count % 3 ||
            draw.vertex_count > scene::kMaxTriangles * 3 - frame.vertices.size() ||
            std::size_t(draw.vertex_count) * 48 > reader.remaining()) return false;
        draw.first_vertex = static_cast<std::uint32_t>(frame.vertices.size());
        for (std::uint32_t j = 0; j < draw.vertex_count; ++j) {
            Vertex vertex{};
            if (!reader.f32(vertex.x) || !reader.f32(vertex.y) || !reader.f32(vertex.z) ||
                !reader.f32(vertex.u) || !reader.f32(vertex.v) ||
                !reader.f32(vertex.nx, 1) || !reader.f32(vertex.ny, 1) || !reader.f32(vertex.nz, 1) ||
                !reader.f32(vertex.tx, 1) || !reader.f32(vertex.ty, 1) || !reader.f32(vertex.tz, 1) ||
                !reader.f32(vertex.tangent_sign, 1)) return false;
            const float normal_length = vertex.nx*vertex.nx + vertex.ny*vertex.ny + vertex.nz*vertex.nz;
            const float tangent_length = vertex.tx*vertex.tx + vertex.ty*vertex.ty + vertex.tz*vertex.tz;
            const float dot = vertex.nx*vertex.tx + vertex.ny*vertex.ty + vertex.nz*vertex.tz;
            if (normal_length < .98f || normal_length > 1.02f ||
                (vertex.tangent_sign == 0 ? tangent_length != 0 :
                 ((vertex.tangent_sign != -1 && vertex.tangent_sign != 1) || tangent_length < .98f ||
                  tangent_length > 1.02f || std::fabs(dot) > .02f))) return false;
            frame.vertices.push_back(vertex);
        }
        frame.draws.push_back(draw);
    }
    if (!reader.end()) return false;
    output = std::move(frame);
    return true;
}
} // namespace t3render::pbr
