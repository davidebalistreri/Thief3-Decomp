#include "scene_live_viewer.hpp"
#include "scene_live_dx12.hpp"
#include "scene_live_flat.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iomanip>
#include <iterator>
#include <locale>
#include <mutex>
#include <sstream>
#include <utility>

namespace t3render::live {
namespace {
LRESULT CALLBACK ViewerWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
HWND CreateViewerWindow() {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{}; cls.lpfnWndProc = ViewerWindowProc; cls.hInstance = instance;
    cls.lpszClassName = L"T3RenderSceneLiveViewer";
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    const auto window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName,
        L"T3Render — SceneIR live (sperimentale)", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768, nullptr, nullptr, instance, nullptr);
    if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
    return window;
}
struct Submission { FlatD3D12* gpu; bool attempted = false; };
bool SubmitPrepared(void* context) noexcept {
    auto& submission = *static_cast<Submission*>(context);
    submission.attempted = true;
    return submission.gpu->Submit();
}
bool WriteBytes(HANDLE file, const void* data, std::size_t size) noexcept {
    auto* bytes=static_cast<const std::uint8_t*>(data);
    while (size) {
        const auto chunk=static_cast<DWORD>(std::min<std::size_t>(size, 1u<<20));
        DWORD written=0;
        if (!WriteFile(file,bytes,chunk,&written,nullptr) || !written) return false;
        bytes+=written; size-=written;
    }
    return true;
}
struct Diagnostics {
    std::wstring directory, prefix;
    std::string session_id;
    HANDLE log=INVALID_HANDLE_VALUE;
    unsigned images=0, entries=0;
    ULONGLONG started=0;
    Matrix first_view{};
    ~Diagnostics() { if (log!=INVALID_HANDLE_VALUE) CloseHandle(log); }
    void Open(const std::wstring& path, const std::string& session) {
        if (path.empty()) return;
        // The same output directory can be inherited by multiple TDS
        // processes. Distinct session names keep their logs and images apart.
        if (session.empty() || session.size()>96) return;
        std::wstring tag;
        for (const unsigned char c:session) {
            if ((c>='a' && c<='z') || (c>='0' && c<='9'))
                tag.push_back(static_cast<wchar_t>(c));
            else if (c==':') tag.push_back(L'-');
            else return;
        }
        started=GetTickCount64();
        directory=path;session_id=session;prefix=L"scene-live-"+tag;
        log=CreateFileW((directory+L"\\"+prefix+L".jsonl").c_str(),GENERIC_WRITE,FILE_SHARE_READ,
                        nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (log==INVALID_HANDLE_VALUE) OutputDebugStringW(L"T3Render: cannot create live diagnostics\n");
    }
    void Event(const char* event) {
        if (log==INVALID_HANDLE_VALUE || entries>=4096) return;
        const std::string line=std::string("{\"event\":\"")+event+"\",\"session_id\":\""+session_id+"\"}\n";
        if (WriteBytes(log,line.data(),line.size())) ++entries;
        else { CloseHandle(log);log=INVALID_HANDLE_VALUE; }
    }
    void Telemetry(const DriverStats& s) {
        if (log==INVALID_HANDLE_VALUE || entries>=4096) return;
        std::ostringstream out;
        out<<"{\"event\":\"driver\",\"elapsed_ms\":"<<(GetTickCount64()-started)
           <<",\"probes\":"<<s.probes<<",\"selected_probes\":"<<s.selected_probes
           <<",\"selected_probe_bad\":"<<s.selected_probe_bad<<",\"bed_scopes\":"<<s.bed_scopes
           <<",\"bsp_scopes\":"<<s.bsp_scopes<<",\"bsp_cache_hits\":"<<s.bsp_cache_hits
           <<",\"draw_geometry_bad\":"<<s.draw_geometry_bad
           <<",\"draw_camera_bad\":"<<s.draw_camera_bad<<",\"draw_world_bad\":"<<s.draw_world_bad
           <<",\"draw_viewport_bad\":"<<s.draw_viewport_bad<<",\"scope_unstable\":"<<s.scope_unstable
           <<",\"scope_rejected\":"<<s.scope_rejected<<",\"losses\":"<<s.losses
           <<",\"primed\":"<<s.primed<<",\"published\":"<<s.published<<",\"rejected\":"<<s.rejected
           <<",\"last_rejection\":"<<s.last_rejection<<"}\n";
        const auto line=out.str();
        if (WriteBytes(log,line.data(),line.size())) ++entries;
        else { CloseHandle(log);log=INVALID_HANDLE_VALUE; }
    }
    bool WantsImage(const Snapshot& snapshot) const {
        if (log==INVALID_HANDLE_VALUE || images>=2) return false;
        if (!images) return true;
        for (std::size_t i=0;i<16;++i)
            if (std::fabs(snapshot.camera.view[i]-first_view[i])>0.001f) return true;
        return false;
    }
    bool Image(const Snapshot& snapshot, const scene::Frame& frame, const std::vector<std::uint8_t>& rgba) {
        if (rgba.size()!=std::size_t(frame.width)*frame.height*4) return false;
        wchar_t filename[80]{};
        std::swprintf(filename,std::size(filename),L"-frame-%llu.ppm",
                      static_cast<unsigned long long>(snapshot.frame_id));
        std::vector<std::uint8_t> row(std::size_t(frame.width)*3);
        HANDLE file=CreateFileW((directory+L"\\"+prefix+filename).c_str(),GENERIC_WRITE,FILE_SHARE_READ,
                                nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file==INVALID_HANDLE_VALUE) return false;
        char header[64];const int count=std::snprintf(header,sizeof(header),"P6\n%u %u\n255\n",frame.width,frame.height);
        bool ok=count>0 && WriteBytes(file,header,static_cast<std::size_t>(count));
        for (std::uint32_t y=0;ok && y<frame.height;++y) {
            for (std::uint32_t x=0;x<frame.width;++x) for (unsigned c=0;c<3;++c)
                row[std::size_t(x)*3+c]=rgba[(std::size_t(y)*frame.width+x)*4+c];
            ok=WriteBytes(file,row.data(),row.size());
        }
        CloseHandle(file);
        if (ok) { if (!images) first_view=snapshot.camera.view; ++images; }
        return ok;
    }
    void Frame(const Snapshot& snapshot, const ResidentFlatFrame& frame, const FlatCacheStats& cache, bool image) {
        if (log==INVALID_HANDLE_VALUE || entries>=4096) return;
        if (snapshot.provenance.origin==FrameOrigin::LiveProcess && snapshot.provenance.session_id!=session_id) return;
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9);
        const char* source=snapshot.provenance.origin==FrameOrigin::LiveProcess ? "live_process" :
                           snapshot.provenance.origin==FrameOrigin::CapturedReplay ? "captured_replay" : "synthetic";
        const auto scope=!snapshot.meshes.empty() ? snapshot.meshes[0].selection.identity_scope :
                         !snapshot.bsp.empty() ? snapshot.bsp[0].selection.identity_scope : IdentityScope::Unknown;
        const char* identity_scope=scope==IdentityScope::VisibleList ? "visible_list" :
                                   scope==IdentityScope::FullObjectScan ? "full_object_scan" : "unknown";
        out<<"{\"event\":\"frame\",\"source\":\""<<source<<"\",\"frame\":"<<snapshot.frame_id
           <<",\"identity_scope\":\""<<identity_scope<<"\""
           <<",\"render_pass\":"<<snapshot.render_pass<<",\"stage\":"<<snapshot.stage
           <<",\"map_epoch\":"<<snapshot.map_epoch<<",\"device_epoch\":"<<snapshot.device_epoch
           <<",\"continuity\":"<<snapshot.continuity
           <<",\"width\":"<<frame.camera.width<<",\"height\":"<<frame.camera.height
           <<",\"viewport\":{\"x\":"<<snapshot.camera.x<<",\"y\":"<<snapshot.camera.y
           <<",\"width\":"<<snapshot.camera.width<<",\"height\":"<<snapshot.camera.height
           <<",\"min_depth\":"<<snapshot.camera.min_depth<<",\"max_depth\":"<<snapshot.camera.max_depth<<'}'
           <<",\"mesh_instances\":"<<snapshot.meshes.size()<<",\"bsp_blocks\":"<<snapshot.bsp.size()
           <<",\"draws\":"<<frame.draws.size()<<",\"resident_resources\":"<<cache.resources
           <<",\"bed_override_instances\":"<<frame.override_instances
           <<",\"bedroom_material_runs\":"<<frame.bedroom_material_runs
           <<",\"textured_draws\":"<<cache.textured_draws
           <<",\"resident_bytes\":"<<cache.bytes<<",\"resource_uploads\":"<<cache.uploads
           <<",\"resource_evictions\":"<<cache.evictions<<",\"image\":"<<(image?"true":"false")<<",\"view\":[";
        for (unsigned i=0;i<16;++i) { if (i) out<<',';out<<snapshot.camera.view[i]; }
        out<<"],\"projection\":[";
        for (unsigned i=0;i<16;++i) { if (i) out<<',';out<<snapshot.camera.projection[i]; }
        out<<"]}\n";const auto text=out.str();
        if (WriteBytes(log,text.data(),text.size())) ++entries;
        else { CloseHandle(log);log=INVALID_HANDLE_VALUE; }
    }
};
} // namespace

struct SceneViewer::State {
    ViewerProvider provider;
    HANDLE wake = nullptr;
    std::atomic<bool> stop{false};
    bool window = false;
    mutable std::mutex mutex;
    ViewerStats stats;
    ~State() { if (wake) CloseHandle(wake); }
};
SceneViewer::SceneViewer(std::shared_ptr<State> state, void* thread) noexcept
    : state_(std::move(state)), thread_(thread) {}

std::unique_ptr<SceneViewer> SceneViewer::Start(ViewerProvider provider, bool window) noexcept {
    try {
        if (!provider.context || !provider.current || !provider.submit_if_current) return {};
        auto state = std::make_shared<State>(); state->provider = std::move(provider); state->window = window;
        state->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!state->wake) return {};
        auto viewer = std::unique_ptr<SceneViewer>(new SceneViewer(state, nullptr));
        auto argument = std::make_unique<std::shared_ptr<State>>(state);
        const auto thread = CreateThread(nullptr, 0, Run, argument.get(), 0, nullptr);
        if (!thread) return {};
        (void)argument.release();
        viewer->thread_ = thread;
        return viewer;
    } catch (...) { return {}; }
}
SceneViewer::~SceneViewer() {
    Stop();
    if (thread_) { (void)WaitForSingleObject(static_cast<HANDLE>(thread_), 1000); CloseHandle(static_cast<HANDLE>(thread_)); }
}
void SceneViewer::Notify() noexcept { if (state_ && state_->wake) (void)SetEvent(state_->wake); }
void SceneViewer::Stop() noexcept { if (state_) { state_->stop.store(true); Notify(); } }
ViewerStats SceneViewer::stats() const noexcept {
    try { if (state_) { std::lock_guard<std::mutex> lock(state_->mutex); return state_->stats; } }
    catch (...) {}
    ViewerStats result; result.failed = true; return result;
}

DWORD WINAPI SceneViewer::Run(void* context) noexcept {
    auto owner = std::unique_ptr<std::shared_ptr<State>>(static_cast<std::shared_ptr<State>*>(context));
    auto state = *owner;
    HWND window = nullptr;
    bool failed = false;
    Diagnostics diagnostics;
    try {
        diagnostics.Open(state->provider.diagnostic_directory,state->provider.diagnostic_session_id);
        diagnostics.Event("viewer_started");
        if (state->window) { window = CreateViewerWindow(); if (!window) failed = true; }
        auto gpu = failed || state->stop.load() ? nullptr : FlatD3D12::Create(window,128u*1024u*1024u,
                                                                               state->provider.bedroom_materials);
        if (!gpu && !state->stop.load()) failed = true;
        if (gpu) {
            diagnostics.Event("viewer_initialized");
            std::lock_guard<std::mutex> lock(state->mutex); state->stats.initialized = true;
        }
        std::shared_ptr<const Snapshot> last;
        scene::Frame last_frame;
        ULONGLONG next_telemetry=0;
        while (gpu && !state->stop.load()) {
            (void)WaitForSingleObject(state->wake, 50);
            if (state->stop.load()) break;
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { state->stop.store(true); break; }
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (state->stop.load()) break;
            const auto now=GetTickCount64();
            if (state->provider.driver_stats && now>=next_telemetry) {
                diagnostics.Telemetry(state->provider.driver_stats(state->provider.context.get()));
                next_telemetry=now+1000;
            }
            const auto current = state->provider.current(state->provider.context.get());
            if (!current) {
                // A rejected first frame can populate the cache without ever
                // becoming `last`. Retire it even when nothing was displayed.
                const bool had_residents = gpu->cache_stats().resources != 0;
                if (!gpu->RetireResidents()) { failed = true; break; }
                if (had_residents) {
                    std::lock_guard<std::mutex> lock(state->mutex); ++state->stats.resident_retirements;
                }
                if (last) {
                    last_frame.vertices.clear(); last_frame.draws.clear();
                    if (!gpu->Render(last_frame)) { failed = true; break; }
                    last.reset();
                    if (window) SetWindowTextW(window,L"T3Render — in attesa di SceneIR");
                    std::lock_guard<std::mutex> lock(state->mutex); ++state->stats.cleared;
                    state->stats.bedroom_material_runs=state->stats.textured_draws=0;
                }
                continue;
            }
            if (current == last) continue;
            ResidentFlatFrame frame;
            const bool wants_image=diagnostics.WantsImage(*current);
            if (!BuildResidentFlatFrame(*current, frame,state->provider.bed_override.get(),state->provider.bedroom_material_ids) ||
                !gpu->PrepareResident(frame,wants_image)) { failed = true; break; }
            Submission submission{gpu.get()};
            const bool submitted = state->provider.submit_if_current(state->provider.context.get(), current,
                                                                      SubmitPrepared, &submission);
            if (!submitted) {
                if (submission.attempted) { failed = true; break; }
                gpu->Cancel();
                std::lock_guard<std::mutex> lock(state->mutex); ++state->stats.dropped;
                continue;
            }
            std::vector<std::uint8_t> pixels;
            if (!gpu->Finish(wants_image ? &pixels : nullptr)) { failed = true; break; }
            // Present may enter the window system, so never hold the scene or
            // driver lock across it. A newer frame is allowed only across the
            // same continuity; a later reset can still race the display call.
            const auto latest=state->provider.current(state->provider.context.get());
            if (!latest || !DisplayContinuityMatches(*current,*latest)) {
                std::lock_guard<std::mutex> lock(state->mutex); ++state->stats.dropped;
                continue;
            }
            const auto presentation=gpu->Present();
            if (presentation==FlatPresent::Failed) { failed = true; break; }
            if (presentation==FlatPresent::Busy) {
                std::lock_guard<std::mutex> lock(state->mutex); ++state->stats.dropped;
                continue;
            }
            const bool wrote_image=wants_image && diagnostics.Image(*current,frame.camera,pixels);
            diagnostics.Frame(*current,frame,gpu->cache_stats(),wrote_image);
            last = current; last_frame = std::move(frame.camera);
            if (window) {
                wchar_t title[128]{};
                std::swprintf(title, std::size(title), L"T3Render — frame %llu (parziale)",
                              static_cast<unsigned long long>(current->frame_id));
                SetWindowTextW(window,title);
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->stats.frames; state->stats.map_epoch = current->map_epoch;
                state->stats.device_epoch = current->device_epoch; state->stats.frame_id = current->frame_id;
                state->stats.bedroom_material_runs=frame.bedroom_material_runs;
                state->stats.textured_draws=gpu->cache_stats().textured_draws;
            }
        }
    } catch (...) { failed = true; }
    try { diagnostics.Event(failed ? "viewer_failed" : "viewer_stopped"); } catch (...) {}
    if (window && IsWindow(window)) DestroyWindow(window);
    try { std::lock_guard<std::mutex> lock(state->mutex); state->stats.failed = failed; state->stats.stopped = true; }
    catch (...) {}
    return failed ? 1 : 0;
}
} // namespace t3render::live
