#include "scene_live_catalog.hpp"
#include "scene_packet.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
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
bool StaticActorKey(const std::string& id,const std::string& prefix) {
    if (id.compare(0,prefix.size(),prefix)!=0) return false;
    const auto suffix=id.substr(prefix.size());
    if (suffix.empty() || (suffix.size()>1 && suffix[0]=='0')) return false;
    std::uint32_t number=0;
    for (unsigned char c:suffix) {
        if (c<'0' || c>'9' || number>(65534u-(c-'0'))/10u) return false;
        number=number*10u+(c-'0');
    }
    return true;
}
bool StaticMeshKey(const Resource& resource,const std::string& prefix) {
    if (resource.name.empty()) return false;
    std::string lower;lower.reserve(resource.name.size());
    for (unsigned char c:resource.name) {
        if (c<33 || c>126 || c==':') return false;
        lower.push_back(static_cast<char>(c>='A' && c<='Z' ? c+('a'-'A') : c));
    }
    return resource.id==prefix+resource.source_sha256+":"+lower;
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
bool MaterialTextureData(Reader& reader, bool opaque, std::string& fingerprint, scene::Texture& output) {
    std::uint32_t width=0,height=0,count=0;
    if (!Text(reader,fingerprint) || !Digest(fingerprint) || !reader.u32(width) || !reader.u32(height) ||
        !reader.u32(count) || !width || width>1024 || width!=height || (width&(width-1))) return false;
    std::uint32_t levels=1;
    for (auto size=width;size>1;size>>=1) ++levels;
    if (count!=levels) return false;
    scene::Texture texture;texture.mips.reserve(count);
    for (std::uint32_t level=0;level<count;++level) {
        const auto bytes=std::size_t(width)*height*4;
        if (bytes>reader.remaining()) return false;
        scene::Mip mip{width,height,std::vector<std::uint8_t>(bytes)};
        if (!reader.bytes(mip.rgba.data(),bytes)) return false;
        if (opaque) for (std::size_t i=3;i<bytes;i+=4) if (mip.rgba[i]!=255) return false;
        texture.mips.push_back(std::move(mip));
        width=std::max(1u,width/2);height=std::max(1u,height/2);
    }
    output=std::move(texture);return true;
}
bool BspKey(const std::string& id, const std::string& prefix) {
    if (id.compare(0,prefix.size(),prefix)!=0) return false;
    const auto suffix=id.substr(prefix.size());const auto separator=suffix.find(':');
    const auto digit=[](unsigned char c){return c>='0' && c<='9';};
    return separator!=std::string::npos && separator && separator+1<suffix.size() &&
        std::all_of(suffix.begin(),suffix.begin()+separator,digit) &&
        std::all_of(suffix.begin()+separator+1,suffix.end(),digit);
}
bool ValidBedOverride(const Resource& original, const Resource& replacement, const Conventions& conventions,
                      const std::unordered_map<std::string,Material>& materials) {
    static constexpr const char* names[]{"BedTrimWood", "HeadboardWood", "GENiFABRICwoolA2_D",
                                          "BlanketSeam", "PillowLinen", "PillowSeam"};
    const auto& g=replacement.geometry;
    if (!conventions.meters_per_unit || std::fabs(*conventions.meters_per_unit-0.01905f)>1e-9f ||
        replacement.name!="bed_high" || replacement.id!="remaster:mesh:bed_high:"+replacement.source_sha256 ||
        g.positions.empty() || g.positions.size()>50000 || g.indices.empty() || g.indices.size()>150000 ||
        g.normals.size()!=g.positions.size() || g.uv0.size()!=g.positions.size() || !g.tangents.empty() ||
        g.sections.size()!=std::size(names) || materials.size()!=std::size(names) || original.geometry.positions.empty()) return false;
    std::uint64_t next=0;
    for (std::size_t i=0;i<std::size(names);++i) {
        const auto id=std::string("remaster:material:bed_high:")+names[i];
        const auto& part=g.sections[i];const auto material=materials.find(id);
        if (part.name!=std::string("generated:")+names[i] || part.first_index!=next || !part.material_id ||
            *part.material_id!=id || material==materials.end() || material->second.name!=names[i]) return false;
        next+=part.index_count;
    }
    if (next!=g.indices.size()) return false;
    for (const auto& normal:g.normals) {
        const double length=double(normal[0])*normal[0]+double(normal[1])*normal[1]+double(normal[2])*normal[2];
        if (!std::isfinite(length) || length<0.98 || length>1.02) return false;
    }
    for (const auto& uv:g.uv0) if (std::fabs(uv[0])>10000 || std::fabs(uv[1])>10000) return false;
    for (unsigned axis=0;axis<3;++axis) {
        auto bounds=[axis](const std::vector<Vec3>& points) {
            float low=points[0][axis],high=low;
            for (const auto& p:points) { low=std::min(low,p[axis]);high=std::max(high,p[axis]); }
            return std::pair<float,float>{low,high};
        };
        const auto a=bounds(original.geometry.positions), b=bounds(g.positions);
        if (std::fabs(double(a.first)-b.first)* *conventions.meters_per_unit>0.30001 ||
            std::fabs(double(a.second)-b.second)* *conventions.meters_per_unit>0.30001) return false;
    }
    return true;
}

bool LoadGeneralMap(Reader& reader,LoadedCatalog& output) {
    LoadedCatalog loaded;std::string map,map_sha;
    if (!Text(reader,map) || map.size()>128 || map.find(':')!=std::string::npos ||
        !Text(reader,map_sha) || !Digest(map_sha) || !Text(reader,loaded.offline_scene_sha256) ||
        !Digest(loaded.offline_scene_sha256)) return false;
    std::uint32_t handed=0,winding=0,uv=0,clip=0,has_scale=0;
    if (!reader.u32(handed) || handed>2 || !reader.u32(winding) || winding>2 ||
        !reader.u32(uv) || uv>2 || !reader.u32(clip) || clip>1 || !reader.u32(has_scale) || has_scale>1) return false;
    loaded.conventions.length_unit=LengthUnit::T3Unit;loaded.conventions.axes=Axes::T3Xyz;loaded.conventions.up_axis=UpAxis::Z;
    loaded.conventions.handedness=static_cast<Handedness>(handed);loaded.conventions.winding=static_cast<Winding>(winding);
    loaded.conventions.uv_origin=static_cast<UvOrigin>(uv);loaded.conventions.clip_depth=static_cast<ClipDepth>(clip);
    if (has_scale) {float value=0;if (!reader.f32(value) || value<=0) return false;loaded.conventions.meters_per_unit=value;}
    std::uint32_t material_count=0,bsp_count=0;
    if (!reader.u32(material_count) || !material_count || material_count>kMaxMaterials ||
        !reader.u32(bsp_count) || bsp_count>kMaxBsp) return false;
    auto residency=std::make_shared<SceneResidency>();
    if (!residency->BeginMap(0,map,map_sha)) return false;
    const auto epoch=residency->epochs();BindingCatalog catalog;catalog.map=map;catalog.map_sha256=map_sha;
    for (std::uint32_t i=0;i<material_count;++i) {
        Material material;
        if (!Text(reader,material.id) || !Text(reader,material.name) || catalog.materials.count(material.id) ||
            !residency->AddMaterial(epoch.map,material)) return false;
        const auto id=material.id;catalog.materials.emplace(id,std::move(material));
    }
    const auto bsp_prefix="bsp:"+map+":"+map_sha+":";
    std::unordered_set<std::string> bsp_ids;catalog.bsp.reserve(bsp_count);
    for (std::uint32_t i=0;i<bsp_count;++i) {
        Resource block;
        if (!ResourceData(reader,ResourceKind::BspBlock,block) || block.source_sha256!=map_sha ||
            !BspKey(block.id,bsp_prefix) || !bsp_ids.insert(block.id).second) return false;
        for (const auto& section:block.geometry.sections)
            if (!section.material_id || !catalog.materials.count(*section.material_id)) return false;
        const auto id=block.id;if (!residency->AddResource(epoch.map,std::move(block))) return false;
        auto resource=residency->FindSceneResource(epoch.map,id);if (!resource) return false;
        catalog.bsp.push_back(std::move(resource));
    }
    std::uint32_t fingerprint_count=0;
    if (!reader.u32(fingerprint_count) || fingerprint_count>2) return false;
    std::unordered_set<std::string> fingerprints;
    for (std::uint32_t i=0;i<fingerprint_count;++i) {
        std::string value;if (!Text(reader,value) || !Digest(value) || !fingerprints.insert(value).second) return false;
    }
    std::uint32_t mesh_count=0;
    if (!reader.u32(mesh_count) || mesh_count>4096u-bsp_count) return false;
    std::unordered_map<std::string,std::shared_ptr<const Resource>> resources;
    std::unordered_set<std::string> names;const auto mesh_prefix="mesh:"+map+":"+map_sha+":";
    for (std::uint32_t i=0;i<mesh_count;++i) {
        Resource mesh;
        if (!ResourceData(reader,ResourceKind::StaticMesh,mesh) || !fingerprints.count(mesh.source_sha256) ||
            mesh.geometry.positions.size()>65536 || mesh.geometry.indices.size()>65536 ||
            mesh.geometry.sections.size()>scene_bridge::kMaxMeshNodeSections || !StaticMeshKey(mesh,mesh_prefix) ||
            resources.count(mesh.id)) return false;
        std::string name=mesh.name;
        for (char& c:name) if (c>='A' && c<='Z') c=static_cast<char>(c+('a'-'A'));
        if (!names.insert(name).second) return false;
        for (const auto& part:mesh.geometry.sections)
            if (part.material_id && !catalog.materials.count(*part.material_id)) return false;
        const auto id=mesh.id;if (!residency->AddResource(epoch.map,std::move(mesh))) return false;
        auto resource=residency->FindSceneResource(epoch.map,id);
        if (!resource || !resources.emplace(id,std::move(resource)).second) return false;
    }
    std::uint32_t actor_count=0;
    if (!reader.u32(actor_count) || actor_count>8192) return false;
    const auto actor_prefix="actor:"+map+":"+map_sha+":StaticMeshActor__";
    std::unordered_set<std::string> used_resources;
    for (std::uint32_t i=0;i<actor_count;++i) {
        StaticMeshCandidate candidate;std::string resource_id;std::uint32_t count=0;
        if (!Text(reader,candidate.instance_id) || !StaticActorKey(candidate.instance_id,actor_prefix) ||
            !Text(reader,resource_id) || !reader.u32(count) || !count || count>kMaxSections) return false;
        const auto found=resources.find(resource_id);
        if (found==resources.end() || count!=found->second->geometry.sections.size()) return false;
        candidate.resource=found->second;candidate.material_ids.reserve(count);
        for (std::uint32_t section=0;section<count;++section) {
            std::string material;if (!Text(reader,material) || !catalog.materials.count(material)) return false;
            candidate.material_ids.push_back(std::move(material));
        }
        used_resources.insert(resource_id);const auto key=candidate.instance_id;
        if (!catalog.static_mesh_actors.emplace(key,std::move(candidate)).second) return false;
    }
    if ((!bsp_count && !actor_count) || used_resources.size()!=resources.size() || !reader.end()) return false;
    catalog.bsp_catalog_complete=true; // completeness comes from the offline importer
    loaded.residency=std::move(residency);loaded.binding=std::move(catalog);output=std::move(loaded);
    return true;
}
} // namespace

bool LoadCatalog(const std::uint8_t* bytes, std::size_t size, CatalogDigest digest,
                 void* digest_context, LoadedCatalog& output) {
    if (!bytes || !digest || size<40 || size>kMaxCatalogBytes) return false;
    std::uint8_t computed[32]{};
    if (!digest(bytes,size-32,computed,digest_context) || std::memcmp(computed,bytes+size-32,32)) return false;
    Reader reader(bytes,size-32);char magic[8];
    LoadedCatalog loaded;std::string map, map_sha, actor;
    if (!reader.bytes(magic,sizeof(magic))) return false;
    if (std::memcmp(magic,"T3LCAT05",8)==0) return LoadGeneralMap(reader,output);
    const bool version_two=std::memcmp(magic,"T3LCAT02",8)==0;
    const bool version_three=std::memcmp(magic,"T3LCAT03",8)==0;
    const bool version_four=std::memcmp(magic,"T3LCAT04",8)==0;
    if ((!version_two && !version_three && !version_four && std::memcmp(magic,"T3LCAT01",8)) || !Text(reader,map) || map.size()>128 || map.find(':')!=std::string::npos ||
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
    if (version_three && (map!="SouthQuarter1_int1" || !loaded.conventions.meters_per_unit ||
                          std::fabs(*loaded.conventions.meters_per_unit-0.01905f)>1e-9f)) return false;
    std::uint32_t material_count=0,bsp_count=0;
    if (!reader.u32(material_count) || !material_count || material_count>kMaxMaterials ||
        !reader.u32(bsp_count) || !bsp_count || bsp_count>kMaxBsp) return false;
    auto residency=std::make_shared<SceneResidency>();
    if (!residency->BeginMap(0,map,map_sha)) return false;
    const auto epoch=residency->epochs();
    BindingCatalog catalog;catalog.map=map;catalog.map_sha256=map_sha;catalog.bed_instance_id=actor;
    std::unordered_map<std::string,Material> override_materials;
    const std::string override_prefix="remaster:material:bed_high:";
    for (std::uint32_t i=0;i<material_count;++i) {
        Material material;
        if (!Text(reader,material.id) || !Text(reader,material.name) || catalog.materials.count(material.id) ||
            override_materials.count(material.id)) return false;
        const auto id=material.id;
        if ((version_three || version_four) && (id=="remaster:material:stone_wall_v2" || id=="remaster:material:wood_floor_v2")) return false;
        if ((version_two || version_three || version_four) && id.compare(0,override_prefix.size(),override_prefix)==0)
            override_materials.emplace(id,std::move(material));
        else {
            if (!residency->AddMaterial(epoch.map,material)) return false;
            catalog.materials.emplace(id,std::move(material));
        }
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
    std::uint32_t has_override=version_two ? 1u : 0u;
    if ((version_three || version_four) && (!reader.u32(has_override) || has_override>1)) return false;
    std::uint32_t has_materials=version_three ? 1u : 0u;
    if (version_four && (!reader.u32(has_materials) || has_materials>1)) return false;
    if (has_materials && (map!="SouthQuarter1_int1" || !loaded.conventions.meters_per_unit ||
                          std::fabs(*loaded.conventions.meters_per_unit-0.01905f)>1e-9f)) return false;
    if (has_override) {
        Resource replacement;
        if (!ResourceData(reader,ResourceKind::StaticMesh,replacement) ||
            !ValidBedOverride(*catalog.bed,replacement,loaded.conventions,override_materials)) return false;
        auto rule=std::make_shared<MeshOverride>();
        rule->map=map;rule->map_sha256=map_sha;rule->instance_id=actor;
        rule->source=catalog.bed;rule->replacement=std::make_shared<const Resource>(std::move(replacement));
        loaded.bed_override=std::move(rule);
    } else if (!override_materials.empty()) return false;
    if (has_materials) {
        std::uint32_t count=0;
        if (!reader.u32(count) || count!=2) return false;
        auto library=std::make_shared<BedroomMaterialLibrary>();
        static constexpr const char* ids[]{"remaster:material:stone_wall_v2", "remaster:material:wood_floor_v2"};
        std::uint32_t size=0;
        for (unsigned i=0;i<2;++i) {
            auto& material=library->materials[i];
            if (!Text(reader,material.id) || material.id!=ids[i]) return false;
            for (unsigned channel=0;channel<4;++channel) {
                if (!MaterialTextureData(reader,channel==0,material.source_sha256[channel],material.channels[channel])) return false;
                const auto& mip=material.channels[channel].mips.front();
                if (!size) size=mip.width;
                if (mip.width!=size || mip.height!=size) return false;
            }
        }
        loaded.bedroom_materials=std::move(library);
    }
    if (version_four) {
        if (catalog.bed->geometry.positions.size()>65536 || catalog.bed->geometry.indices.size()>65536 ||
            catalog.bed->geometry.sections.size()>scene_bridge::kMaxMeshNodeSections) return false;
        std::uint32_t fingerprint_count=0;
        if (!reader.u32(fingerprint_count) || !fingerprint_count || fingerprint_count>2) return false;
        std::unordered_set<std::string> fingerprints;
        for (std::uint32_t i=0;i<fingerprint_count;++i) {
            std::string value;
            if (!Text(reader,value) || !Digest(value) || !fingerprints.insert(value).second) return false;
        }
        if (!fingerprints.count(catalog.bed->source_sha256)) return false;
        std::uint32_t mesh_count=0;
        if (bsp_count>=4096 || !reader.u32(mesh_count) || mesh_count>4095u-bsp_count) return false;
        std::unordered_map<std::string,std::shared_ptr<const Resource>> resources;
        resources.emplace(catalog.bed->id,catalog.bed);
        std::unordered_set<std::string> mesh_names;
        const auto mesh_prefix="mesh:"+map+":"+map_sha+":";
        auto name_key=[](const std::string& name) {
            std::string value=name;
            for (char& c:value) if (c>='A' && c<='Z') c=static_cast<char>(c+('a'-'A'));
            return value;
        };
        mesh_names.insert(name_key(catalog.bed->name));
        for (std::uint32_t i=0;i<mesh_count;++i) {
            Resource mesh;
            if (!ResourceData(reader,ResourceKind::StaticMesh,mesh) || !fingerprints.count(mesh.source_sha256) ||
                mesh.geometry.positions.size()>65536 || mesh.geometry.indices.size()>65536 ||
                mesh.geometry.sections.size()>scene_bridge::kMaxMeshNodeSections || !StaticMeshKey(mesh,mesh_prefix) ||
                resources.count(mesh.id) || !mesh_names.insert(name_key(mesh.name)).second) return false;
            for (const auto& part:mesh.geometry.sections)
                if (part.material_id && !catalog.materials.count(*part.material_id)) return false;
            const auto id=mesh.id;
            if (!residency->AddResource(epoch.map,std::move(mesh))) return false;
            auto resource=residency->FindSceneResource(epoch.map,id);
            if (!resource || !resources.emplace(id,std::move(resource)).second) return false;
        }
        std::uint32_t actor_count=0;
        if (!reader.u32(actor_count) || !actor_count || actor_count>8192) return false;
        const auto actor_prefix="actor:"+map+":"+map_sha+":StaticMeshActor__";
        std::unordered_set<std::string> used_resources;
        for (std::uint32_t i=0;i<actor_count;++i) {
            StaticMeshCandidate candidate;std::string resource_id;std::uint32_t count=0;
            if (!Text(reader,candidate.instance_id) || !StaticActorKey(candidate.instance_id,actor_prefix) ||
                !Text(reader,resource_id) || !reader.u32(count) || !count || count>kMaxSections) return false;
            const auto found=resources.find(resource_id);
            if (found==resources.end() || count!=found->second->geometry.sections.size()) return false;
            candidate.resource=found->second;candidate.material_ids.reserve(count);
            for (std::uint32_t section=0;section<count;++section) {
                std::string material;
                if (!Text(reader,material) || !catalog.materials.count(material)) return false;
                candidate.material_ids.push_back(std::move(material));
            }
            used_resources.insert(resource_id);
            const auto key=candidate.instance_id;
            if (!catalog.static_mesh_actors.emplace(key,std::move(candidate)).second) return false;
        }
        const auto target=catalog.static_mesh_actors.find(actor);
        if (target==catalog.static_mesh_actors.end() || target->second.resource!=catalog.bed) return false;
        for (std::size_t i=0;i<catalog.bed->geometry.sections.size();++i) {
            const auto& expected=catalog.bed_materials.empty() ? catalog.bed->geometry.sections[i].material_id : catalog.bed_materials[i];
            if (target->second.material_ids[i]!=expected) return false;
        }
        if (used_resources.size()!=resources.size()) return false;
    }
    if (!reader.end()) return false;
    catalog.bsp_catalog_complete=true; // complete only relative to the offline exporter
    loaded.residency=std::move(residency);loaded.binding=std::move(catalog);output=std::move(loaded);
    return true;
}
} // namespace t3render::live
