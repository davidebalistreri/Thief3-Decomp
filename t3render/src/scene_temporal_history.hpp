#pragma once

// Bounded GPU history input; frame ordering still requires SceneIR provenance.
#include "scene_pbr_packet.hpp"
#include <algorithm>
#include <tuple>

namespace t3render::temporal {
struct History {
    std::uint32_t width = 0, height = 0, flags = 0;
    float view[16]{}, projection[16]{}, camera[3]{}, scale = 0, exposure = 0, jitter[2]{};
    std::vector<std::uint8_t> color, position;
};

inline bool SameLightingAndTextures(const pbr::Frame& a, const pbr::Frame& b) {
    if (a.width != b.width || a.height != b.height || a.min_depth != b.min_depth || a.max_depth != b.max_depth ||
        a.meters_per_unit != b.meters_per_unit ||
        a.shadow_size != b.shadow_size || a.shadow_near_m != b.shadow_near_m || a.shadow_far_m != b.shadow_far_m ||
        a.shadow_bias_m != b.shadow_bias_m || a.shadow_softness != b.shadow_softness || a.has_emission != b.has_emission ||
        a.has_metallic != b.has_metallic ||
        std::memcmp(a.ambient, b.ambient, sizeof(a.ambient)) || a.lights.size() != b.lights.size() ||
        a.textures.size() != b.textures.size()) return false;
    for (std::size_t i = 0; i < a.lights.size(); ++i) {
        const auto& x = a.lights[i]; const auto& y = b.lights[i];
        if (std::memcmp(x.position, y.position, sizeof(x.position)) || x.radius_m != y.radius_m ||
            std::memcmp(x.color, y.color, sizeof(x.color)) || x.intensity != y.intensity) return false;
    }
    for (std::size_t i = 0; i < a.textures.size(); ++i) {
        const auto& x = a.textures[i].mips; const auto& y = b.textures[i].mips;
        if (x.size() != y.size()) return false;
        for (std::size_t j = 0; j < x.size(); ++j)
            if (x[j].width != y[j].width || x[j].height != y[j].height || x[j].rgba != y[j].rgba) return false;
    }
    if (a.has_metallic) {
        using MaterialSignature = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                                             float, float, float, float>;
        std::vector<MaterialSignature> left, right;
        const auto collect = [](const pbr::Frame& frame, std::vector<MaterialSignature>& values) {
            for (const auto& draw : frame.draws)
                values.emplace_back(draw.textures[0], draw.textures[1], draw.textures[2], draw.textures[3],
                                    draw.metallic, draw.emission[0], draw.emission[1], draw.emission[2]);
        };
        collect(a, left); collect(b, right);
        std::sort(left.begin(), left.end()); left.erase(std::unique(left.begin(), left.end()), left.end());
        std::sort(right.begin(), right.end()); right.erase(std::unique(right.begin(), right.end()), right.end());
        if (left != right) return false;
    }
    return true;
}

inline bool ParseHistory(const std::uint8_t* bytes, std::size_t size, const pbr::Frame& previous,
                         const pbr::Frame& current, const std::uint8_t* expected_digest,
                         std::uint32_t expected_flags, History& output) {
    constexpr std::size_t header_size = 208;
    constexpr std::size_t max_size = header_size + std::size_t(2048)*2048*16;
    if (!bytes || !expected_digest || size < header_size || size > max_size || !SameLightingAndTextures(previous, current)) return false;
    scene::Reader reader(bytes, size);
    History history; char magic[8]; std::uint8_t digest[32];
    if (!reader.bytes(magic, 8) || std::memcmp(magic, "T3TEMP01", 8) ||
        !reader.u32(history.width) || !reader.u32(history.height) || history.width != previous.width || history.height != previous.height ||
        !reader.bytes(digest, sizeof(digest)) || std::memcmp(digest, expected_digest, sizeof(digest))) return false;
    for (float& x : history.view) if (!reader.f32(x, 10000)) return false;
    for (float& x : history.projection) if (!reader.f32(x, 10000)) return false;
    for (float& x : history.camera) if (!reader.f32(x)) return false;
    if (!reader.f32(history.scale, 1) || history.scale < .00001f || !reader.f32(history.exposure, 32) || history.exposure < .01f ||
        !reader.f32(history.jitter[0], .5f) || !reader.f32(history.jitter[1], .5f) ||
        !reader.u32(history.flags) || history.flags != expected_flags || history.flags > 31 ||
        std::memcmp(history.view, previous.view, sizeof(history.view)) ||
        std::memcmp(history.projection, previous.projection, sizeof(history.projection)) ||
        std::memcmp(history.camera, previous.camera, sizeof(history.camera)) || history.scale != previous.meters_per_unit ||
        history.exposure != previous.exposure) return false;
    const std::size_t image_bytes = std::size_t(history.width)*history.height*8;
    if (reader.remaining() != image_bytes*2) return false;
    history.color.resize(image_bytes); history.position.resize(image_bytes);
    if (!reader.bytes(history.color.data(), image_bytes) || !reader.bytes(history.position.data(), image_bytes) || !reader.end()) return false;
    for (const auto* image : {&history.color, &history.position}) {
        const bool positions = image == &history.position;
        for (std::size_t i = 0; i < image_bytes; i += 8) for (unsigned channel = 0; channel < 4; ++channel) {
            const std::uint16_t value = std::uint16_t((*image)[i+2*channel]) | (std::uint16_t((*image)[i+2*channel+1]) << 8);
            if ((value & 0x7c00u) == 0x7c00u ||
                (channel == 3 && value != 0 && value != 0x3c00u) ||
                (channel < 3 && !positions && (value & 0x8000u) && (value & 0x7fffu)) ||
                (channel < 3 && positions && (value & 0x7fffu) > 0x7b53u)) return false;
        }
    }
    output = std::move(history);
    return true;
}
} // namespace t3render::temporal
