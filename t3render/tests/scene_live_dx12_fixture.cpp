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
    const bool ok=run(hwnd) && viewer_pipeline(diagnostic_directory) && synthetic_textures() &&
                  (catalog_path.empty() || catalog_override(catalog_path.c_str()));
    if (hwnd) DestroyWindow(hwnd);
    return ok ? 0 : 1;
}
