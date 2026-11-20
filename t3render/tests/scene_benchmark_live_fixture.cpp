#include "../src/scene_benchmark_win.hpp"
#include "../src/scene_live_viewer.hpp"
#include <windows.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

using namespace t3render::live;
namespace {
struct Fixture { std::mutex mutex;std::shared_ptr<const Snapshot> current; };
std::shared_ptr<const Snapshot> Current(void* p) noexcept {
    try {auto& fixture=*static_cast<Fixture*>(p);std::lock_guard<std::mutex> lock(fixture.mutex);return fixture.current;}
    catch (...) {return {};}
}
bool Submit(void* p,const std::shared_ptr<const Snapshot>& expected,SubmitOperation submit,void* context) noexcept {
    try {auto& fixture=*static_cast<Fixture*>(p);std::lock_guard<std::mutex> lock(fixture.mutex);
        return fixture.current==expected && submit(context);}
    catch (...) {return false;}
}
void Set(Fixture& fixture,std::shared_ptr<const Snapshot> value) {
    std::lock_guard<std::mutex> lock(fixture.mutex);fixture.current=std::move(value);
}
bool Exists(const std::wstring& path) {return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
}

int wmain(int argc,wchar_t** argv) {
    if (argc!=3) return 2;
    BenchmarkPackage package;
    if (!LoadBenchmarkPackage(argv[1],package)) {std::cerr<<"invalid test package\n";return 1;}
    auto context=std::make_shared<Fixture>();
    auto invalid=std::make_shared<Snapshot>(*package.scene.on);
    invalid->map="not-the-bedroom";invalid->frame_id=41;
    invalid->provenance.origin=FrameOrigin::LiveProcess;invalid->provenance.session_id="live:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    Set(*context,invalid);
    ViewerProvider provider;provider.context=context;provider.current=Current;provider.submit_if_current=Submit;
    provider.bed_override=package.bed;provider.bedroom_materials=package.materials;
    provider.benchmark_package_directory=argv[1];provider.benchmark_output_directory=argv[2];
    provider.diagnostic_directory=argv[2];provider.diagnostic_session_id=invalid->provenance.session_id;
    provider.diagnostic_initial_images=true;
    provider.remaster_toggle_path=std::wstring(argv[2])+L"\\fixture-remaster-toggle";
    auto viewer=SceneViewer::Start(std::move(provider),false);
    if (!viewer) return 3;
    viewer->Notify();
    const auto before=GetTickCount64()+20000;
    while (GetTickCount64()<before && !viewer->stats().frames && !viewer->stats().failed) Sleep(10);
    const auto result_dir=std::wstring(argv[2])+L"\\t3bench-live-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    if (!viewer->stats().frames || viewer->stats().failed || Exists(result_dir)) return 4;
    auto valid=std::make_shared<Snapshot>(*invalid);valid->map=package.scene.on->map;valid->frame_id=42;
    valid->camera.view[12]+=50; // The benchmark must ignore this live camera.
    Set(*context,valid);viewer->Notify();
    const auto until=GetTickCount64()+60000;
    while (GetTickCount64()<until && (!Exists(result_dir+L"\\metrics.json") || viewer->stats().frame_id!=42) &&
           !viewer->stats().failed) Sleep(10);
    if (viewer->stats().failed || !Exists(result_dir+L"\\metrics.json") || viewer->stats().frame_id!=42) return 5;
    viewer->Stop();viewer.reset();
    std::cout<<"t3bench_live_fixture=ok\n";return 0;
}
