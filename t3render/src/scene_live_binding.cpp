#include "scene_live_binding.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace t3render::live {
static_assert(sizeof(float)==sizeof(std::uint32_t), "probe matrices require 32-bit floats");
namespace {
using namespace t3render::scene_bridge;
constexpr std::uint32_t kCaller = 0x109271ae, kMeshVtable = 0x10e4e8c0, kBspVtable = 0x10e4a668;

bool EqualAscii(const std::string& a, const char* b, std::size_t capacity) {
    const auto* end = static_cast<const char*>(std::memchr(b, '\0', capacity));
    if (!end || a.size() != static_cast<std::size_t>(end-b)) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; };
        if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}
bool Digest(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Value(const ProbeValue& value, std::uint32_t expected) {
    return value.available && value.value == expected;
}
bool ReadFloats(const std::uint32_t* bits, float* output, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        std::memcpy(&output[i], &bits[i], sizeof(float));
        if (!std::isfinite(output[i])) return false;
    }
    return true;
}
bool MapMatches(const BindingCatalog& catalog, const ProbeSnapshot& snapshot) {
    const auto& target = snapshot.target;
    if (catalog.map.empty() || catalog.map.size() > 128 || !Digest(catalog.map_sha256) || !target.entry_available ||
        (!target.visible_list_only && (!target.object_scan_complete || target.object_scan_truncated || target.actors_named_target != 1)) ||
        !target.list_scan_complete || target.list_scan_truncated || target.list_occurrences != 1 ||
        target.outer_count != 1 || target.outer_truncated) return false;
    const auto& entry=target.entry;
    const auto& cls=target.owner_class;
    if (!snapshot.object_table_complete || !entry.owner_table_member || !entry.owner_stable ||
        !entry.owner_name_available || !EqualAscii("StaticMeshActor",entry.owner_name,sizeof(entry.owner_name)) ||
        !entry.owner_fname.available || (entry.owner_fname.value>>16) != 1521 || !entry.owner_class.available ||
        !cls.member || !cls.stable || !cls.name_available || !cls.fname.available || (cls.fname.value>>16) != 0 ||
        !Value(cls.address,entry.owner_class.value) || !EqualAscii("StaticMeshActor",cls.name,sizeof(cls.name))) return false;
    const auto& outer = target.outer[0];
    if (!outer.member || !outer.stable || !outer.name_available) return false;
    const auto* end = static_cast<const char*>(std::memchr(outer.name, '\0', sizeof(outer.name)));
    if (!end) return false;
    const std::string name(outer.name, end);
    constexpr char prefix[] = "LOADGAME:";
    if (name.size() >= sizeof(prefix)-1 && EqualAscii(name.substr(0,sizeof(prefix)-1),prefix,sizeof(prefix)))
        return EqualAscii(catalog.map,name.c_str()+sizeof(prefix)-1,name.size()-(sizeof(prefix)-1)+1);
    return EqualAscii(catalog.map,name.c_str(),name.size()+1);
}
bool General(const BindingCatalog& catalog, const ProbeSnapshot& snapshot,
             const SubmissionSample& before) {
    ProbeCamera ignored;
    return snapshot.thread_id && ReadProbeCamera(snapshot, snapshot.thread_id, before.pass, ignored) &&
           MapMatches(catalog, snapshot) && before.valid && before.self == snapshot.self &&
           Value(snapshot.pass, before.pass);
}
bool BoundsMatch(const Geometry& geometry, const std::uint32_t* raw) {
    if (geometry.positions.empty()) return false;
    float bounds[6];
    if (!ReadFloats(raw,bounds,6)) return false;
    for (unsigned axis=0; axis<3; ++axis) {
        float min=std::numeric_limits<float>::infinity(), max=-min;
        for (const auto& position : geometry.positions) {
            if (!std::isfinite(position[axis])) return false;
            min=std::min(min,position[axis]);max=std::max(max,position[axis]);
        }
        if (bounds[axis] != min || bounds[axis+3] != max) return false;
    }
    return true;
}
bool MaterialMatches(const BindingCatalog& catalog, const std::optional<std::string>& id,
                     const char* name, std::size_t size, bool available) {
    if (!id || !available) return false;
    const auto material=catalog.materials.find(*id);
    return material != catalog.materials.end() && material->second.id == *id && EqualAscii(material->second.name,name,size);
}
template<class RuntimeSection>
bool SectionMatches(const Geometry& geometry, const Section& section, const RuntimeSection& observed) {
    if (section.first_index != observed.first_index || section.index_count != observed.index_count ||
        section.index_count % 3 || section.index_count/3 != observed.triangles || !observed.vertex_count ||
        std::uint64_t{section.first_index}+section.index_count > geometry.indices.size() ||
        std::uint64_t{observed.first_vertex}+observed.vertex_count > geometry.positions.size()) return false;
    for (std::uint32_t i=0;i<section.index_count;++i) {
        const auto value=geometry.indices[section.first_index+i];
        if (value < observed.first_vertex || std::uint64_t{value} >= std::uint64_t{observed.first_vertex}+observed.vertex_count)
            return false;
    }
    return true;
}
bool GeometryComplete(const BindingCatalog& catalog, const Resource& resource,
                      const std::vector<std::optional<std::string>>* overrides=nullptr) {
    const auto& geometry=resource.geometry;
    if (geometry.positions.empty() || geometry.indices.empty() || geometry.sections.empty() ||
        geometry.sections.size()>kMaxProbeBspSections || geometry.positions.size()>65536 || geometry.indices.size()>65536 ||
        (overrides && !overrides->empty() && overrides->size()!=geometry.sections.size())) return false;
    for (const auto& position : geometry.positions) for (float value : position) if (!std::isfinite(value)) return false;
    for (auto index : geometry.indices) if (index>=geometry.positions.size()) return false;
    for (std::size_t i=0;i<geometry.sections.size();++i) {
        const auto& section=geometry.sections[i];
        const auto& id=(overrides && !overrides->empty()) ? (*overrides)[i] : section.material_id;
        if (section.name.empty() || !section.index_count || section.index_count%3 ||
            std::uint64_t{section.first_index}+section.index_count>geometry.indices.size() || !id) return false;
        const auto material=catalog.materials.find(*id);
        if (material==catalog.materials.end() || material->second.id!=*id || material->second.name.empty()) return false;
    }
    return true;
}
bool ObservedBspComplete(const ProbeBspBlock& block) {
    if (!block.stable || !block.metadata_available || !block.sections_available || block.sections_truncated ||
        !block.section_count || block.sections_copied!=block.section_count || block.sections_copied>kMaxProbeBspSections) return false;
    float bounds[6], transform[16];
    if (!ReadFloats(block.bounds_bits,bounds,6) || !ReadFloats(block.transform_bits,transform,16)) return false;
    for (unsigned i=0;i<3;++i) if (bounds[i]>bounds[i+3]) return false;
    for (std::uint32_t i=0;i<block.sections_copied;++i) {
        const auto& section=block.sections[i];
        if (!section.material_name_available || !section.material_name[0] || !std::memchr(section.material_name,'\0',sizeof(section.material_name)) ||
            !section.index_count || section.index_count%3 || section.triangles!=section.index_count/3 ||
            !section.vertex_count || std::uint64_t{section.first_index}+section.index_count>block.index_count ||
            std::uint64_t{section.first_vertex}+section.vertex_count>block.vertex_count) return false;
    }
    return true;
}
bool MeshMetadata(const BindingCatalog& catalog, const ProbeMeshNode& node) {
    if (!catalog.bed || catalog.bed->kind != ResourceKind::StaticMesh || !node.stable || !node.pool_scan_complete ||
        node.pool_scan_truncated || node.occurrences != 1 || !node.name_available ||
        !EqualAscii(catalog.bed->name,node.name,sizeof(node.name)) || !node.counts_available || !node.bounds_available ||
        !node.sections_available || node.sections_truncated) return false;
    const auto& geometry=catalog.bed->geometry;
    if (node.vertex_count != geometry.positions.size() || node.index_count != geometry.indices.size() ||
        node.triangle_count != geometry.indices.size()/3 || node.section_count != geometry.sections.size() ||
        node.sections_copied != geometry.sections.size() || node.sections_copied > kMaxMeshNodeSections ||
        (!catalog.bed_materials.empty() && catalog.bed_materials.size() != geometry.sections.size()) ||
        !BoundsMatch(geometry,node.bounds_bits)) return false;
    for (std::size_t i=0;i<geometry.sections.size();++i) {
        const auto& section=geometry.sections[i]; const auto& observed=node.sections[i];
        const auto& mat=catalog.bed_materials.empty() ? section.material_id : catalog.bed_materials[i];
        if (!observed.name_available || !EqualAscii(section.name,observed.name,sizeof(observed.name)) ||
            !MaterialMatches(catalog,mat,observed.material_name,sizeof(observed.material_name),observed.material_name_available) ||
            !SectionMatches(geometry,section,observed)) return false;
    }
    return true;
}
bool BspMetadata(const BindingCatalog& catalog, const Resource& resource, const ProbeBspBlock& block) {
    const auto& geometry=resource.geometry;
    if (resource.kind != ResourceKind::BspBlock || !block.stable || !block.metadata_available ||
        !block.sections_available || block.sections_truncated || block.vertex_count != geometry.positions.size() ||
        block.index_count != geometry.indices.size() || block.triangle_count != geometry.indices.size()/3 ||
        block.section_count != geometry.sections.size() || block.sections_copied != geometry.sections.size() ||
        block.sections_copied > kMaxProbeBspSections || !BoundsMatch(geometry,block.bounds_bits)) return false;
    for (std::size_t i=0;i<geometry.sections.size();++i) {
        const auto& section=geometry.sections[i]; const auto& observed=block.sections[i];
        if (!MaterialMatches(catalog,section.material_id,observed.material_name,sizeof(observed.material_name),observed.material_name_available) ||
            !SectionMatches(geometry,section,observed)) return false;
    }
    return true;
}
bool IdentityTransform(const std::uint32_t* bits) {
    float values[16]; if (!ReadFloats(bits,values,16)) return false;
    for (unsigned i=0;i<16;++i) if (values[i] != (i%5 == 0 ? 1.0f : 0.0f)) return false;
    return true;
}
} // namespace

bool ReadProbeCamera(const scene_bridge::ProbeSnapshot& snapshot, std::uint32_t thread_id,
                     std::uint32_t render_pass, ProbeCamera& camera) {
    const auto& sampled=snapshot.current_camera;
    if (!thread_id || snapshot.thread_id != thread_id || !snapshot.self || snapshot.caller != kCaller ||
        !snapshot.known_caller || !snapshot.pass_stable || !Value(snapshot.pass,render_pass) || !sampled.stable ||
        !sampled.address.available || !sampled.address.value || !sampled.derived_matrix_available || !sampled.projection_available) return false;
    ProbeCamera result;
    if (!ReadFloats(sampled.derived_matrix_bits,result.view.data(),16) ||
        !ReadFloats(sampled.projection_bits,result.projection.data(),16)) return false;
    camera=result; return true;
}

bool ResolveBed(const BindingCatalog& catalog, const scene_bridge::ProbeSnapshot& snapshot,
                const scene_bridge::SubmissionSample& before, std::uint32_t section,
                std::uint64_t generation, MeshSelection& output) {
    const std::string actor="actor:"+catalog.map+":"+catalog.map_sha256+":StaticMeshActor__1520";
    if (!generation || catalog.bed_instance_id != actor || !General(catalog,snapshot,before) ||
        before.type != 4 || before.vtable != kMeshVtable || !snapshot.object_table_complete) return false;
    const auto& target=snapshot.target; const auto& entry=target.entry;
    const auto& cls=target.owner_class; const auto& node=target.mesh_node; const auto& asset=target.asset;
    if (!entry.owner_table_member || !entry.owner_stable || !entry.owner_name_available ||
        !EqualAscii("StaticMeshActor",entry.owner_name,sizeof(entry.owner_name)) ||
        !entry.owner_fname.available || (entry.owner_fname.value>>16) != 1521 ||
        !entry.owner_object_index.available || !entry.object_matrix_available ||
        !Value(entry.address,before.element) || !Value(entry.vtable,before.vtable) || !Value(entry.type,before.type) ||
        !Value(entry.owner_candidate,before.owner) || !Value(entry.owner_object_index,before.owner_index) ||
        !Value(entry.owner_fname,before.owner_fname) || !Value(entry.owner_backlink,before.backlink) ||
        before.backlink != before.element || std::memcmp(entry.object_matrix_bits,before.matrix,sizeof(before.matrix)) ||
        !cls.member || !cls.stable || !cls.name_available || !EqualAscii("StaticMeshActor",cls.name,sizeof(cls.name)) ||
        !cls.fname.available || (cls.fname.value>>16) != 0 || !entry.owner_class.available ||
        !Value(cls.address,entry.owner_class.value) || !Value(node.address,before.mesh_node) ||
        asset.status != AssetStatus::Candidate || !asset.stable || !asset.name_available || !asset.audit.available ||
        !asset.descriptor_match || !asset.local_property_found || !asset.proxy_live_member ||
        !asset.proxy_vtable_match || !asset.resource_vtable_match || asset.audit.chain_count > kMaxAssetAuditSlots ||
        !before.element || !before.owner || !before.mesh_node || !before.asset_available || !before.asset_proxy ||
        !before.asset_resource || before.asset_proxy != asset.audit.proxy || before.asset_resource != asset.audit.resource ||
        !catalog.bed || !Digest(catalog.bed->source_sha256) || !GeometryComplete(catalog,*catalog.bed,&catalog.bed_materials) ||
        !EqualAscii("INNbed1A1",catalog.bed->name.c_str(),catalog.bed->name.size()+1) ||
        catalog.bed->id != "mesh:"+catalog.map+":"+catalog.map_sha256+":"+catalog.bed->source_sha256+":innbed1a1" ||
        !EqualAscii(catalog.bed->name,asset.name,sizeof(asset.name)) ||
        !EqualAscii(catalog.bed->name,before.asset_name,sizeof(before.asset_name)) ||
        !MeshMetadata(catalog,node) || section >= catalog.bed->geometry.sections.size()) return false;
    MeshSelection result; result.instance_id=catalog.bed_instance_id; result.resource_id=catalog.bed->id;
    result.generation=generation; result.binding=BindingEvidence::Candidate; result.material_ids=catalog.bed_materials;
    result.identity_scope=target.visible_list_only ? IdentityScope::VisibleList : IdentityScope::FullObjectScan;
    result.matched_resource=catalog.bed;
    for (std::size_t i=0;i<catalog.bed->geometry.sections.size();++i) {
        const auto& id=catalog.bed_materials.empty() ? catalog.bed->geometry.sections[i].material_id : catalog.bed_materials[i];
        result.matched_material_names.push_back(catalog.materials.at(*id).name);
    }
    if (!ReadFloats(before.matrix,result.transform.data(),16)) return false;
    result.sections.push_back(section); output=std::move(result); return true;
}

bool ResolveBsp(const BindingCatalog& catalog, const scene_bridge::ProbeSnapshot& snapshot,
                const scene_bridge::SubmissionSample& before, std::uint32_t section, BspSelection& output) {
    if (!catalog.bsp_catalog_complete || catalog.bsp.empty() || catalog.bsp.size()>4096 ||
        !General(catalog,snapshot,before) || before.type != 5 || before.vtable != kBspVtable) return false;
    const auto& bsp=snapshot.bsp;
    if (!bsp.source_stable || bsp.candidates_truncated || snapshot.entries_truncated ||
        !snapshot.count.available || !snapshot.array.available || !snapshot.array.value ||
        snapshot.entries_attempted > kMaxProbeEntries || snapshot.count.value != snapshot.entries_attempted ||
        bsp.candidates_copied > kMaxProbeBspBlocks || bsp.candidates_copied != bsp.member_count ||
        bsp.type5_seen != bsp.member_count ||
        !before.bsp_source || !before.bsp_blocks || !bsp.block_count || before.bsp_index>=bsp.block_count ||
        std::uint64_t{before.bsp_blocks}+std::uint64_t{before.bsp_index}*0x1ac != before.element ||
        !Value(bsp.source,before.bsp_source) || !Value(bsp.blocks,before.bsp_blocks)) return false;
    const ProbeBspBlock* current=nullptr;
    std::unordered_set<std::uint32_t> addresses, source_indices, list_indices;
    for (std::uint32_t i=0;i<bsp.candidates_copied;++i) {
        const auto& block=bsp.candidates[i];
        if (!ObservedBspComplete(block) || !block.address.available || !block.address.value ||
            block.list_index>=snapshot.entries_attempted || block.source_index>=bsp.block_count ||
            std::uint64_t{before.bsp_blocks}+std::uint64_t{block.source_index}*0x1ac != block.address.value ||
            !addresses.insert(block.address.value).second || !source_indices.insert(block.source_index).second ||
            !list_indices.insert(block.list_index).second) return false;
        const auto& list_entry=snapshot.entries[block.list_index];
        if (list_entry.list_index!=block.list_index || !Value(list_entry.address,block.address.value) ||
            !Value(list_entry.vtable,kBspVtable) || !Value(list_entry.type,5)) return false;
        if (Value(block.address,before.element) && block.source_index == before.bsp_index) {
            if (current) return false;
            current=&block;
        }
    }
    if (!current || current->list_index >= snapshot.entries_attempted ||
        snapshot.entries_attempted > kMaxProbeEntries ||
        snapshot.entries[current->list_index].list_index != current->list_index ||
        !Value(snapshot.entries[current->list_index].address,before.element) ||
        !Value(snapshot.entries[current->list_index].vtable,kBspVtable) ||
        !Value(snapshot.entries[current->list_index].type,5) ||
        std::memcmp(current->transform_bits,before.matrix,sizeof(before.matrix)) || !IdentityTransform(before.matrix)) return false;
    std::shared_ptr<const Resource> selected;
    std::unordered_set<std::string> ids;
    const auto prefix="bsp:"+catalog.map+":"+catalog.map_sha256+":";
    for (const auto& resource : catalog.bsp) {
        if (!resource || resource->kind!=ResourceKind::BspBlock || resource->source_sha256!=catalog.map_sha256 ||
            resource->id.compare(0,prefix.size(),prefix)!=0 || !ids.insert(resource->id).second ||
            !GeometryComplete(catalog,*resource)) return false;
        const auto suffix=resource->id.substr(prefix.size());
        const auto colon=suffix.find(':');
        const auto digit=[](unsigned char c) { return c>='0' && c<='9'; };
        if (colon==std::string::npos || !colon || colon+1==suffix.size() ||
            !std::all_of(suffix.begin(),suffix.begin()+colon,digit) ||
            !std::all_of(suffix.begin()+colon+1,suffix.end(),digit)) return false;
        if (BspMetadata(catalog,*resource,*current)) {
            if (selected) return false;
            selected=resource;
        }
    }
    if (!selected || section >= selected->geometry.sections.size()) return false;
    for (std::uint32_t i=0;i<bsp.candidates_copied;++i)
        if (&bsp.candidates[i] != current && BspMetadata(catalog,*selected,bsp.candidates[i])) return false;
    BspSelection result; result.resource_id=selected->id;result.binding=BindingEvidence::Candidate;
    result.identity_scope=snapshot.target.visible_list_only ? IdentityScope::VisibleList : IdentityScope::FullObjectScan;
    result.matched_resource=selected;
    for (const auto& part : selected->geometry.sections)
        result.matched_material_names.push_back(catalog.materials.at(*part.material_id).name);
    result.sections.push_back(section);output=std::move(result);return true;
}
bool ScopeDrawMatches(const Resource& resource, std::uint32_t section, const IndexedDraw& draw) noexcept {
    const auto& geometry=resource.geometry;
    if (!draw.indexed || !draw.triangle_list || !draw.vertex_count || section>=geometry.sections.size() ||
        std::uint64_t{draw.min_vertex}+draw.vertex_count>geometry.positions.size()) return false;
    const auto& part=geometry.sections[section];
    if (!part.index_count || part.index_count%3 || part.first_index!=draw.first_index ||
        part.index_count/3!=draw.triangle_count ||
        std::uint64_t{part.first_index}+part.index_count>geometry.indices.size()) return false;
    for (std::uint32_t i=0;i<part.index_count;++i) {
        const auto index=geometry.indices[part.first_index+i];
        if (index<draw.min_vertex || std::uint64_t{index}>=std::uint64_t{draw.min_vertex}+draw.vertex_count) return false;
    }
    return true;
}
bool ScopeCameraMatches(const ProbeCamera& camera, const Matrix& view, const Matrix& projection) noexcept {
    return std::memcmp(camera.view.data(),view.data(),sizeof(float)*view.size())==0 &&
           std::memcmp(camera.projection.data(),projection.data(),sizeof(float)*projection.size())==0;
}
bool ScopeWorldMatches(const scene_bridge::SubmissionSample& before, const Matrix& world) noexcept {
    return before.valid && std::memcmp(before.matrix,world.data(),sizeof(float)*world.size())==0;
}
bool ScopeUnchanged(const scene_bridge::SubmissionSample& before,
                    const scene_bridge::SubmissionSample& after) noexcept {
    return scene_bridge::SameSubmission(before,after);
}
} // namespace t3render::live
