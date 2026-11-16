#include "../src/scene_live_catalog.hpp"
#include "../src/scene_live_driver.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <unordered_set>
#include <vector>

using namespace t3render::live;
namespace {
struct Expected { std::uint8_t bytes[32]{}; };
bool Digest(const std::uint8_t*,std::size_t,std::uint8_t* output,void* context) noexcept {
    std::memcpy(output,static_cast<Expected*>(context)->bytes,32);return true;
}
int hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    return -1;
}
} // namespace
int main(int argc,char** argv) {
    if (argc!=2 || std::strlen(argv[1])!=64) return 3;
    Expected expected;
    for (unsigned i=0;i<32;++i) {
        const int a=hex(argv[1][2*i]),b=hex(argv[1][2*i+1]);
        if (a<0 || b<0) return 3;
        expected.bytes[i]=static_cast<std::uint8_t>((a<<4)|b);
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(std::cin)),{});
    LoadedCatalog catalog;catalog.offline_scene_sha256="unchanged";
    if (!LoadCatalog(bytes.data(),bytes.size(),Digest,&expected,catalog)) {
        if (catalog.offline_scene_sha256!="unchanged" || catalog.residency) return 4;
        std::cout<<"rejected\n";return 0;
    }
    const auto epoch=catalog.residency->epochs();
    const auto& bed=catalog.binding.bed;
    if ((bed && catalog.residency->FindSceneResource(epoch.map,bed->id)!=bed) ||
        (!bed && (!catalog.binding.bed_instance_id.empty() || !catalog.binding.bed_materials.empty() || catalog.bed_override)) ||
        (!catalog.binding.bsp.empty() && catalog.residency->FindSceneResource(epoch.map,catalog.binding.bsp[0]->id)!=catalog.binding.bsp[0]) ||
        (!bed && catalog.binding.bsp.empty() && catalog.binding.static_mesh_actors.empty())) return 5;
    std::cout<<"accepted "<<catalog.binding.map<<" "<<catalog.binding.materials.size()<<" "
             <<catalog.binding.bsp.size()<<" "<<(bed ? bed->geometry.positions.size() : 0)<<" "<<(bed ? bed->geometry.indices.size() : 0)<<" "
             <<(bed ? bed->geometry.normals.size() : 0)<<" "<<(bed ? bed->geometry.uv0.size() : 0)<<" "<<(bed ? bed->geometry.tangents.size() : 0)<<" "
             <<catalog.binding.bed_materials.size()<<" "<<catalog.offline_scene_sha256;
    if (catalog.bed_override) {
        const auto& rule=*catalog.bed_override;
        if (rule.source!=bed || !rule.replacement || rule.map!=catalog.binding.map ||
            rule.instance_id!=catalog.binding.bed_instance_id ||
            catalog.residency->FindSceneResource(epoch.map,rule.replacement->id)) return 6;
        std::cout<<" override "<<rule.replacement->geometry.positions.size()<<" "
                 <<rule.replacement->geometry.indices.size()<<" "<<rule.replacement->geometry.sections.size()<<" "
                 <<rule.replacement->source_sha256;
    }
    if (catalog.bedroom_materials) {
        for (const auto& material:catalog.bedroom_materials->materials) {
            if (catalog.binding.materials.count(material.id)) return 7;
            for (const auto& texture:material.channels) if (texture.mips.empty()) return 7;
        }
        const auto& wall=catalog.bedroom_materials->materials[0];
        std::cout<<" bedroom_materials "<<catalog.bedroom_materials->materials.size()<<" "
                 <<wall.channels[0].mips[0].width<<" "<<wall.channels[0].mips.size();
    }
    if (!catalog.binding.static_mesh_actors.empty()) {
        std::unordered_set<std::string> resources;
        for (const auto& [id,candidate]:catalog.binding.static_mesh_actors) {
            if (id!=candidate.instance_id || !candidate.resource ||
                catalog.residency->FindSceneResource(epoch.map,candidate.resource->id)!=candidate.resource ||
                candidate.material_ids.size()!=candidate.resource->geometry.sections.size()) return 8;
            resources.insert(candidate.resource->id);
        }
        std::cout<<" static_mesh_candidates "<<catalog.binding.static_mesh_actors.size()<<" "<<resources.size();
    }
    if (!bed) {
        FrameProvenance source;source.origin=FrameOrigin::Synthetic;source.session_id="general-catalog-fixture";
        if (!SceneDriver::Create(std::move(catalog),std::move(source),0,1)) return 9;
        std::cout<<" bedless_driver_ok";
    }
    std::cout<<"\n";
}
