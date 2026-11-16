#include "../src/scene_live_dx12.hpp"
#include "../src/scene_live_flat.hpp"
#include "../src/scene_live_catalog_win.hpp"
#include "../src/scene_live_viewer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <string>

using namespace t3render::live;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; return false; } } while (0)

namespace {
const Matrix identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
const std::string fingerprint(64, 'a');

Resource quad(const std::string& id, ResourceKind kind, float extent, float z, const char* material) {
    Resource value; value.id=id; value.name=id; value.kind=kind; value.source_sha256=fingerprint;
    value.geometry.positions={{-extent,-extent,z},{extent,-extent,z},{extent,extent,z},{-extent,extent,z}};
    value.geometry.uv0={{0,0},{1,0},{1,1},{0,1}};
    value.geometry.indices={0,1,2,0,2,3}; value.geometry.sections={{"surface",0,6,material}};
    return value;
}
FrameInput input(const Epochs& epoch, std::uint64_t frame) {
    FrameInput value; value.map_epoch=epoch.map; value.device_epoch=epoch.device; value.frame_id=frame;
    value.provenance.origin=FrameOrigin::Synthetic; value.provenance.session_id="gpu-fixture"; value.synchronized=true;
    value.conventions.length_unit=LengthUnit::T3Unit; value.conventions.axes=Axes::T3Xyz; value.conventions.up_axis=UpAxis::Z;
    value.camera.view=identity; value.camera.projection=identity; value.camera.width=64; value.camera.height=64;
    MeshSelection mesh; mesh.instance_id="actor:bed"; mesh.resource_id="mesh:bed";
    mesh.generation=1; mesh.binding=BindingEvidence::Candidate; mesh.transform=identity; mesh.sections={0};
    BspSelection bsp; bsp.resource_id="bsp:wall"; bsp.binding=BindingEvidence::Candidate; bsp.sections={0};
    value.meshes.push_back(mesh); value.bsp.push_back(bsp); return value;
}
bool pixel_is(const std::vector<std::uint8_t>& pixels, unsigned width, unsigned x, unsigned y, const float* color) {
    const auto offset=(std::size_t(y)*width+x)*4;
    for (unsigned c=0;c<3;++c)
        if (std::abs(int(pixels[offset+c])-int(std::lround(color[c]*255)))>1) return false;
    return pixels[offset+3]==255;
}
bool run(HWND window) {
    SceneResidency scene; CHECK(scene.BeginMap(0,"fixture",fingerprint));
    const auto epoch=scene.epochs();
    CHECK(scene.AddMaterial(epoch.map,{"mat:bed","bed"})); CHECK(scene.AddMaterial(epoch.map,{"mat:wall","wall"}));
    CHECK(scene.AddResource(epoch.map,quad("mesh:bed",ResourceKind::StaticMesh,0.3f,0.2f,"mat:bed")));
    CHECK(scene.AddResource(epoch.map,quad("bsp:wall",ResourceKind::BspBlock,0.9f,0.8f,"mat:wall")));
    auto gpu=FlatD3D12::Create(window); CHECK(gpu);
    auto resident_gpu=FlatD3D12::Create(nullptr);CHECK(resident_gpu);
    auto render_resident=[&](std::vector<std::uint8_t>& pixels) {
        ResidentFlatFrame resident;
        return BuildResidentFlatFrame(*scene.current(),resident) && resident_gpu->PrepareResident(resident,true) &&
               resident_gpu->Submit() && resident_gpu->Finish(&pixels) && resident_gpu->Present()!=FlatPresent::Failed;
    };
    auto frame=input(epoch,1); CHECK(scene.Publish(frame));
    t3render::scene::Frame flat; CHECK(BuildFlatFrame(*scene.current(),flat));
    std::vector<std::uint8_t> first, moved, no_bed, empty, resized;
    CHECK(gpu->Render(flat,&first)); CHECK(first.size()==64*64*4);
    std::vector<std::uint8_t> resident_image;
    CHECK(render_resident(resident_image) && resident_image==first);
    CHECK(resident_gpu->cache_stats().uploads==2 && resident_gpu->cache_stats().resources==2);
    // Splitting an indexed section must preserve the image and resident GPU
    // buffers; changing one range must affect only that triangle.
    ResidentFlatFrame split;CHECK(BuildResidentFlatFrame(*scene.current(),split));
    CHECK(split.draws[0].index_count==6);
    auto other=split.draws[0];split.draws[0].index_count=3;other.first_index+=3;other.index_count=3;
    split.draws.insert(split.draws.begin()+1,other);
    std::vector<std::uint8_t> split_image,colored;
    CHECK(resident_gpu->PrepareResident(split,true) && resident_gpu->Submit() && resident_gpu->Finish(&split_image));
    CHECK(split_image==first && resident_gpu->cache_stats().uploads==2);
    split.draws[1].color={1,0,0,1};
    CHECK(resident_gpu->PrepareResident(split,true) && resident_gpu->Submit() && resident_gpu->Finish(&colored));
    CHECK(colored!=first && resident_gpu->cache_stats().uploads==2);
    auto invalid_range=FlatD3D12::Create(nullptr);CHECK(invalid_range);
    split.draws[1].first_index=1;CHECK(!invalid_range->PrepareResident(split));
    const auto one_resource_budget=resident_gpu->cache_stats().bytes/2;
    CHECK(one_resource_budget>0 && one_resource_budget*2==resident_gpu->cache_stats().bytes);
    CHECK(pixel_is(first,64,32,32,flat.draws[0].ambient_color));
    CHECK(pixel_is(first,64,12,12,flat.draws[1].ambient_color));
    frame.frame_id=2; frame.camera.view[12]=0.5f; CHECK(scene.Publish(frame));
    CHECK(BuildFlatFrame(*scene.current(),flat));
    CHECK(gpu->Prepare(flat,true)); gpu->Cancel(); // a stale snapshot is never submitted
    CHECK(gpu->Prepare(flat,true)); CHECK(gpu->Submit()); CHECK(gpu->Finish(&moved));
    CHECK(gpu->Present()!=FlatPresent::Failed);
    CHECK(render_resident(resident_image) && resident_image==moved && resident_gpu->cache_stats().uploads==2);
    CHECK(moved!=first); CHECK(pixel_is(moved,64,48,32,flat.draws[0].ambient_color));
    frame.frame_id=3; frame.camera.view=identity; frame.meshes.clear(); CHECK(scene.Publish(frame));
    CHECK(BuildFlatFrame(*scene.current(),flat)); CHECK(gpu->Render(flat,&no_bed));
    CHECK(render_resident(resident_image) && resident_image==no_bed && resident_gpu->cache_stats().uploads==2);
    CHECK(pixel_is(no_bed,64,32,32,flat.draws[0].ambient_color));
    CHECK(no_bed!=first);
    frame.frame_id=4; frame.bsp.clear(); CHECK(scene.Publish(frame));
    CHECK(BuildFlatFrame(*scene.current(),flat)); CHECK(gpu->Render(flat,&empty));
    CHECK(render_resident(resident_image) && resident_image==empty && resident_gpu->cache_stats().uploads==2);
    for (std::size_t i=0;i<empty.size();i+=4)
        CHECK(empty[i]==22 && empty[i+1]==24 && empty[i+2]==28 && empty[i+3]==255);
    frame.frame_id=5; frame.camera.width=80; frame.camera.height=40; CHECK(scene.Publish(frame));
    CHECK(BuildFlatFrame(*scene.current(),flat)); CHECK(gpu->Render(flat,&resized)); CHECK(resized.size()==80*40*4);
    const auto reset=scene.BeginReset(); CHECK(reset && !scene.current()); CHECK(scene.EndReset(reset,true));
    frame=input(scene.epochs(),6); CHECK(scene.Publish(frame)); CHECK(BuildFlatFrame(*scene.current(),flat));
    std::vector<std::uint8_t> after_reset; CHECK(gpu->Render(flat,&after_reset)); CHECK(after_reset==first);
    CHECK(render_resident(resident_image) && resident_image==first && resident_gpu->cache_stats().uploads==4);
    frame.frame_id=7;frame.meshes[0].transform[12]=0.2f;CHECK(scene.Publish(frame));
    CHECK(BuildFlatFrame(*scene.current(),flat));std::vector<std::uint8_t> shifted;
    CHECK(gpu->Render(flat,&shifted));CHECK(render_resident(resident_image) && resident_image==shifted && shifted!=first);
    CHECK(resident_gpu->cache_stats().uploads==4);
    // Even if no reader observes the empty interval, a loss in the same
    // map/device epoch breaks continuity and retires the old GPU buffers.
    CHECK(scene.DiscardFrame(frame.map_epoch,frame.device_epoch,frame.frame_id));
    frame.frame_id=8;CHECK(scene.Publish(frame));CHECK(render_resident(resident_image) && resident_image==shifted);
    CHECK(resident_gpu->cache_stats().uploads==6 && resident_gpu->cache_stats().resources==2);
    // A one-resource allocation budget must evict unused geometry and reload
    // it when it becomes visible again.
    auto limited=FlatD3D12::Create(nullptr,one_resource_budget);CHECK(limited);
    auto limited_render=[&] {
        ResidentFlatFrame resident;
        return BuildResidentFlatFrame(*scene.current(),resident) && limited->PrepareResident(resident) &&
               limited->Submit() && limited->Finish() && limited->Present()!=FlatPresent::Failed;
    };
    frame=input(scene.epochs(),9);frame.bsp.clear();CHECK(scene.Publish(frame));CHECK(limited_render());
    frame=input(scene.epochs(),10);frame.meshes.clear();CHECK(scene.Publish(frame));CHECK(limited_render());
    frame=input(scene.epochs(),11);frame.bsp.clear();CHECK(scene.Publish(frame));CHECK(limited_render());
    CHECK(limited->cache_stats().uploads==3 && limited->cache_stats().evictions==2 &&
          limited->cache_stats().resources==1 && limited->cache_stats().bytes==one_resource_budget);
    ResidentFlatFrame prepared;CHECK(BuildResidentFlatFrame(*scene.current(),prepared));
    CHECK(limited->PrepareResident(prepared));CHECK(!limited->RetireResidents());limited->Cancel();
    CHECK(limited->RetireResidents() && limited->cache_stats().resources==0 && limited->cache_stats().bytes==0);
    frame=input(scene.epochs(),12);CHECK(scene.Publish(frame));CHECK(BuildResidentFlatFrame(*scene.current(),prepared));
    CHECK(!limited->PrepareResident(prepared)); // both selected resources cannot fit; neither is evictable
    auto override_gpu=FlatD3D12::Create(nullptr);CHECK(override_gpu);
    auto original=scene.current();
    auto high=std::make_shared<const Resource>(quad("mesh:high",ResourceKind::StaticMesh,0.55f,0.2f,"mat:high"));
    MeshOverride rule{"fixture",fingerprint,"actor:bed",original->meshes[0].resource,high};
    auto render_override=[&](const MeshOverride* selected,std::vector<std::uint8_t>& pixels) {
        ResidentFlatFrame current;
        return BuildResidentFlatFrame(*scene.current(),current,selected) && override_gpu->PrepareResident(current,true) &&
               override_gpu->Submit() && override_gpu->Finish(&pixels);
    };
    std::vector<std::uint8_t> baseline,remastered,restored,changed;
    CHECK(render_override(nullptr,baseline) && baseline==first);
    CHECK(render_override(&rule,remastered) && remastered!=baseline && override_gpu->cache_stats().uploads==3);
    CHECK(render_override(nullptr,restored) && restored==baseline && override_gpu->cache_stats().uploads==3);
    frame.frame_id=13;frame.meshes[0].transform[12]=0.2f;CHECK(scene.Publish(frame));
    CHECK(render_override(&rule,changed) && changed!=remastered && override_gpu->cache_stats().uploads==3);
    frame.frame_id=14;frame.meshes.clear();CHECK(scene.Publish(frame));
    CHECK(render_override(&rule,changed) && changed==no_bed);
    const auto override_reset=scene.BeginReset();CHECK(override_reset && scene.EndReset(override_reset,true));
    frame=input(scene.epochs(),15);CHECK(scene.Publish(frame));
    CHECK(render_override(&rule,changed) && changed==remastered && override_gpu->cache_stats().uploads==5);
    std::cout << "scene_live_dx12_fixture=ok mode=" << (window ? "swapchain" : "offscreen")
              << " camera=changed visibility=changed empty=cleared resize=ok reset=ok resident_cache=ok\n";
    return true;
}
struct ViewerFixture {
    SceneResidency scene;
    std::atomic<bool> replace_before_submit{false};
    std::atomic<bool> replace_after_submit{false};
    std::atomic<bool> discard_before_submit{false};
    std::atomic<bool> discard_after_submit{false};
    FrameInput replacement;
};
std::shared_ptr<const Snapshot> viewer_current(void* context) noexcept {
    try { return static_cast<ViewerFixture*>(context)->scene.current(); } catch (...) { return {}; }
}
bool viewer_submit(void* context, const std::shared_ptr<const Snapshot>& snapshot,
                   SubmitOperation submit, void* argument) noexcept {
    try {
        auto& fixture=*static_cast<ViewerFixture*>(context);
        if (fixture.discard_before_submit.exchange(false))
            (void)fixture.scene.DiscardFrame(snapshot->map_epoch,snapshot->device_epoch,snapshot->frame_id);
        if (fixture.replace_before_submit.exchange(false)) (void)fixture.scene.Publish(fixture.replacement);
        const bool submitted=fixture.scene.SubmitIfCurrent(snapshot,submit,argument);
        if (submitted && fixture.discard_after_submit.exchange(false))
            (void)fixture.scene.DiscardFrame(snapshot->map_epoch,snapshot->device_epoch,snapshot->frame_id);
        if (submitted && fixture.replace_after_submit.exchange(false)) (void)fixture.scene.Publish(fixture.replacement);
        return submitted;
    } catch (...) { return false; }
}
bool await_viewer(SceneViewer& viewer, std::uint64_t frames, std::uint64_t cleared = 0) {
    const auto end=GetTickCount64()+20000;
    while (GetTickCount64()<end) {
        const auto status=viewer.stats();
        if (status.failed || status.stopped) return false;
        if (status.initialized && status.frames>=frames && status.cleared>=cleared) return true;
        Sleep(10);
    }
    return false;
}
bool await_retirement(SceneViewer& viewer, std::uint64_t dropped, std::uint64_t retired) {
    const auto end=GetTickCount64()+20000;
    while (GetTickCount64()<end) {
        const auto status=viewer.stats();
        if (status.failed || status.stopped) return false;
        if (status.dropped>=dropped && status.resident_retirements>=retired) return true;
        Sleep(10);
    }
    return false;
}
bool viewer_pipeline(const std::wstring& diagnostic_directory) {
    auto fixture=std::make_shared<ViewerFixture>(); auto& scene=fixture->scene; CHECK(scene.BeginMap(0,"fixture",fingerprint));
    auto epoch=scene.epochs(); CHECK(scene.AddMaterial(epoch.map,{"mat:bed","bed"}));
    CHECK(scene.AddMaterial(epoch.map,{"mat:wall","wall"}));
    CHECK(scene.AddResource(epoch.map,quad("mesh:bed",ResourceKind::StaticMesh,0.3f,0.2f,"mat:bed")));
    CHECK(scene.AddResource(epoch.map,quad("bsp:wall",ResourceKind::BspBlock,0.9f,0.8f,"mat:wall")));
    ViewerProvider provider; provider.context=fixture; provider.current=viewer_current; provider.submit_if_current=viewer_submit;
    provider.diagnostic_directory=diagnostic_directory;
    provider.diagnostic_session_id="synthetic:"+std::to_string(GetCurrentProcessId());
    auto viewer=SceneViewer::Start(std::move(provider),false); CHECK(viewer && await_viewer(*viewer,0));
    auto frame=input(epoch,1); fixture->discard_before_submit.store(true);
    CHECK(scene.Publish(frame)); viewer->Notify(); CHECK(await_retirement(*viewer,1,1));
    frame.frame_id=2; fixture->discard_after_submit.store(true);
    CHECK(scene.Publish(frame)); viewer->Notify(); CHECK(await_retirement(*viewer,2,2));
    CHECK(viewer->stats().frames==0); // neither invalidated frame was displayed
    frame.frame_id=3; CHECK(scene.Publish(frame)); viewer->Notify(); CHECK(await_viewer(*viewer,1));
    frame.frame_id=4; frame.camera.view[12]=0.5f; frame.meshes.clear();
    fixture->replacement=frame; fixture->replacement.frame_id=5; fixture->replace_before_submit.store(true);
    CHECK(scene.Publish(frame)); viewer->Notify(); CHECK(await_viewer(*viewer,2));
    auto status=viewer->stats(); CHECK(status.frame_id==5 && status.dropped==3 &&
                                      status.map_epoch==epoch.map && status.device_epoch==epoch.device);
    const auto reset=scene.BeginReset(); CHECK(reset); viewer->Notify(); CHECK(await_viewer(*viewer,2,1));
    CHECK(scene.EndReset(reset,true)); epoch=scene.epochs();
    frame=input(epoch,6);fixture->replacement=frame;fixture->replacement.frame_id=7;
    fixture->replacement.camera.view[12]=0.25f;fixture->replace_after_submit.store(true);
    CHECK(scene.Publish(frame)); viewer->Notify(); CHECK(await_viewer(*viewer,4,1));
    status=viewer->stats(); CHECK(status.frame_id==7 && status.device_epoch==epoch.device);
    viewer->Stop();
    const auto deadline=GetTickCount64()+20000;
    while (!viewer->stats().stopped && GetTickCount64()<deadline) Sleep(10);
    CHECK(viewer->stats().stopped && !viewer->stats().failed);
    std::cout << "scene_live_viewer_fixture=ok source=synthetic frames=4 stale_drop=ok newer_frame=ok reset_clear=ok invalidated_first_frame_retirement=ok\n";
    return true;
}
bool synthetic_textures() {
    auto library=std::make_shared<BedroomMaterialLibrary>();
    const char* ids[]{"remaster:material:stone_wall_v2","remaster:material:wood_floor_v2"};
    const std::uint8_t colors[][4]={{40,120,200,255},{190,90,30,255}};
    for (unsigned i=0;i<2;++i) {
        auto& material=library->materials[i];material.id=ids[i];
        for (unsigned channel=0;channel<4;++channel) {
            material.source_sha256[channel]=fingerprint;
            material.channels[channel].mips.push_back({1,1,{colors[i][0],colors[i][1],colors[i][2],255}});
        }
    }
    auto bad=std::make_shared<BedroomMaterialLibrary>(*library);bad->materials[0].channels[0].mips[0].rgba[3]=0;
    CHECK(!FlatD3D12::Create(nullptr,128u*1024u*1024u,bad));
    SceneResidency scene;CHECK(scene.BeginMap(0,"fixture",fingerprint));const auto epoch=scene.epochs();
    CHECK(scene.AddMaterial(epoch.map,{"mat:bed","bed"}));CHECK(scene.AddMaterial(epoch.map,{"mat:wall","wall"}));
    CHECK(scene.AddResource(epoch.map,quad("mesh:bed",ResourceKind::StaticMesh,0.3f,0.2f,"mat:bed")));
    CHECK(scene.AddResource(epoch.map,quad("bsp:wall",ResourceKind::BspBlock,0.9f,0.8f,"mat:wall")));
    auto frame=input(epoch,1);CHECK(scene.Publish(frame));
    auto gpu=FlatD3D12::Create(nullptr,128u*1024u*1024u,library);CHECK(gpu);
    auto render=[&](ResidentFlatFrame& resident,std::vector<std::uint8_t>& pixels) {
        return gpu->PrepareResident(resident,true) && gpu->Submit() && gpu->Finish(&pixels);
    };
    ResidentFlatFrame resident;CHECK(BuildResidentFlatFrame(*scene.current(),resident));
    std::vector<std::uint8_t> original,wall,floor,missing,reset_image;
    CHECK(render(resident,original) && gpu->cache_stats().textured_draws==0);
    resident.draws[0].material_id=ids[0];CHECK(render(resident,wall) && gpu->cache_stats().textured_draws==1);
    const float wall_color[]{40.0f/255,120.0f/255,200.0f/255,1};
    if (!pixel_is(wall,64,32,32,wall_color)) {
        const auto at=(32u*64+32)*4;
        std::cerr<<"texture pixel="<<unsigned(wall[at])<<','<<unsigned(wall[at+1])<<','
                 <<unsigned(wall[at+2])<<','<<unsigned(wall[at+3])<<" original="
                 <<unsigned(original[at])<<','<<unsigned(original[at+1])<<','<<unsigned(original[at+2]);
        std::size_t changed=0,colored=0;
        for (std::size_t i=0;i<wall.size();i+=4) {
            if (wall[i]!=original[i] || wall[i+1]!=original[i+1] || wall[i+2]!=original[i+2]) ++changed;
            if (wall[i]==40 && wall[i+1]==120 && wall[i+2]==200) ++colored;
        }
        std::cerr<<" changed="<<changed<<" colored="<<colored<<"\n";
    }
    CHECK(wall!=original && pixel_is(wall,64,32,32,wall_color));
    resident.draws[0].material_id=ids[1];CHECK(render(resident,floor) && gpu->cache_stats().textured_draws==1);
    const float floor_color[]{190.0f/255,90.0f/255,30.0f/255,1};
    CHECK(floor!=wall && pixel_is(floor,64,32,32,floor_color));
    auto no_uv=std::make_shared<Resource>(*resident.draws[0].resource);no_uv->id+="-without-uv";no_uv->geometry.uv0.clear();
    resident.draws[0].resource=no_uv;CHECK(render(resident,missing) && missing==original && !gpu->cache_stats().textured_draws);
    const auto reset=scene.BeginReset();CHECK(reset && scene.EndReset(reset,true));
    frame=input(scene.epochs(),2);CHECK(scene.Publish(frame));CHECK(BuildResidentFlatFrame(*scene.current(),resident));
    resident.draws[0].material_id=ids[0];
    CHECK(render(resident,reset_image) && reset_image==wall && gpu->cache_stats().textured_draws==1);
    auto pattern=std::make_shared<BedroomMaterialLibrary>(*library);
    for (auto& material:pattern->materials) for (auto& channel:material.channels)
        channel.mips={{2,2,{128,128,128,255, 128,128,128,255, 128,128,128,255, 128,128,128,255}},
                      {1,1,{128,128,128,255}}};
    pattern->materials[0].channels[0].mips[0].rgba={255,0,0,255, 0,255,0,255,
                                                   255,0,0,255, 0,255,0,255};
    auto pattern_gpu=FlatD3D12::Create(nullptr,128u*1024u*1024u,pattern);CHECK(pattern_gpu);
    std::vector<std::uint8_t> pattern_image;
    CHECK(pattern_gpu->PrepareResident(resident,true) && pattern_gpu->Submit() && pattern_gpu->Finish(&pattern_image));
    const auto left=(32u*64+27)*4,right=(32u*64+37)*4;
    CHECK(int(pattern_image[left])-int(pattern_image[left+1])>80 &&
          int(pattern_image[right+1])-int(pattern_image[right])>80);
    std::cout<<"scene_live_textures_fixture=ok source=synthetic albedo=sampled uv=varies switch=ok missing_uv=fallback reset=ok\n";
    return true;
}

// Entirely synthetic light, material, camera and geometry. These fixtures do
// not claim any measured TDS light state or runtime tangent convention.
bool synthetic_pbr(HWND window) {
    SceneResidency scene;CHECK(scene.BeginMap(0,"pbr-fixture",fingerprint));
    auto epoch=scene.epochs();CHECK(scene.AddMaterial(epoch.map,{"mat:bed","bed"}));
    auto surface=quad("mesh:bed",ResourceKind::StaticMesh,.8f,.2f,"mat:bed");
    surface.geometry.normals.assign(4,Vec3{0,0,-1});CHECK(scene.AddResource(epoch.map,std::move(surface)));
    auto angled=quad("mesh:angled",ResourceKind::StaticMesh,.4f,.2f,"mat:bed");
    angled.geometry.normals.assign(4,Vec3{.70710678f,0,-.70710678f});CHECK(scene.AddResource(epoch.map,std::move(angled)));
    auto frame=input(epoch,1);frame.bsp.clear();frame.conventions.meters_per_unit=1;
    frame.light_coverage=LightCoverage::Complete;
    VisualLight light;light.id="light:synthetic";light.generation=1;light.binding=BindingEvidence::Verified;
    light.enabled={true,BindingEvidence::Verified};light.kind={LightKind::Point,BindingEvidence::Verified};
    light.position={Vec3{0,0,-1},BindingEvidence::Verified};light.color_linear={Vec3{1,1,1},BindingEvidence::Verified};
    light.intensity={1,BindingEvidence::Verified};light.influence_radius={10,BindingEvidence::Verified};
    light.source_radius_m={.05f,BindingEvidence::Verified};frame.lights={light};CHECK(scene.Publish(frame));
    auto snapshot=scene.current();ResidentFlatFrame resident;CHECK(BuildResidentFlatFrame(*snapshot,resident));
    resident.draws[0].color={.5f,.5f,.5f,1};
    auto gpu=FlatD3D12::Create(window);CHECK(gpu);
    auto mismatch=resident;++mismatch.frame_id;CHECK(!gpu->PrepareLit(*snapshot,mismatch));
    mismatch=resident;mismatch.provenance.session_id="another-session";CHECK(!gpu->PrepareLit(*snapshot,mismatch));
    mismatch=resident;++mismatch.stage;CHECK(!gpu->PrepareLit(*snapshot,mismatch));
    auto wrong_camera=resident;wrong_camera.camera.view[12]+=.1f;CHECK(!gpu->PrepareLit(*snapshot,wrong_camera));
    auto incomplete=*snapshot;incomplete.light_coverage=LightCoverage::Partial;
    CHECK(!gpu->PrepareLit(incomplete,resident));
    incomplete=*snapshot;incomplete.lights[0].intensity.evidence=BindingEvidence::Candidate;
    CHECK(!gpu->PrepareLit(incomplete,resident));
    incomplete=*snapshot;incomplete.provenance.origin=FrameOrigin::LiveProcess;
    CHECK(!gpu->PrepareLit(incomplete,resident,false,true));
    auto render=[&](const Snapshot& current,ResidentFlatFrame& draws,std::vector<std::uint8_t>& pixels) {
        return gpu->PrepareLit(current,draws,true) && gpu->Submit() && gpu->Finish(&pixels) &&
               gpu->Present()!=FlatPresent::Failed;
    };
    std::vector<std::uint8_t> one,high,off,reset_image;
    CHECK(render(*snapshot,resident,one));
    const auto at=(32u*64+32)*4;
    // Independent estimate for normal incidence, inverse-square point light,
    // GGX roughness .7 and ACES/sRGB output: about 107/255 at this pixel.
    CHECK(one[at]>=100 && one[at]<=116 && one[at]==one[at+1] && one[at]==one[at+2]);
    frame.frame_id=2;frame.lights[0].intensity.value=32;CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));resident.draws[0].color={.5f,.5f,.5f,1};
    CHECK(render(*scene.current(),resident,high));
    CHECK(high[at]>245 && high[at]>one[at]); // values above 1 survived in HDR until tonemapping
    frame.frame_id=3;frame.lights[0].enabled.value=false;CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));CHECK(render(*scene.current(),resident,off));
    CHECK(off[at]==0 && off[at+1]==0 && off[at+2]==0);
    const auto reset=scene.BeginReset();CHECK(reset && scene.EndReset(reset,true));
    frame.device_epoch=scene.epochs().device;frame.frame_id=4;frame.lights[0]=light;CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));resident.draws[0].color={.5f,.5f,.5f,1};
    CHECK(render(*scene.current(),resident,reset_image) && reset_image==one && gpu->cache_stats().uploads==2);
    frame.frame_id=5;frame.meshes[0].instance_id="actor:angled";frame.meshes[0].resource_id="mesh:angled";
    frame.meshes[0].transform[0]=2;CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));resident.draws[0].color={.5f,.5f,.5f,1};
    std::vector<std::uint8_t> nonuniform;CHECK(render(*scene.current(),resident,nonuniform));
    // Inverse-transpose normal gives about 98, whereas multiplying the normal
    // by the world matrix would give about 56 for this non-uniform scale.
    CHECK(nonuniform[at]>=90 && nonuniform[at]<=106 && gpu->cache_stats().pbr_draws==1);
    frame.frame_id=6;frame.meshes[0].transform[0]=0;CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));CHECK(render(*scene.current(),resident,nonuniform));
    CHECK(gpu->cache_stats().pbr_draws==0); // singular transforms never reach the normal shader
    frame.frame_id=7;frame.meshes[0].instance_id="actor:bed";frame.meshes[0].resource_id="mesh:bed";
    frame.meshes[0].transform=identity;frame.lights[0].position.value=Vec3{0,0,-2.2f};CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));resident.draws[0].color={.5f,.5f,.5f,1};
    std::vector<std::uint8_t> farther;CHECK(render(*scene.current(),resident,farther));
    CHECK(farther[at]>0 && farther[at]+25<one[at]);
    frame.frame_id=8;frame.lights[0]=light;frame.lights[0].kind={LightKind::Spot,BindingEvidence::Verified};
    frame.lights[0].direction={Vec3{0,0,1},BindingEvidence::Verified};
    frame.lights[0].cos_inner={.9f,BindingEvidence::Verified};frame.lights[0].cos_outer={.7f,BindingEvidence::Verified};
    CHECK(scene.Publish(frame));CHECK(BuildResidentFlatFrame(*scene.current(),resident));resident.draws[0].color={.5f,.5f,.5f,1};
    std::vector<std::uint8_t> spot_on,spot_off;CHECK(render(*scene.current(),resident,spot_on));
    CHECK(std::abs(int(spot_on[at])-int(one[at]))<=1);
    frame.frame_id=9;frame.lights[0].direction.value=Vec3{0,0,-1};CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));CHECK(render(*scene.current(),resident,spot_off));CHECK(spot_off[at]==0);
    frame.frame_id=10;frame.lights[0]=light;frame.camera.width=80;frame.camera.height=40;CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));resident.draws[0].color={.5f,.5f,.5f,1};
    std::vector<std::uint8_t> resized;CHECK(render(*scene.current(),resident,resized) && resized.size()==80u*40u*4u);
    frame.frame_id=11;frame.camera.width=64;frame.camera.height=64;CHECK(scene.Publish(frame));
    CHECK(BuildResidentFlatFrame(*scene.current(),resident));resident.draws[0].color={.5f,.5f,.5f,1};
    CHECK(render(*scene.current(),resident,resized) && resized==one);
    auto worker=std::make_shared<ViewerFixture>();CHECK(worker->scene.BeginMap(0,"worker-fixture",fingerprint));
    const auto worker_epoch=worker->scene.epochs();CHECK(worker->scene.AddMaterial(worker_epoch.map,{"mat:bed","bed"}));
    auto worker_mesh=quad("mesh:bed",ResourceKind::StaticMesh,.8f,.2f,"mat:bed");
    worker_mesh.geometry.normals.assign(4,Vec3{0,0,-1});CHECK(worker->scene.AddResource(worker_epoch.map,std::move(worker_mesh)));
    auto worker_frame=input(worker_epoch,1);worker_frame.bsp.clear();worker_frame.conventions.meters_per_unit=1;
    worker_frame.light_coverage=LightCoverage::Complete;worker_frame.lights={light};CHECK(worker->scene.Publish(worker_frame));
    ViewerProvider provider;provider.context=worker;provider.current=viewer_current;provider.submit_if_current=viewer_submit;
    auto viewer=SceneViewer::Start(std::move(provider),false);CHECK(viewer);viewer->Notify();CHECK(await_viewer(*viewer,1));
    CHECK(viewer->stats().pbr_draws==1 && viewer->stats().shadowed_lights==1);
    worker_frame.frame_id=2;worker_frame.light_coverage=LightCoverage::Partial;CHECK(worker->scene.Publish(worker_frame));
    viewer->Notify();CHECK(await_viewer(*viewer,2));CHECK(viewer->stats().pbr_draws==0 && viewer->stats().shadowed_lights==0);
    viewer->Stop();const auto deadline=GetTickCount64()+20000;
    while (!viewer->stats().stopped && GetTickCount64()<deadline) Sleep(10);
    CHECK(viewer->stats().stopped && !viewer->stats().failed);
    std::cout<<"scene_live_pbr_fixture=ok source=synthetic ggx=ok hdr=ok normal_inverse_transpose=ok scope=ok disabled=dark reset=ok\n";
    return true;
}

bool synthetic_pbr_materials() {
    auto library=[](std::uint8_t roughness,const std::array<std::uint8_t,4>& normal) {
        auto out=std::make_shared<BedroomMaterialLibrary>();
        const char* ids[]{"remaster:material:stone_wall_v2","remaster:material:wood_floor_v2"};
        for (unsigned i=0;i<2;++i) {
            auto& m=out->materials[i];m.id=ids[i];
            const std::array<std::array<std::uint8_t,4>,4> channels{{{128,128,128,255},normal,
                                                                      {roughness,roughness,roughness,255},{255,255,255,255}}};
            for (unsigned channel=0;channel<4;++channel) {
                m.source_sha256[channel]=fingerprint;
                m.channels[channel].mips.push_back({1,1,std::vector<std::uint8_t>(channels[channel].begin(),channels[channel].end())});
            }
        }
        return out;
    };
    SceneResidency scene;CHECK(scene.BeginMap(0,"material-fixture",fingerprint));const auto epoch=scene.epochs();
    CHECK(scene.AddMaterial(epoch.map,{"mat:bed","bed"}));
    auto mesh=quad("mesh:bed",ResourceKind::StaticMesh,.8f,.2f,"mat:bed");
    mesh.geometry.normals.assign(4,Vec3{0,0,-1});CHECK(scene.AddResource(epoch.map,std::move(mesh)));
    auto frame=input(epoch,1);frame.bsp.clear();frame.conventions.meters_per_unit=1;frame.light_coverage=LightCoverage::Complete;
    VisualLight light;light.id="light:synthetic";light.generation=1;light.binding=BindingEvidence::Verified;
    light.enabled={true,BindingEvidence::Verified};light.kind={LightKind::Point,BindingEvidence::Verified};
    light.position={Vec3{0,0,-1},BindingEvidence::Verified};light.color_linear={Vec3{1,1,1},BindingEvidence::Verified};
    light.intensity={1,BindingEvidence::Verified};light.influence_radius={10,BindingEvidence::Verified};
    light.source_radius_m={.05f,BindingEvidence::Verified};frame.lights={light};CHECK(scene.Publish(frame));
    ResidentFlatFrame resident;CHECK(BuildResidentFlatFrame(*scene.current(),resident));
    resident.draws[0].material_id="remaster:material:stone_wall_v2";
    auto a=FlatD3D12::Create(nullptr,128u*1024u*1024u,library(179,{128,128,255,255}));
    auto b=FlatD3D12::Create(nullptr,128u*1024u*1024u,library(179,{218,128,218,255}));
    auto glossy=FlatD3D12::Create(nullptr,128u*1024u*1024u,library(51,{128,128,255,255}));
    CHECK(a && b && glossy);
    auto render=[&](FlatD3D12& gpu,bool normal,std::vector<std::uint8_t>& image) {
        return gpu.PrepareLit(*scene.current(),resident,true,normal) && gpu.Submit() && gpu.Finish(&image) &&
               gpu.cache_stats().textured_draws==1 && gpu.cache_stats().pbr_draws==1;
    };
    std::vector<std::uint8_t> base,unknown_normal,mapped,low_roughness;
    CHECK(render(*a,false,base) && render(*b,false,unknown_normal) && base==unknown_normal);
    CHECK(render(*b,true,mapped) && mapped!=base);
    CHECK(render(*glossy,false,low_roughness));const auto at=(32u*64+32)*4;
    std::cout<<"synthetic_pbr_material_sample="<<unsigned(base[at])<<","<<unsigned(low_roughness[at])<<"\n";
    CHECK(base[at]>=60 && base[at]<=70 && low_roughness[at]>base[at]+40); // sRGB albedo, linear roughness
    std::cout<<"scene_live_pbr_material_fixture=ok source=synthetic srgb=ok roughness=ok normal=fixture_only\n";
    return true;
}

bool synthetic_shadows() {
    SceneResidency scene;CHECK(scene.BeginMap(0,"shadow-fixture",fingerprint));auto epoch=scene.epochs();
    CHECK(scene.AddMaterial(epoch.map,{"mat:bed","bed"}));CHECK(scene.AddMaterial(epoch.map,{"mat:wall","wall"}));
    auto blocker=quad("mesh:bed",ResourceKind::StaticMesh,.15f,.25f,"mat:bed");
    blocker.geometry.normals.assign(4,Vec3{0,0,-1});CHECK(scene.AddResource(epoch.map,std::move(blocker)));
    auto receiver=quad("bsp:wall",ResourceKind::BspBlock,.9f,.8f,"mat:wall");
    receiver.geometry.normals.assign(4,Vec3{0,0,-1});CHECK(scene.AddResource(epoch.map,std::move(receiver)));
    auto frame=input(epoch,1);frame.camera.width=128;frame.camera.height=128;
    frame.conventions.meters_per_unit=1;frame.light_coverage=LightCoverage::Complete;
    VisualLight light;light.id="light:a";light.generation=1;light.binding=BindingEvidence::Verified;
    light.enabled={true,BindingEvidence::Verified};light.kind={LightKind::Point,BindingEvidence::Verified};
    light.position={Vec3{-.8f,0,-.6f},BindingEvidence::Verified};light.color_linear={Vec3{1,1,1},BindingEvidence::Verified};
    light.intensity={2,BindingEvidence::Verified};light.influence_radius={10,BindingEvidence::Verified};
    light.source_radius_m={.01f,BindingEvidence::Verified};frame.lights={light};CHECK(scene.Publish(frame));
    auto gpu=FlatD3D12::Create(nullptr);CHECK(gpu);
    auto prepare=[&](ResidentFlatFrame& resident) {
        if (!BuildResidentFlatFrame(*scene.current(),resident)) return false;
        for (auto& draw:resident.draws) draw.color={.5f,.5f,.5f,1};
        return true;
    };
    auto render=[&](ResidentFlatFrame& resident,bool enabled,std::vector<std::uint8_t>& output) {
        return gpu->PrepareLit(*scene.current(),resident,true,false,enabled) && gpu->Submit() && gpu->Finish(&output);
    };
    ResidentFlatFrame resident;CHECK(prepare(resident));std::vector<std::uint8_t> unshadowed,shadowed,again;
    CHECK(render(resident,false,unshadowed));CHECK(!gpu->cache_stats().shadowed_lights);
    CHECK(render(resident,true,shadowed));CHECK(gpu->cache_stats().shadowed_lights==1 && gpu->cache_stats().shadow_draws==12);
    const auto shaded=(64u*128+96)*4, lit=(64u*128+32)*4;
    std::cout<<"synthetic_shadow_samples="<<unsigned(unshadowed[shaded])<<","<<unsigned(shadowed[shaded])
             <<" lit="<<unsigned(unshadowed[lit])<<","<<unsigned(shadowed[lit])<<"\n";
    CHECK(int(unshadowed[shaded])-int(shadowed[shaded])>20);
    CHECK(std::abs(int(unshadowed[lit])-int(shadowed[lit]))<=3);
    CHECK(gpu->PrepareLit(*scene.current(),resident,true));gpu->Cancel();
    CHECK(render(resident,true,again) && again==shadowed);
    frame.frame_id=2;frame.meshes.clear();CHECK(scene.Publish(frame));CHECK(prepare(resident));
    std::vector<std::uint8_t> hidden;CHECK(render(resident,true,hidden));
    CHECK(int(hidden[shaded])-int(shadowed[shaded])>20 && gpu->cache_stats().uploads==2);
    frame.frame_id=3;frame.meshes=input(epoch,1).meshes;frame.meshes[0].transform[12]=-.25f;
    CHECK(scene.Publish(frame));CHECK(prepare(resident));std::vector<std::uint8_t> moved;
    CHECK(render(resident,true,moved) && moved!=shadowed);
    CHECK(int(moved[shaded])-int(shadowed[shaded])>15);
    frame.frame_id=4;frame.meshes[0].transform=identity;frame.lights[0].source_radius_m.value=.8f;
    CHECK(scene.Publish(frame));CHECK(prepare(resident));std::vector<std::uint8_t> soft,soft_clear;
    CHECK(render(resident,true,soft) && soft!=shadowed && render(resident,false,soft_clear));
    frame.frame_id=5;frame.meshes[0].transform[12]=.33143f;frame.meshes[0].transform[14]=.35f;
    CHECK(scene.Publish(frame));CHECK(prepare(resident));std::vector<std::uint8_t> contact,contact_clear;
    CHECK(render(resident,true,contact) && render(resident,false,contact_clear));
    auto penumbra=[](const std::vector<std::uint8_t>& clear,const std::vector<std::uint8_t>& shaded_image) {
        unsigned count=0;
        for (unsigned x=98;x<120;++x) {
            const auto i=(64u*128+x)*4;const int reference=clear[i], value=shaded_image[i];
            if (reference>15 && value*10>reference && value*10<reference*9) ++count;
        }
        return count;
    };
    const auto far_penumbra=penumbra(soft_clear,soft),near_penumbra=penumbra(contact_clear,contact);
    std::cout<<"synthetic_shadow_penumbra="<<far_penumbra<<","<<near_penumbra<<"\n";
    CHECK(far_penumbra>=near_penumbra+2);
    frame.frame_id=6;frame.meshes[0].transform=identity;frame.lights[0]=light;auto second=light;second.id="light:b";
    second.position.value=Vec3{.8f,0,-.6f};frame.lights.push_back(second);
    CHECK(scene.Publish(frame));CHECK(prepare(resident));std::vector<std::uint8_t> two;
    CHECK(render(resident,true,two) && gpu->cache_stats().shadowed_lights==2 && gpu->cache_stats().shadow_draws==24);
    CHECK(int(two[shaded])-int(shadowed[shaded])>20);
    auto third=light;third.id="light:c";auto fourth=light;fourth.id="light:d";
    auto fifth=second;fifth.id="light:e";
    frame.frame_id=7;frame.lights.push_back(third);frame.lights.push_back(fourth);frame.lights.push_back(fifth);
    CHECK(scene.Publish(frame));CHECK(prepare(resident));std::vector<std::uint8_t> five;
    CHECK(render(resident,true,five) && gpu->cache_stats().shadowed_lights==4 && gpu->cache_stats().shadow_draws==48);
    CHECK(five[shaded]>two[shaded]); // the fifth light still contributes, without a shadow map
    frame.frame_id=8;CHECK(scene.Publish(frame));CHECK(prepare(resident));
    const auto draw=resident.draws[0];resident.draws.assign(700,draw);
    CHECK(render(resident,true,two) && gpu->cache_stats().shadow_budget_exceeded &&
          gpu->cache_stats().shadowed_lights==0 && gpu->cache_stats().shadow_draws==0);
    frame.frame_id=9;frame.lights={light};frame.lights[0].kind={LightKind::Spot,BindingEvidence::Verified};
    frame.lights[0].direction={Vec3{.68045f,0,.73279f},BindingEvidence::Verified};
    frame.lights[0].cos_inner={.9f,BindingEvidence::Verified};frame.lights[0].cos_outer={.7f,BindingEvidence::Verified};
    CHECK(scene.Publish(frame));CHECK(prepare(resident));std::vector<std::uint8_t> spot,spot_clear;
    CHECK(render(resident,true,spot) && render(resident,false,spot_clear));
    CHECK(int(spot_clear[shaded])-int(spot[shaded])>20);
    const auto ticket=scene.BeginReset();CHECK(ticket && scene.EndReset(ticket,true));epoch=scene.epochs();
    frame=input(epoch,10);frame.camera.width=128;frame.camera.height=128;
    frame.conventions.meters_per_unit=1;frame.light_coverage=LightCoverage::Complete;frame.lights={light};
    CHECK(scene.Publish(frame));CHECK(prepare(resident));CHECK(render(resident,true,again) && again==shadowed);
    CHECK(gpu->cache_stats().uploads==4);
    for (auto& part:resident.draws) {
        auto no_normals=std::make_shared<Resource>(*part.resource);no_normals->id+="-without-normals";
        no_normals->geometry.normals.clear();part.resource=std::move(no_normals);
    }
    CHECK(render(resident,true,again) && !gpu->cache_stats().pbr_draws && !gpu->cache_stats().shadowed_lights &&
          !gpu->cache_stats().shadow_draws);
    std::cout<<"scene_live_shadow_fixture=ok source=synthetic blocker=ok soft=ok visibility=ok motion=ok budget=ok reset=ok\n";
    return true;
}
struct CatalogViewerFixture { std::shared_ptr<SceneResidency> scene; };
std::shared_ptr<const Snapshot> catalog_viewer_current(void* context) noexcept {
    try {return static_cast<CatalogViewerFixture*>(context)->scene->current();} catch (...) {return {};}
}
bool catalog_viewer_submit(void* context,const std::shared_ptr<const Snapshot>& snapshot,
                           SubmitOperation operation,void* argument) noexcept {
    return static_cast<CatalogViewerFixture*>(context)->scene->SubmitIfCurrent(snapshot,operation,argument);
}
// The catalog contains real offline assets, but the camera, selection and
// transforms below are synthetic and do not constitute a TDS observation.
bool catalog_override(const wchar_t* path) {
    LoadedCatalog catalog; CHECK(LoadCatalogFile(path,catalog));
    CHECK(catalog.bed_override && catalog.binding.bed && catalog.residency);
    const auto& original=catalog.binding.bed->geometry;
    CHECK(!original.positions.empty() && !original.sections.empty());
    Vec3 low=original.positions[0], high=low;
    for (const auto& position:original.positions) for (unsigned axis=0;axis<3;++axis) {
        low[axis]=std::min(low[axis],position[axis]);high[axis]=std::max(high[axis],position[axis]);
    }
    const float dx=high[0]-low[0],dy=high[1]-low[1],dz=high[2]-low[2];
    CHECK(dx>0 && dy>0 && dz>0);
    FrameInput frame;auto& scene=*catalog.residency;const auto epoch=scene.epochs();
    frame.map_epoch=epoch.map;frame.device_epoch=epoch.device;frame.frame_id=1;
    frame.provenance.origin=FrameOrigin::Synthetic;frame.provenance.session_id="offline-catalog-gpu-fixture";
    frame.synchronized=true;frame.conventions=catalog.conventions;
    frame.camera.width=128;frame.camera.height=128;frame.camera.view=identity;
    // Orthographic X/Z view, looking along Y, with room for movement.
    frame.camera.projection={1.0f/dx,0,0,0, 0,0,0.8f/dy,0, 0,1.0f/dz,0,0,
                             -(low[0]+high[0])/(2*dx),-(low[2]+high[2])/(2*dz),
                             0.1f-0.8f*low[1]/dy,1};
    MeshSelection mesh;mesh.instance_id=catalog.binding.bed_instance_id;mesh.resource_id=catalog.binding.bed->id;
    mesh.generation=1;mesh.binding=BindingEvidence::Candidate;mesh.transform=identity;
    mesh.material_ids=catalog.binding.bed_materials;
    for (std::uint32_t i=0;i<original.sections.size();++i) mesh.sections.push_back(i);
    frame.meshes.push_back(mesh);CHECK(scene.Publish(frame));
    const std::string override_prefix="remaster:material:bed_high:";
    for (const auto& item:scene.current()->materials)
        CHECK(item.first.compare(0,override_prefix.size(),override_prefix)!=0);
    auto gpu=FlatD3D12::Create(nullptr,128u*1024u*1024u,catalog.bedroom_materials);CHECK(gpu);
    auto render=[&](const MeshOverride* rule,std::vector<std::uint8_t>& pixels,std::uint32_t overrides) {
        ResidentFlatFrame resident;
        return BuildResidentFlatFrame(*scene.current(),resident,rule) && resident.override_instances==overrides &&
               gpu->PrepareResident(resident,true) && gpu->Submit() && gpu->Finish(&pixels);
    };
    std::vector<std::uint8_t> source,replaced,restored,moved,empty,after_reset;
    CHECK(render(nullptr,source,0));
    if (catalog.bedroom_materials) {
        ResidentFlatFrame textured;CHECK(BuildResidentFlatFrame(*scene.current(),textured));
        for (auto& draw:textured.draws) draw.material_id=catalog.bedroom_materials->materials[0].id;
        std::vector<std::uint8_t> pixels;
        CHECK(gpu->PrepareResident(textured,true) && gpu->Submit() && gpu->Finish(&pixels));
        CHECK(gpu->cache_stats().textured_draws==textured.draws.size() && pixels!=source);
    }
    CHECK(render(catalog.bed_override.get(),replaced,1) && replaced!=source);
    CHECK(render(nullptr,restored,0) && restored==source && gpu->cache_stats().uploads==2);
    frame.frame_id=2;frame.meshes[0].transform[12]=dx*0.25f;CHECK(scene.Publish(frame));
    CHECK(render(catalog.bed_override.get(),moved,1) && moved!=replaced && gpu->cache_stats().uploads==2);
    frame.frame_id=3;frame.meshes.clear();CHECK(scene.Publish(frame));
    CHECK(render(catalog.bed_override.get(),empty,0));
    CHECK(empty!=source && empty!=replaced);
    const auto reset=scene.BeginReset();CHECK(reset && scene.EndReset(reset,true));
    frame.frame_id=4;frame.device_epoch=scene.epochs().device;frame.meshes.push_back(mesh);CHECK(scene.Publish(frame));
    CHECK(render(catalog.bed_override.get(),after_reset,1) && after_reset==replaced && gpu->cache_stats().uploads==3);
    if (catalog.bedroom_materials) {
        // Offline catalog geometry with a synthetic orthographic camera and
        // selection; the anchor uses the previously observed bed transform.
        frame.frame_id=5;frame.meshes[0].transform[12]=3398;frame.meshes[0].transform[13]=1514;
        frame.meshes[0].transform[14]=272;
        frame.camera.projection={.005f,0,0,0, 0,.005f,0,0, 0,0,.002f,0, -17.5f,-7.7f,-.4f,1};
        for (const auto& block:catalog.binding.bsp) {
            BspSelection selection;selection.resource_id=block->id;selection.binding=BindingEvidence::Candidate;
            for (std::uint32_t i=0;i<block->geometry.sections.size();++i) selection.sections.push_back(i);
            frame.bsp.push_back(std::move(selection));
        }
        CHECK(scene.Publish(frame));
        ResidentFlatFrame plain,remaster;
        CHECK(BuildResidentFlatFrame(*scene.current(),plain));
        CHECK(BuildResidentFlatFrame(*scene.current(),remaster,nullptr,true) && remaster.bedroom_material_runs>0);
        std::vector<std::uint8_t> before,with_materials;
        CHECK(gpu->PrepareResident(plain,true) && gpu->Submit() && gpu->Finish(&before));
        CHECK(gpu->PrepareResident(remaster,true) && gpu->Submit() && gpu->Finish(&with_materials));
        CHECK(gpu->cache_stats().textured_draws==remaster.bedroom_material_runs && before!=with_materials);
        std::cout<<"scene_live_bedroom_materials_fixture=ok source=offline camera=synthetic runs="
                 <<remaster.bedroom_material_runs<<"\n";

        auto context=std::make_shared<CatalogViewerFixture>();context->scene=catalog.residency;
        ViewerProvider provider;provider.context=context;provider.current=catalog_viewer_current;
        provider.submit_if_current=catalog_viewer_submit;provider.bed_override=catalog.bed_override;
        provider.bedroom_materials=catalog.bedroom_materials;provider.bedroom_material_ids=true;
        auto viewer=SceneViewer::Start(std::move(provider),false);CHECK(viewer);
        viewer->Notify();CHECK(await_viewer(*viewer,1));
        auto stats=viewer->stats();CHECK(stats.frame_id==5 && stats.bedroom_material_runs>0 &&
                                         stats.textured_draws==stats.bedroom_material_runs);
        auto hidden=frame;hidden.frame_id=6;hidden.meshes.clear();CHECK(scene.Publish(hidden));
        viewer->Notify();CHECK(await_viewer(*viewer,2));stats=viewer->stats();
        CHECK(stats.frame_id==6 && !stats.bedroom_material_runs && !stats.textured_draws);
        const auto ticket=scene.BeginReset();CHECK(ticket);viewer->Notify();CHECK(await_viewer(*viewer,2,1));
        CHECK(scene.EndReset(ticket,true));frame.frame_id=7;frame.device_epoch=scene.epochs().device;CHECK(scene.Publish(frame));
        viewer->Notify();CHECK(await_viewer(*viewer,3,1));stats=viewer->stats();
        CHECK(stats.frame_id==7 && stats.bedroom_material_runs>0 && stats.textured_draws==stats.bedroom_material_runs);
        viewer->Stop();const auto deadline=GetTickCount64()+20000;
        while (!viewer->stats().stopped && GetTickCount64()<deadline) Sleep(10);
        CHECK(viewer->stats().stopped && !viewer->stats().failed);
        std::cout<<"scene_live_material_viewer_fixture=ok source=offline visibility=changed reset=ok\n";
    }
    if (!catalog.binding.static_mesh_actors.empty()) {
        const auto actor_id="actor:"+catalog.binding.map+":"+catalog.binding.map_sha256+":StaticMeshActor__258";
        const auto found=catalog.binding.static_mesh_actors.find(actor_id);
        CHECK(found!=catalog.binding.static_mesh_actors.end() && found->second.resource->name=="GENrafters1");
        const auto& candidate=found->second;const auto& geometry=candidate.resource->geometry;
        Vec3 minimum=geometry.positions[0],maximum=minimum;
        for (const auto& position:geometry.positions) for (unsigned axis=0;axis<3;++axis) {
            minimum[axis]=std::min(minimum[axis],position[axis]);maximum[axis]=std::max(maximum[axis],position[axis]);
        }
        const float sx=maximum[0]-minimum[0],sy=maximum[1]-minimum[1],sz=maximum[2]-minimum[2];
        CHECK(sx>0 && sy>0 && sz>0);
        FrameInput generic;generic.map_epoch=epoch.map;generic.device_epoch=scene.epochs().device;generic.frame_id=8;
        generic.provenance=frame.provenance;generic.synchronized=true;generic.conventions=catalog.conventions;
        generic.camera.width=128;generic.camera.height=128;generic.camera.view=identity;
        generic.camera.projection={1.0f/sx,0,0,0, 0,0,.8f/sy,0, 0,1.0f/sz,0,0,
                                   -(minimum[0]+maximum[0])/(2*sx),-(minimum[2]+maximum[2])/(2*sz),
                                   .1f-.8f*minimum[1]/sy,1};
        MeshSelection selection;selection.instance_id=actor_id;selection.resource_id=candidate.resource->id;
        selection.matched_resource=candidate.resource;selection.generation=1;selection.binding=BindingEvidence::Candidate;
        selection.identity_scope=IdentityScope::VisibleList;selection.material_ids=candidate.material_ids;selection.transform=identity;
        for (std::uint32_t i=0;i<geometry.sections.size();++i) selection.sections.push_back(i);
        generic.meshes.push_back(selection);CHECK(scene.Publish(generic));
        auto render_generic=[&](std::vector<std::uint8_t>& image) {
            ResidentFlatFrame draw;
            return BuildResidentFlatFrame(*scene.current(),draw) && gpu->PrepareResident(draw,true) && gpu->Submit() && gpu->Finish(&image);
        };
        std::vector<std::uint8_t> original_generic,moved_generic,hidden_generic,reset_generic;
        CHECK(render_generic(original_generic));
        generic.frame_id=9;generic.meshes[0].transform[12]=sx*.3f;CHECK(scene.Publish(generic));
        CHECK(render_generic(moved_generic) && moved_generic!=original_generic);
        generic.frame_id=10;generic.meshes.clear();CHECK(scene.Publish(generic));
        CHECK(render_generic(hidden_generic) && hidden_generic!=original_generic);
        const auto ticket=scene.BeginReset();CHECK(ticket && scene.EndReset(ticket,true));
        generic.frame_id=11;generic.device_epoch=scene.epochs().device;generic.meshes.push_back(selection);CHECK(scene.Publish(generic));
        CHECK(render_generic(reset_generic) && reset_generic==original_generic);
        std::cout<<"scene_live_static_catalog_fixture=ok source=offline actor=StaticMeshActor__258"
                 <<" mesh=GENrafters1 camera=synthetic movement=ok visibility=ok reset=ok candidates="
                 <<catalog.binding.static_mesh_actors.size()<<"\n";
    }
    std::cout << "scene_live_catalog_dx12_fixture=ok source=offline camera=synthetic original_override_restore=ok"
              << " movement=ok visibility=ok reset=ok vertices=" << catalog.bed_override->replacement->geometry.positions.size()
              << " indices=" << catalog.bed_override->replacement->geometry.indices.size()
              << " materials=" << (catalog.bedroom_materials ? "offline" : "none") << "\n";
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    bool window=false;std::wstring diagnostic_directory,catalog_path;
    for (int i=1;i<argc;++i) {
        if (std::wstring(argv[i])==L"--window" && !window) window=true;
        else if (std::wstring(argv[i])==L"--diagnostics" && i+1<argc && diagnostic_directory.empty())
            diagnostic_directory=argv[++i];
        else if (std::wstring(argv[i])==L"--catalog" && i+1<argc && catalog_path.empty())
            catalog_path=argv[++i];
        else return 2;
    }
    HWND hwnd=nullptr;
    if (window) {
        WNDCLASSW cls{}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=GetModuleHandleW(nullptr);
        cls.lpszClassName=L"T3RenderLiveDx12Fixture";
        if (!RegisterClassW(&cls)) return 2;
        hwnd=CreateWindowExW(0,cls.lpszClassName,L"T3Render fixture",WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT,CW_USEDEFAULT,128,128,nullptr,nullptr,cls.hInstance,nullptr);
        if (!hwnd) return 2;
    }
    const bool ok=run(hwnd) && viewer_pipeline(diagnostic_directory) && synthetic_textures() && synthetic_pbr(hwnd) &&
                  synthetic_pbr_materials() && synthetic_shadows() &&
                  (catalog_path.empty() || catalog_override(catalog_path.c_str()));
    if (hwnd) DestroyWindow(hwnd);
    return ok ? 0 : 1;
}
