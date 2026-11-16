#include "scene_live_catalog.hpp"
#include "scene_packet.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace t3render::live {
namespace {
using scene::Reader;
constexpr std::uint32_t kMaxMaterials=8192, kMaxBsp=4096, kMaxVertices=1000000, kMaxIndices=3000000, kMaxSections=1024;

bool Text(Reader& reader, std::string& output, bool optional=false) {
    std::uint32_t count=0;
    if (!reader.u32(count) || count>512 || count>reader.remaining() || (!optional && !count)) return false;
    std::string value(count,'\0');
    if (count && !reader.bytes(value.data(),count)) return false;
    if (value.find('\0')!=std::string::npos) return false;
    output=std::move(value);return true;
}
bool Digest(const std::string& value) {
    return value.size()==64 && std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    });
}
bool GeometryData(Reader& reader, Geometry& output) {
    std::uint32_t vertices=0, indices=0, sections=0, flags=0;
    if (!reader.u32(vertices) || !reader.u32(indices) || !reader.u32(sections) || !reader.u32(flags) ||
        !vertices || vertices>kMaxVertices || !indices || indices>kMaxIndices ||
        !sections || sections>kMaxSections || flags>7) return false;
    const std::uint64_t stride=12u+((flags&1)?12u:0u)+((flags&2)?8u:0u)+((flags&4)?12u:0u);
    const std::uint64_t minimum=std::uint64_t{vertices}*stride+std::uint64_t{indices}*4+std::uint64_t{sections}*17;
    if (minimum>reader.remaining()) return false;
    Geometry g;g.positions.resize(vertices);
    if (flags&1) g.normals.resize(vertices);
    if (flags&2) g.uv0.resize(vertices);
    if (flags&4) g.tangents.resize(vertices);
    for (auto& row:g.positions) for (float& v:row) if (!reader.f32(v)) return false;
    for (auto& row:g.normals) for (float& v:row) if (!reader.f32(v)) return false;
    for (auto& row:g.uv0) for (float& v:row) if (!reader.f32(v)) return false;
    for (auto& row:g.tangents) for (float& v:row) if (!reader.f32(v)) return false;
    g.indices.resize(indices);
    for (auto& index:g.indices) if (!reader.u32(index) || index>=vertices) return false;
    g.sections.reserve(sections);
    for (std::uint32_t i=0;i<sections;++i) {
        Section part;std::string material;
        if (!Text(reader,part.name) || !reader.u32(part.first_index) || !reader.u32(part.index_count) ||
            !part.index_count || part.index_count%3 || std::uint64_t{part.first_index}+part.index_count>indices ||
            !Text(reader,material,true)) return false;
        if (!material.empty()) part.material_id=std::move(material);
        g.sections.push_back(std::move(part));
    }
    output=std::move(g);return true;
}
bool ResourceData(Reader& reader, ResourceKind kind, Resource& output) {
    Resource resource;resource.kind=kind;
    if (!Text(reader,resource.id) || !Text(reader,resource.name) || !Text(reader,resource.source_sha256) ||
        !Digest(resource.source_sha256) || !GeometryData(reader,resource.geometry)) return false;
    output=std::move(resource);return true;
}
bool BspKey(const std::string& id, const std::string& prefix) {
    if (id.compare(0,prefix.size(),prefix)!=0) return false;
    const auto suffix=id.substr(prefix.size());const auto separator=suffix.find(':');
    const auto digit=[](unsigned char c){return c>='0' && c<='9';};
    return separator!=std::string::npos && separator && separator+1<suffix.size() &&
        std::all_of(suffix.begin(),suffix.begin()+separator,digit) &&
        std::all_of(suffix.begin()+separator+1,suffix.end(),digit);
}
} // namespace

bool LoadCatalog(const std::uint8_t* bytes, std::size_t size, CatalogDigest digest,
                 void* digest_context, LoadedCatalog& output) {
    if (!bytes || !digest || size<40 || size>kMaxCatalogBytes) return false;
    std::uint8_t computed[32]{};
    if (!digest(bytes,size-32,computed,digest_context) || std::memcmp(computed,bytes+size-32,32)) return false;
    Reader reader(bytes,size-32);char magic[8];
    LoadedCatalog loaded;std::string map, map_sha, actor;
    if (!reader.bytes(magic,sizeof(magic)) || std::memcmp(magic,"T3LCAT01",8) ||
        !Text(reader,map) || map.size()>128 || map.find(':')!=std::string::npos ||
        !Text(reader,map_sha) || !Digest(map_sha) || !Text(reader,loaded.offline_scene_sha256) ||
        !Digest(loaded.offline_scene_sha256) || !Text(reader,actor) ||
        actor!="actor:"+map+":"+map_sha+":StaticMeshActor__1520") return false;
    std::uint32_t handed=0,winding=0,uv=0,clip=0,has_scale=0;
    if (!reader.u32(handed) || handed>2 || !reader.u32(winding) || winding>2 ||
        !reader.u32(uv) || uv>2 || !reader.u32(clip) || clip>1 || !reader.u32(has_scale) || has_scale>1) return false;
    loaded.conventions.length_unit=LengthUnit::T3Unit;loaded.conventions.axes=Axes::T3Xyz;loaded.conventions.up_axis=UpAxis::Z;
    loaded.conventions.handedness=static_cast<Handedness>(handed);loaded.conventions.winding=static_cast<Winding>(winding);
    loaded.conventions.uv_origin=static_cast<UvOrigin>(uv);loaded.conventions.clip_depth=static_cast<ClipDepth>(clip);
    if (has_scale) { float value=0;if (!reader.f32(value) || value<=0) return false;loaded.conventions.meters_per_unit=value; }
    std::uint32_t material_count=0,bsp_count=0;
    if (!reader.u32(material_count) || !material_count || material_count>kMaxMaterials ||
        !reader.u32(bsp_count) || !bsp_count || bsp_count>kMaxBsp) return false;
    auto residency=std::make_shared<SceneResidency>();
    if (!residency->BeginMap(0,map,map_sha)) return false;
    const auto epoch=residency->epochs();
    BindingCatalog catalog;catalog.map=map;catalog.map_sha256=map_sha;catalog.bed_instance_id=actor;
    for (std::uint32_t i=0;i<material_count;++i) {
        Material material;
        if (!Text(reader,material.id) || !Text(reader,material.name) || catalog.materials.count(material.id) ||
            !residency->AddMaterial(epoch.map,material)) return false;
        const auto id=material.id;catalog.materials.emplace(id,std::move(material));
    }
    Resource bed;
    if (!ResourceData(reader,ResourceKind::StaticMesh,bed) ||
        bed.id!="mesh:"+map+":"+map_sha+":"+bed.source_sha256+":innbed1a1" ||
        bed.name.size()!=9 || !std::equal(bed.name.begin(),bed.name.end(),"innbed1a1",[](unsigned char a,unsigned char b){
            return (a>='A' && a<='Z' ? a+('a'-'A') : a)==b;
        })) return false;
    std::uint32_t skin_count=0;
    if (!reader.u32(skin_count) || (skin_count && skin_count!=bed.geometry.sections.size())) return false;
    catalog.bed_materials.reserve(skin_count);
    for (std::uint32_t i=0;i<skin_count;++i) {
        std::string material;if (!Text(reader,material,true)) return false;
        if (material.empty()) catalog.bed_materials.push_back(std::nullopt);
        else catalog.bed_materials.push_back(std::move(material));
    }
    for (std::size_t i=0;i<bed.geometry.sections.size();++i) {
        const auto& id=catalog.bed_materials.empty() ? bed.geometry.sections[i].material_id : catalog.bed_materials[i];
        if (!id || !catalog.materials.count(*id)) return false;
    }
    const auto bed_id=bed.id;
    if (!residency->AddResource(epoch.map,std::move(bed))) return false;
    catalog.bed=residency->FindSceneResource(epoch.map,bed_id);
    if (!catalog.bed) return false;
    std::unordered_set<std::string> ids;
    const auto prefix="bsp:"+map+":"+map_sha+":";
    catalog.bsp.reserve(bsp_count);
    for (std::uint32_t i=0;i<bsp_count;++i) {
        Resource block;
        if (!ResourceData(reader,ResourceKind::BspBlock,block) || block.source_sha256!=map_sha ||
            !BspKey(block.id,prefix) || !ids.insert(block.id).second) return false;
        for (const auto& section:block.geometry.sections)
            if (!section.material_id || !catalog.materials.count(*section.material_id)) return false;
        const auto id=block.id;
        if (!residency->AddResource(epoch.map,std::move(block))) return false;
        auto resource=residency->FindSceneResource(epoch.map,id);
        if (!resource) return false;
        catalog.bsp.push_back(std::move(resource));
    }
    if (!reader.end()) return false;
    catalog.bsp_catalog_complete=true; // complete only relative to the offline exporter
    loaded.residency=std::move(residency);loaded.binding=std::move(catalog);output=std::move(loaded);
    return true;
}
} // namespace t3render::live
