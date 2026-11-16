#pragma once

// One semantic resource and two independently validated PBR frames.
// This is a standalone renderer prototype, not an integration into TDS.
#include "scene_pbr_packet.hpp"
#include <string>
#include <unordered_map>

namespace t3render::pbr {
constexpr std::size_t kMaxVariantKeyBytes = 512;
constexpr std::size_t kMaxVariantBytes = 2 * scene::kMaxPacketBytes + kMaxVariantKeyBytes + 20;

inline bool SameResidentContext(const Frame& a, const Frame& b) {
    if (a.width != b.width || a.height != b.height || a.min_depth != b.min_depth || a.max_depth != b.max_depth ||
        a.meters_per_unit != b.meters_per_unit ||
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

inline bool SameVariantContext(const Frame& a, const Frame& b) {
    return SameResidentContext(a, b) && !std::memcmp(a.view, b.view, sizeof(a.view)) &&
        !std::memcmp(a.projection, b.projection, sizeof(a.projection)) &&
        !std::memcmp(a.camera, b.camera, sizeof(a.camera));
}

inline std::uint64_t HashBytes(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}

inline std::uint64_t TextureHash(const Texture& texture) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto& mip : texture.mips) {
        hash = HashBytes(hash, &mip.width, sizeof(mip.width));
        hash = HashBytes(hash, &mip.height, sizeof(mip.height));
        hash = HashBytes(hash, mip.rgba.data(), mip.rgba.size());
    }
    return hash;
}

inline bool SameTexture(const Texture& a, const Texture& b) {
    if (a.mips.size() != b.mips.size()) return false;
    for (std::size_t i = 0; i < a.mips.size(); ++i)
        if (a.mips[i].width != b.mips[i].width || a.mips[i].height != b.mips[i].height ||
            a.mips[i].rgba != b.mips[i].rgba) return false;
    return true;
}

inline std::vector<std::uint64_t> TextureHashes(const Frame& frame) {
    std::vector<std::uint64_t> hashes;
    for (const auto& texture : frame.textures) hashes.push_back(TextureHash(texture));
    return hashes;
}

inline std::uint64_t DrawHash(const Frame& frame, const Draw& draw, const std::vector<std::uint64_t>& textures) {
    std::uint64_t hash = 14695981039346656037ull;
    hash = HashBytes(hash, &draw.vertex_count, sizeof(draw.vertex_count));
    hash = HashBytes(hash, &draw.shadow_mask, sizeof(draw.shadow_mask));
    hash = HashBytes(hash, draw.emission, sizeof(draw.emission));
    hash = HashBytes(hash, &draw.metallic, sizeof(draw.metallic));
    for (auto index : draw.textures) hash = HashBytes(hash, &textures[index], sizeof(textures[index]));
    return HashBytes(hash, frame.vertices.data()+draw.first_vertex, std::size_t(draw.vertex_count)*sizeof(Vertex));
}

inline bool SameDraw(const Frame& a, const Draw& x, const Frame& b, const Draw& y) {
    if (x.vertex_count != y.vertex_count || x.shadow_mask != y.shadow_mask ||
        std::memcmp(x.emission, y.emission, sizeof(x.emission)) || std::memcmp(&x.metallic, &y.metallic, sizeof(float)) ||
        std::memcmp(a.vertices.data()+x.first_vertex, b.vertices.data()+y.first_vertex,
                    std::size_t(x.vertex_count)*sizeof(Vertex))) return false;
    for (unsigned i = 0; i < 4; ++i) if (!SameTexture(a.textures[x.textures[i]], b.textures[y.textures[i]])) return false;
    return true;
}

inline std::uint64_t CasterHash(const Frame& frame, const ShadowCaster& caster) {
    std::uint64_t hash = 14695981039346656037ull;
    hash = HashBytes(hash, &caster.vertex_count, sizeof(caster.vertex_count));
    hash = HashBytes(hash, &caster.shadow_mask, sizeof(caster.shadow_mask));
    return HashBytes(hash, frame.vertices.data()+caster.first_vertex, std::size_t(caster.vertex_count)*sizeof(Vertex));
}

inline bool SameCaster(const Frame& a, const ShadowCaster& x, const Frame& b, const ShadowCaster& y) {
    return x.vertex_count == y.vertex_count && x.shadow_mask == y.shadow_mask &&
        !std::memcmp(a.vertices.data()+x.first_vertex, b.vertices.data()+y.first_vertex,
                     std::size_t(x.vertex_count)*sizeof(Vertex));
}

template<class Collection, class ResidentHash, class IncomingHash, class Equal>
inline bool MatchResident(const Collection& resident, const Collection& incoming, ResidentHash hash_resident,
                          IncomingHash hash_incoming, Equal equal, std::vector<std::uint32_t>& output) {
    if (incoming.size() > resident.size()) return false;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> index;
    for (std::size_t i = 0; i < resident.size(); ++i) {
        auto& bucket = index[hash_resident(resident[i])];
        if (bucket.size() >= 32) return false; // bound collision and ambiguity work
        bucket.push_back(static_cast<std::uint32_t>(i));
    }
    std::vector<bool> used(resident.size(), false);
    std::vector<std::uint32_t> matches;
    for (const auto& item : incoming) {
        const auto found = index.find(hash_incoming(item));
        if (found == index.end()) return false;
        std::uint32_t match = 0; unsigned count = 0;
        for (auto candidate : found->second) if (equal(resident[candidate], item)) { match = candidate; ++count; }
        if (count != 1 || used[match]) return false;
        used[match] = true; matches.push_back(match);
    }
    output = std::move(matches);
    return true;
}

class Variants {
    std::string resource_key_;
    Frame original_, replacement_;
    std::vector<std::uint8_t> original_packet_;
    std::vector<std::uint64_t> original_texture_hashes_, replacement_texture_hashes_;
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
        original_packet_ = std::move(raw_original);
        original_texture_hashes_ = TextureHashes(original_);
        replacement_texture_hashes_ = TextureHashes(replacement_);
        replacement_selected_ = false;
        return true;
    }

    bool Select(const std::string& resource_key, bool replacement) {
        if (resource_key_.empty() || resource_key != resource_key_) return false;
        replacement_selected_ = replacement;
        return true;
    }
    const Frame& Current() const { return replacement_selected_ ? replacement_ : original_; }
    bool ReplacementSelected() const { return replacement_selected_; }

    // A later packet may update only its camera. Check every other byte,
    // including the complete geometry, materials and lights, before mutation.
    // Runtime frame provenance is the caller's responsibility.
    bool UpdateCamera(const std::uint8_t* bytes, std::size_t size) {
        constexpr std::size_t camera_start = 24, camera_end = 164;
        if (!bytes || original_packet_.size() < camera_end || size != original_packet_.size() ||
            std::memcmp(bytes, original_packet_.data(), camera_start) ||
            std::memcmp(bytes + camera_end, original_packet_.data() + camera_end, size - camera_end)) return false;
        scene::Reader reader(bytes + camera_start, camera_end - camera_start);
        float view[16], projection[16], camera[3];
        for (float& value : view) if (!reader.f32(value, 10000)) return false;
        for (float& value : projection) if (!reader.f32(value, 10000)) return false;
        for (float& value : camera) if (!reader.f32(value)) return false;
        if (!reader.end()) return false;
        for (Frame* frame : {&original_, &replacement_}) {
            std::memcpy(frame->view, view, sizeof(view));
            std::memcpy(frame->projection, projection, sizeof(projection));
            std::memcpy(frame->camera, camera, sizeof(camera));
        }
        return true;
    }

    // Select only already-resident, uniquely matching draws and casters from
    // the next bundle. New or ambiguous resources fail without changing state.
    // Caller must separately establish semantic IDs and frame provenance.
    bool UpdateScene(const std::uint8_t* bytes, std::size_t size,
                     std::vector<std::uint32_t> (&draws)[2], std::vector<std::uint32_t> (&casters)[2]) {
        Variants incoming;
        if (!incoming.Parse(bytes, size) || incoming.resource_key_ != resource_key_ ||
            !SameResidentContext(original_, incoming.original_) ||
            !SameResidentContext(replacement_, incoming.replacement_)) return false;
        std::vector<std::uint32_t> next_draws[2], next_casters[2];
        const Frame* resident[] = {&original_, &replacement_};
        const Frame* next[] = {&incoming.original_, &incoming.replacement_};
        const std::vector<std::uint64_t>* resident_textures[] = {&original_texture_hashes_, &replacement_texture_hashes_};
        const std::vector<std::uint64_t>* next_textures[] = {&incoming.original_texture_hashes_, &incoming.replacement_texture_hashes_};
        for (unsigned v = 0; v < 2; ++v) {
            const Frame& a = *resident[v]; const Frame& b = *next[v];
            const auto& at = *resident_textures[v]; const auto& bt = *next_textures[v];
            if (!MatchResident(a.draws, b.draws,
                    [&](const Draw& draw) { return DrawHash(a, draw, at); },
                    [&](const Draw& draw) { return DrawHash(b, draw, bt); },
                    [&](const Draw& x, const Draw& y) { return SameDraw(a, x, b, y); }, next_draws[v]) ||
                !MatchResident(a.offline_casters, b.offline_casters,
                    [&](const ShadowCaster& caster) { return CasterHash(a, caster); },
                    [&](const ShadowCaster& caster) { return CasterHash(b, caster); },
                    [&](const ShadowCaster& x, const ShadowCaster& y) { return SameCaster(a, x, b, y); }, next_casters[v])) return false;
        }
        for (auto& index : next_draws[1]) index += static_cast<std::uint32_t>(original_.draws.size());
        for (auto& index : next_casters[1]) index += static_cast<std::uint32_t>(original_.offline_casters.size());
        for (Frame* frame : {&original_, &replacement_}) {
            std::memcpy(frame->view, incoming.original_.view, sizeof(frame->view));
            std::memcpy(frame->projection, incoming.original_.projection, sizeof(frame->projection));
            std::memcpy(frame->camera, incoming.original_.camera, sizeof(frame->camera));
        }
        for (unsigned v = 0; v < 2; ++v) { draws[v] = std::move(next_draws[v]); casters[v] = std::move(next_casters[v]); }
        return true;
    }

    // Build one GPU input with shared textures and variant-tagged submissions.
    // Each filtered draw sequence remains in its original order.
    bool BuildCombined(Frame& output, std::vector<std::uint8_t>& draw_variants,
                       std::vector<std::uint8_t>& caster_variants) const {
        if (resource_key_.empty() || !SameVariantContext(original_, replacement_)) return false;
        Frame frame = original_;
        std::vector<std::uint8_t> draws(original_.draws.size(), 1), casters(original_.offline_casters.size(), 1);
        std::vector<std::uint32_t> texture_map;
        const auto same_texture = [](const Texture& a, const Texture& b) {
            if (a.mips.size() != b.mips.size()) return false;
            for (std::size_t i = 0; i < a.mips.size(); ++i)
                if (a.mips[i].width != b.mips[i].width || a.mips[i].height != b.mips[i].height ||
                    a.mips[i].rgba != b.mips[i].rgba) return false;
            return true;
        };
        for (const auto& texture : replacement_.textures) {
            std::size_t index = 0;
            for (; index < frame.textures.size(); ++index) if (same_texture(frame.textures[index], texture)) break;
            if (index == frame.textures.size()) {
                if (frame.textures.size() >= scene::kMaxTextures) return false;
                frame.textures.push_back(texture);
            }
            texture_map.push_back(static_cast<std::uint32_t>(index));
        }
        const auto base = frame.vertices.size();
        const auto total_vertices = std::uint64_t{base} + replacement_.vertices.size();
        if (total_vertices > UINT32_MAX || total_vertices * sizeof(scene::Vertex) > UINT32_MAX) return false;
        frame.vertices.insert(frame.vertices.end(), replacement_.vertices.begin(), replacement_.vertices.end());
        for (auto draw : replacement_.draws) {
            draw.first_vertex += static_cast<std::uint32_t>(base);
            for (auto& texture : draw.textures) texture = texture_map[texture];
            frame.draws.push_back(draw); draws.push_back(2);
        }
        for (auto caster : replacement_.offline_casters) {
            caster.first_vertex += static_cast<std::uint32_t>(base);
            frame.offline_casters.push_back(caster); casters.push_back(2);
        }
        output = std::move(frame); draw_variants = std::move(draws); caster_variants = std::move(casters);
        return true;
    }
};
} // namespace t3render::pbr
