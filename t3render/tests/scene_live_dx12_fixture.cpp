#include "../src/scene_live_dx12.hpp"
#include "../src/scene_live_flat.hpp"
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
} // namespace

int wmain(int argc, wchar_t** argv) {
    bool window=false;std::wstring diagnostic_directory;
    for (int i=1;i<argc;++i) {
        if (std::wstring(argv[i])==L"--window" && !window) window=true;
        else if (std::wstring(argv[i])==L"--diagnostics" && i+1<argc && diagnostic_directory.empty())
            diagnostic_directory=argv[++i];
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
    const bool ok=run(hwnd) && viewer_pipeline(diagnostic_directory);
    if (hwnd) DestroyWindow(hwnd);
    return ok ? 0 : 1;
}
