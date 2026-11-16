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

inline bool AttachAssetCandidate(SubmissionSample& sample,const AssetCandidate& asset) noexcept {
    if (!sample.valid || asset.status!=AssetStatus::Candidate || !asset.stable || !asset.name_available ||
        !asset.audit.available || !asset.audit.proxy || !asset.audit.resource) return false;
    sample.asset_proxy=asset.audit.proxy;sample.asset_resource=asset.audit.resource;
    std::memcpy(sample.asset_name,asset.name,sizeof(sample.asset_name));sample.asset_available=true;return true;
}

inline SubmissionSample SampleSubmission(const ProbeReader& r, std::uint32_t element, bool inspect_static_mesh = false) noexcept {
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
        if ((fname.value >> 16) == 1521 || inspect_static_mesh) {
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

// This bounded, opt-in scope sample is expensive: it performs membership and
// mesh-pool scans. Same-read checks are candidate evidence, not an ABA proof.
inline ProbeStaticMesh SampleVisibleStaticMesh(const ProbeReader& reader,const ProbeSnapshot& snapshot,
                                                const SubmissionSample& submission) noexcept {
    ProbeStaticMesh result;
    if (!submission.valid || submission.vtable!=0x10e4e8c0 || submission.type!=4 ||
        !snapshot.count.available || !snapshot.array.available || !snapshot.array.value || snapshot.entries_truncated ||
        snapshot.count.value!=snapshot.entries_attempted || snapshot.entries_attempted>kMaxProbeEntries) return result;
    auto stable_list=[&]() noexcept {
        std::uint32_t slots[kMaxProbeEntries]{};
        if (snapshot.entries_attempted && !ReadProbe(reader,snapshot.array.value,slots,snapshot.entries_attempted*4)) return false;
        for (std::uint32_t i=0;i<snapshot.entries_attempted;++i) {
            const auto& item=snapshot.entries[i];
            if (item.list_index!=i || !item.address.available || item.address.value!=slots[i] ||
                (item.address.value && (!item.vtable.available || !item.type.available))) return false;
        }
        const auto count=ReadProbeOffset(reader,snapshot.self,0x50),array=ReadProbeOffset(reader,snapshot.self,0x58);
        return count.available && count.value==snapshot.count.value && array.available && array.value==snapshot.array.value;
    };
    if (!stable_list()) return result;
    std::uint32_t selected=0,element_occurrences=0;
    for (std::uint32_t i=0;i<snapshot.entries_attempted;++i) {
        const auto& item=snapshot.entries[i];
        if (item.address.value==submission.element) {selected=i;++element_occurrences;}
    }
    auto unique_name=[&]() noexcept {
        std::uint32_t count=0;
        for (std::uint32_t i=0;i<snapshot.entries_attempted;++i) {
            const auto& item=snapshot.entries[i];
            if (!item.address.value) continue;
            const auto vtable=ReadProbeU32(reader,item.address.value),type=ReadProbeOffset(reader,item.address.value,0x44);
            if (!vtable.available || !type.available || vtable.value!=item.vtable.value || type.value!=item.type.value) return false;
            if (vtable.value!=0x10e4e8c0 || type.value!=4) continue;
            const auto owner=ReadProbeOffset(reader,item.address.value,0x54);
            if (!owner.available || !owner.value) return false;
            const auto fname=ReadProbeOffset(reader,owner.value,0x20);
            if (!fname.available) return false;
            if ((fname.value>>16)!=(submission.owner_fname>>16)) continue;
            char name[64]{};
            if (!ReadProbeName(reader,fname.value,name)) return false;
            if (std::strcmp(name,"StaticMeshActor")==0) ++count;
        }
        return count==1;
    };
    if (element_occurrences!=1 || !unique_name()) return result;
    ProbeSnapshot one;one.self=snapshot.self;one.count=snapshot.count;one.array=snapshot.array;
    one.entries_attempted=1;one.entries[0]=snapshot.entries[selected];
    ReadProbeOwners(reader,one);
    const auto entry=one.entries[0];
    if (!one.object_table_complete || !entry.owner_stable || !entry.owner_table_member ||
        !entry.owner_candidate.available || entry.owner_candidate.value!=submission.owner ||
        !entry.owner_fname.available || entry.owner_fname.value!=submission.owner_fname ||
        !entry.owner_name_available || std::strcmp(entry.owner_name,"StaticMeshActor")!=0) return result;
    ProbeArray32 table{};
    if (!ReadProbe(reader,0x10f3e4a0,&table,sizeof(table)) ||
        !ValidProbeArray(table,static_cast<std::uint32_t>(kMaxTargetObjects))) return result;
    const auto cls=ReadNamedProbeObject(reader,table,entry.owner_class);
    const auto outer_pointer=ReadProbeOffset(reader,entry.owner_candidate.value,0x18);
    const auto outer=ReadNamedProbeObject(reader,table,outer_pointer);
    if (!cls.member || !cls.stable || !cls.name_available || !cls.fname.available ||
        (cls.fname.value>>16)!=0 || std::strcmp(cls.name,"StaticMeshActor")!=0 ||
        !outer_pointer.available || !outer_pointer.value || !outer.member || !outer.stable ||
        !outer.name_available || !outer.fname.available || (outer.fname.value>>16)!=0 ||
        !outer.outer.available || outer.outer.value) return result;
    const AssetReader asset_reader{reader.context,reader.read};
    const auto asset=InspectAssetCandidate(asset_reader,entry.owner_candidate.value);
    ProbeMeshNode node;ReadProbeMeshNode(reader,submission.element,node);
    one.entries[0]=snapshot.entries[selected];one.object_table_complete=false;ReadProbeOwners(reader,one);
    const auto& later=one.entries[0];
    const auto outer_after=ReadProbeOffset(reader,entry.owner_candidate.value,0x18);
    const auto cls_after=ReadNamedProbeObject(reader,table,entry.owner_class);
    const auto map_after=ReadNamedProbeObject(reader,table,outer_pointer);
    if (!one.object_table_complete || !later.owner_stable || !later.owner_candidate.available ||
        later.owner_candidate.value!=entry.owner_candidate.value || !later.owner_fname.available ||
        later.owner_fname.value!=entry.owner_fname.value || !later.owner_class.available ||
        later.owner_class.value!=entry.owner_class.value || !outer_after.available || outer_after.value!=outer_pointer.value ||
        !cls_after.member || !cls_after.stable || !cls_after.name_available || !cls_after.fname.available ||
        cls_after.fname.value!=cls.fname.value || std::strcmp(cls_after.name,cls.name)!=0 ||
        !map_after.member || !map_after.stable || !map_after.name_available || !map_after.fname.available ||
        map_after.fname.value!=outer.fname.value || !map_after.outer.available || map_after.outer.value ||
        std::strcmp(map_after.name,outer.name)!=0 ||
        std::memcmp(later.owner_location_bits,entry.owner_location_bits,sizeof(entry.owner_location_bits)) ||
        std::memcmp(later.owner_rotation_bits,entry.owner_rotation_bits,sizeof(entry.owner_rotation_bits)) ||
        std::memcmp(later.object_matrix_bits,entry.object_matrix_bits,sizeof(entry.object_matrix_bits)) ||
        !stable_list() || !unique_name() || !stable_list()) return result;
    result.list_stable=true;result.object_table_complete=true;result.name_occurrences=1;
    result.entry=entry;result.owner_outer=outer_pointer;result.owner_class=cls;result.outer=outer;
    result.asset=asset;result.mesh_node=node;
    return result;
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
