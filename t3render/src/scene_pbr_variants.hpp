#pragma once

// One semantic resource and two independently validated PBR frames.
// This is a standalone renderer prototype, not an integration into TDS.
#include "scene_pbr_packet.hpp"
#include <string>

namespace t3render::pbr {
constexpr std::size_t kMaxVariantKeyBytes = 512;
constexpr std::size_t kMaxVariantBytes = 2 * scene::kMaxPacketBytes + kMaxVariantKeyBytes + 20;

inline bool SameVariantContext(const Frame& a, const Frame& b) {
    if (a.width != b.width || a.height != b.height || a.min_depth != b.min_depth || a.max_depth != b.max_depth ||
        std::memcmp(a.view, b.view, sizeof(a.view)) || std::memcmp(a.projection, b.projection, sizeof(a.projection)) ||
        std::memcmp(a.camera, b.camera, sizeof(a.camera)) || a.meters_per_unit != b.meters_per_unit ||
        std::memcmp(a.ambient, b.ambient, sizeof(a.ambient)) || a.exposure != b.exposure ||
        a.shadow_size != b.shadow_size || a.shadow_near_m != b.shadow_near_m || a.shadow_far_m != b.shadow_far_m ||
        a.shadow_bias_m != b.shadow_bias_m || a.shadow_softness != b.shadow_softness ||
        a.has_emission != b.has_emission || a.has_metallic != b.has_metallic || a.lights.size() != b.lights.size()) return false;
    for (std::size_t i = 0; i < a.lights.size(); ++i) {
        const auto& x = a.lights[i]; const auto& y = b.lights[i];
        if (std::memcmp(x.position, y.position, sizeof(x.position)) || x.radius_m != y.radius_m ||
            std::memcmp(x.color, y.color, sizeof(x.color)) || x.intensity != y.intensity) return false;
    }
    return true;
}

class Variants {
    std::string resource_key_;
    Frame original_, replacement_;
    bool replacement_selected_ = false;
public:
    bool Parse(const std::uint8_t* bytes, std::size_t size) {
        if (!bytes || size > kMaxVariantBytes) return false;
        scene::Reader reader(bytes, size);
        char magic[8]; std::uint32_t key_size = 0, original_size = 0, replacement_size = 0;
        if (!reader.bytes(magic, sizeof(magic)) || std::memcmp(magic, "T3VAR01\0", 8) ||
            !reader.u32(key_size) || !reader.u32(original_size) || !reader.u32(replacement_size) ||
            !key_size || key_size > kMaxVariantKeyBytes || !original_size || !replacement_size ||
            original_size > scene::kMaxPacketBytes || replacement_size > scene::kMaxPacketBytes ||
            std::uint64_t{key_size} + original_size + replacement_size != reader.remaining()) return false;
        std::string key(key_size, '\0');
        if (!reader.bytes(key.data(), key.size())) return false;
        for (unsigned char c : key) if (c < 33 || c > 126) return false;
        if (key.compare(0, 5, "mesh:") && key.compare(0, 6, "asset:")) return false;
        std::vector<std::uint8_t> raw_original(original_size), raw_replacement(replacement_size);
        Frame original, replacement;
        if (!reader.bytes(raw_original.data(), raw_original.size()) ||
            !reader.bytes(raw_replacement.data(), raw_replacement.size()) || !reader.end() ||
            !ParsePacket(raw_original.data(), raw_original.size(), original) ||
            !ParsePacket(raw_replacement.data(), raw_replacement.size(), replacement) ||
            !SameVariantContext(original, replacement)) return false;
        resource_key_ = std::move(key);
        original_ = std::move(original); replacement_ = std::move(replacement);
        replacement_selected_ = false;
        return true;
    }

    bool Select(const std::string& resource_key, bool replacement) {
        if (resource_key_.empty() || resource_key != resource_key_) return false;
        replacement_selected_ = replacement;
        return true;
    }
    const Frame& Current() const { return replacement_selected_ ? replacement_ : original_; }
};
} // namespace t3render::pbr
