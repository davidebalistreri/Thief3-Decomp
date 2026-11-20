#include "../src/scene_benchmark.hpp"
#include <iostream>

using namespace t3render::live;
int main() {
    const std::string map="SouthQuarter1_int1";
    const std::string hash="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
    const std::string actor="actor:"+map+":"+hash+":StaticMeshActor__1520";
    LoadedCatalog c;c.residency=std::make_shared<SceneResidency>();
    if (!c.residency->BeginMap(0,map,hash)) return 2;
    auto epoch=c.residency->epochs();
    Resource mesh;mesh.id="mesh:bed";mesh.name="bed";mesh.source_sha256=std::string(64,'a');
    mesh.geometry.positions={{0,0,0},{1,0,0},{0,1,0}};mesh.geometry.indices={0,1,2};
    mesh.geometry.sections={{"bed",0,3,std::nullopt}};
    Resource bsp=mesh;bsp.id="bsp:wall";bsp.name="wall";bsp.kind=ResourceKind::BspBlock;
    if (!c.residency->AddResource(epoch.map,mesh) || !c.residency->AddResource(epoch.map,bsp)) return 3;
    auto bed=c.residency->FindSceneResource(epoch.map,mesh.id);
    auto wall=c.residency->FindSceneResource(epoch.map,bsp.id);
    c.binding.map=map;c.binding.map_sha256=hash;c.binding.bed_instance_id=actor;c.binding.bed=bed;
    c.binding.static_mesh_actors.emplace(actor,StaticMeshCandidate{actor,bed,{}});c.binding.bsp.push_back(wall);
    c.bed_override=std::make_shared<MeshOverride>();c.bedroom_materials=std::make_shared<BedroomMaterialLibrary>();
    c.conventions.length_unit=LengthUnit::T3Unit;c.conventions.axes=Axes::T3Xyz;
    c.conventions.up_axis=UpAxis::Z;c.conventions.meters_per_unit=0.01905f;
    BenchmarkScene result;
    if (!LoadBenchmarkScene(std::cin,c,result)) {std::cout<<"rejected\n";return 0;}
    if (!result.on || !result.off || !RawLightFrameMatches(result.on_lights,*result.on) ||
        !RawLightFrameMatches(result.off_lights,*result.off) || result.on->camera.view!=result.off->camera.view ||
        result.on->camera.projection!=result.off->camera.projection) return 4;
    auto live=*result.on;live.provenance.origin=FrameOrigin::LiveProcess;live.provenance.session_id="live:fixture";
    live.camera.view[12]=999;live.meshes[0].selection.transform[12]=17;
    BenchmarkScene modified;std::size_t meshes=0,blocks=0;
    if (!BenchmarkFromLive(result,live,modified,meshes,blocks) || meshes!=1 || blocks!=1 ||
        modified.on->camera.view!=result.on->camera.view || modified.off->camera.view!=result.on->camera.view ||
        modified.on->meshes[0].selection.transform[12]!=17 || modified.off->meshes[0].selection.transform[12]!=17 ||
        result.on->meshes[0].selection.transform[12]==17 || !RawLightFrameMatches(modified.on_lights,*modified.on)) return 5;
    live.map_sha256=std::string(64,'b');
    if (BenchmarkFromLive(result,live,modified,meshes,blocks)) return 6;
    live.map_sha256=hash;live.continuity=0;
    if (BenchmarkFromLive(result,live,modified,meshes,blocks)) return 7;
    live.continuity=1;live.meshes[0].selection.identity_scope=IdentityScope::Unknown;
    if (BenchmarkFromLive(result,live,modified,meshes,blocks)) return 8;
    live.meshes.clear();live.bsp.clear();
    if (BenchmarkFromLive(result,live,modified,meshes,blocks)) return 9;
    std::cout<<"accepted\n";return 0;
}
