#pragma once

#include "scene_bridge_snapshot.hpp"

namespace t3render::scene_bridge {

// Short-lived evidence at the actual per-section virtual draw call. It is not
// a durable identity: repeated reads cannot exclude ABA or unseen writes.
struct SubmissionSample {
    std::uint32_t self = 0, pass = 0, element = 0, vtable = 0, type = 0;
    std::uint32_t matrix[16]{};
    std::uint32_t owner = 0, owner_index = 0, owner_fname = 0, backlink = 0, mesh_node = 0;
    std::uint32_t asset_proxy = 0, asset_resource = 0;
    char asset_name[128]{};
    bool asset_available = false;
    std::uint32_t bsp_source = 0, bsp_blocks = 0, bsp_index = 0;
    bool valid = false;
};

inline SubmissionSample SampleSubmission(const ProbeReader& r, std::uint32_t element) noexcept {
    SubmissionSample s{};
    s.element = element;
    const auto self = ReadProbeU32(r, 0x10f2c734);
    const auto pass = ReadProbeU32(r, 0x10efef54);
    const auto vtable = ReadProbeU32(r, element);
    const auto type = ReadProbeOffset(r, element, 0x44);
    if (!element || !self.available || !self.value || !pass.available || !vtable.available || !type.available ||
        !ReadProbeBytesOffset(r, element, 0x04, s.matrix, sizeof(s.matrix))) return s;
    s.self = self.value; s.pass = pass.value; s.vtable = vtable.value; s.type = type.value;
    for (auto bits : s.matrix) if ((bits & 0x7f800000u) == 0x7f800000u) return s;
    if (s.vtable == 0x10e4e8c0 && s.type == 4) {
        const auto owner = ReadProbeOffset(r, element, 0x54);
        const auto node = ReadProbeOffset(r, element, 0x108);
        if (!owner.available || !owner.value || !node.available || !node.value) return s;
        const auto index = ReadProbeOffset(r, owner.value, 4);
        const auto fname = ReadProbeOffset(r, owner.value, 0x20);
        const auto backlink = ReadProbeOffset(r, owner.value, 0xb4);
        ProbeArray32 table{};
        if (!index.available || !fname.available || !backlink.available || backlink.value != element ||
            !ReadProbe(r, 0x10f3e4a0, &table, sizeof(table)) ||
            !ValidProbeArray(table, static_cast<std::uint32_t>(kMaxTargetObjects)) || index.value >= table.count)
            return s;
        const auto slot = ReadProbeOffset(r, table.data, index.value * 4);
        if (!slot.available || slot.value != owner.value) return s;
        s.owner = owner.value; s.owner_index = index.value; s.owner_fname = fname.value;
        s.backlink = backlink.value; s.mesh_node = node.value;
        // Only the requested actor needs the expensive bounded property audit.
        // Keep its resource object in the local trace, never in SceneIR.
        if ((fname.value >> 16) == 1521) {
            char actor_name[64]{};
            if (ReadProbeName(r, fname.value, actor_name) && std::strcmp(actor_name, "StaticMeshActor") == 0) {
                const AssetReader asset_reader{r.context, r.read};
                const auto asset = InspectAssetCandidate(asset_reader, owner.value);
                if (asset.status == AssetStatus::Candidate && asset.stable && asset.audit.available) {
                    s.asset_proxy = asset.audit.proxy;
                    s.asset_resource = asset.audit.resource;
                    std::memcpy(s.asset_name, asset.name, sizeof(s.asset_name));
                    s.asset_available = true;
                }
            }
        }
    } else if (s.vtable == 0x10e4a668 && s.type == 5) {
        const auto renderer = ReadProbeU32(r, 0x10f2c740);
        if (!renderer.available || !renderer.value) return s;
        const auto source = ReadProbeOffset(r, renderer.value, 0x0c);
        if (!source.available || !source.value) return s;
        const auto magic = ReadProbeU32(r, source.value);
        const auto count = ReadProbeOffset(r, source.value, 0x0c);
        const auto blocks = ReadProbeOffset(r, source.value, 0x54);
        constexpr std::uint32_t stride = 0x1ac;
        if (!magic.available || magic.value != 0x50534256 || !count.available || !count.value ||
            count.value > 65536 || !blocks.available || blocks.value < 4 || element < blocks.value ||
            std::uint64_t{blocks.value} + std::uint64_t{count.value} * stride > (std::uint64_t{1} << 32)) return s;
        const auto allocated = ReadProbeU32(r, blocks.value - 4);
        const auto distance = element - blocks.value;
        if (!allocated.available || allocated.value != count.value || distance % stride || distance / stride >= count.value)
            return s;
        s.bsp_source = source.value; s.bsp_blocks = blocks.value; s.bsp_index = distance / stride;
    } else return s;
    s.valid = true;
    return s;
}

inline bool SameSubmission(const SubmissionSample& a, const SubmissionSample& b) noexcept {
    return a.valid && b.valid && a.self == b.self && a.pass == b.pass && a.element == b.element &&
        a.vtable == b.vtable && a.type == b.type && std::memcmp(a.matrix, b.matrix, sizeof(a.matrix)) == 0 &&
        a.owner == b.owner && a.owner_index == b.owner_index && a.owner_fname == b.owner_fname &&
        a.backlink == b.backlink && a.mesh_node == b.mesh_node && a.bsp_source == b.bsp_source &&
        a.bsp_blocks == b.bsp_blocks && a.bsp_index == b.bsp_index && a.asset_available == b.asset_available &&
        a.asset_proxy == b.asset_proxy && a.asset_resource == b.asset_resource &&
        std::memcmp(a.asset_name, b.asset_name, sizeof(a.asset_name)) == 0;
}

} // namespace t3render::scene_bridge
