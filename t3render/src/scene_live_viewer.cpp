#include "scene_live_viewer.hpp"
#include "scene_live_dx12.hpp"
#include "scene_live_flat.hpp"
#include "scene_live_lighting.hpp"
#include "scene_live_light_diagnostics.hpp"
#include "scene_live_wall_displacement.hpp"

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
const char* DrawKindName(ObservedDrawKind kind) noexcept {
    switch (kind) {
    case ObservedDrawKind::Clear:return "clear";
    case ObservedDrawKind::Primitive:return "primitive";
    case ObservedDrawKind::IndexedPrimitive:return "indexed";
    case ObservedDrawKind::PrimitiveUP:return "primitive_up";
    case ObservedDrawKind::IndexedPrimitiveUP:return "indexed_up";
    default:return "unknown";
    }
}
const char* ThreadRelationName(ThreadRelation relation) noexcept {
    switch (relation) {
    case ThreadRelation::Same:return "same";
    case ThreadRelation::Different:return "different";
    default:return "unknown";
    }
}
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
    std::wstring directory, prefix, capture_trigger;
    std::string session_id;
    HANDLE log=INVALID_HANDLE_VALUE;
    unsigned images=0, capture_images=0, entries=0;
    bool capture_started=false,initial_images=true;
    ULONGLONG started=0;
    Matrix first_view{};
    static constexpr unsigned kMaxEntries=32768, kMaxCaptureImages=8;
    ~Diagnostics() { if (log!=INVALID_HANDLE_VALUE) CloseHandle(log); }
    void Open(const std::wstring& path, const std::string& session, const std::wstring& trigger, bool initial) {
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
        directory=path;session_id=session;prefix=L"scene-live-"+tag;capture_trigger=trigger;initial_images=initial;
        log=CreateFileW((directory+L"\\"+prefix+L".jsonl").c_str(),GENERIC_WRITE,FILE_SHARE_READ,
                        nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (log==INVALID_HANDLE_VALUE) OutputDebugStringW(L"T3Render: cannot create live diagnostics\n");
    }
    void Event(const char* event) {
        if (log==INVALID_HANDLE_VALUE || entries>=kMaxEntries) return;
        const std::string line=std::string("{\"event\":\"")+event+"\",\"session_id\":\""+session_id+"\"}\n";
        if (WriteBytes(log,line.data(),line.size())) ++entries;
        else { CloseHandle(log);log=INVALID_HANDLE_VALUE; }
    }
    void Telemetry(const DriverStats& s) {
        if (log==INVALID_HANDLE_VALUE || entries>=kMaxEntries) return;
        std::ostringstream out;
        out<<"{\"event\":\"driver\",\"elapsed_ms\":"<<(GetTickCount64()-started)
           <<",\"probes\":"<<s.probes<<",\"selected_probes\":"<<s.selected_probes
           <<",\"selected_probe_bad\":"<<s.selected_probe_bad<<",\"bed_scopes\":"<<s.bed_scopes
           <<",\"static_mesh_samples\":"<<s.static_mesh_samples<<",\"static_mesh_scopes\":"<<s.static_mesh_scopes
           <<",\"static_mesh_rejected\":"<<s.static_mesh_rejected
           <<",\"map_witness_conflicts\":"<<s.map_witness_conflicts
           <<",\"unscoped_viewport_attempts\":"<<s.unscoped_viewport_attempts
           <<",\"map_transitions\":"<<s.map_transitions
           <<",\"bsp_scopes\":"<<s.bsp_scopes<<",\"bsp_cache_hits\":"<<s.bsp_cache_hits
           <<",\"draw_geometry_bad\":"<<s.draw_geometry_bad
           <<",\"draw_camera_bad\":"<<s.draw_camera_bad<<",\"draw_world_bad\":"<<s.draw_world_bad
           <<",\"draw_viewport_bad\":"<<s.draw_viewport_bad<<",\"scope_unstable\":"<<s.scope_unstable
           <<",\"scope_rejected\":"<<s.scope_rejected<<",\"losses\":"<<s.losses
           <<",\"loss_invalidations\":"<<s.loss_invalidations
           <<",\"primed\":"<<s.primed<<",\"published\":"<<s.published<<",\"rejected\":"<<s.rejected
           <<",\"last_rejection\":"<<s.last_rejection
           <<",\"present_pending_probes\":"<<s.present_pending_probes
           <<",\"present_pending_light_samples\":"<<s.present_pending_light_samples
           <<",\"present_pending_scopes\":"<<s.present_pending_scopes
           <<",\"present_pending_draws\":"<<s.present_pending_draws
           <<",\"present_pending_presents\":"<<s.present_pending_presents
           <<",\"present_pending_resets\":"<<s.present_pending_resets;
        if (s.present_pending_draws) {
            out<<",\"oldest_pending_draw_kind\":\""<<DrawKindName(s.oldest_pending_draw_kind)<<"\""
               <<",\"oldest_pending_draw_thread\":\""<<ThreadRelationName(s.oldest_pending_draw_thread)<<"\""
               <<",\"oldest_pending_draw_scoped\":"<<(s.oldest_pending_draw_scoped?"true":"false")
               <<",\"oldest_pending_draw_presents\":"<<s.oldest_pending_draw_presents;
        }
        const auto& timing=s.present_timing;
        if (timing.clock_hz) {
            out<<",\"present_timing\":{\"clock_hz\":"<<timing.clock_hz
               <<",\"successful\":"<<timing.successful<<",\"failed\":"<<timing.failed
               <<",\"overlaps\":"<<timing.overlaps<<",\"resets\":"<<timing.resets
               <<",\"interruptions\":"<<timing.interruptions<<",\"clock_errors\":"<<timing.clock_errors
               <<",\"intervals\":"<<timing.intervals<<",\"total_ticks\":"<<timing.total_ticks
               <<",\"max_ticks\":"<<timing.max_ticks<<",\"bounds_ms\":[";
            for (std::size_t i=0;i<kPresentTimingBoundsMs.size();++i) {
                if (i) out<<',';
                out<<kPresentTimingBoundsMs[i];
            }
            out<<"],\"histogram\":[";
            for (std::size_t i=0;i<timing.histogram.size();++i) {
                if (i) out<<',';
                out<<timing.histogram[i];
            }
            out<<"]}";
        }
        out<<"}\n";
        const auto line=out.str();
        if (WriteBytes(log,line.data(),line.size())) ++entries;
        else { CloseHandle(log);log=INVALID_HANDLE_VALUE; }
    }
    bool WantsImage(const Snapshot& snapshot) {
        if (log==INVALID_HANDLE_VALUE) return false;
        if (!capture_started && !capture_trigger.empty() &&
            GetFileAttributesW(capture_trigger.c_str())!=INVALID_FILE_ATTRIBUTES) capture_started=true;
        if (capture_started) return capture_images<kMaxCaptureImages;
        if (!initial_images || images>=2) return false;
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
        if (ok) {
            if (capture_started) ++capture_images;
            else { if (!images) first_view=snapshot.camera.view; ++images; }
        }
        return ok;
    }
    void Frame(const Snapshot& snapshot, const ResidentFlatFrame& frame, const FlatCacheStats& cache, bool image,
               const RawLightFrame* raw, ULONGLONG presented_at, const char* lighting_mode) {
        if (log==INVALID_HANDLE_VALUE || entries>=kMaxEntries) return;
        if (snapshot.provenance.origin==FrameOrigin::LiveProcess && snapshot.provenance.session_id!=session_id) return;
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9);
        const auto scope=!snapshot.meshes.empty() ? snapshot.meshes[0].selection.identity_scope :
                         !snapshot.bsp.empty() ? snapshot.bsp[0].selection.identity_scope : IdentityScope::Unknown;
        const char* identity_scope=scope==IdentityScope::VisibleList ? "visible_list" :
                                   scope==IdentityScope::FullObjectScan ? "full_object_scan" : "unknown";
        const char* light_coverage=snapshot.light_coverage==LightCoverage::Complete ? "complete" :
                                   snapshot.light_coverage==LightCoverage::Partial ? "partial" : "unknown";
        const char* viewport_source=snapshot.camera.viewport_source==ViewportSource::ScopedDraw ? "scoped_draw" :
                                    snapshot.camera.viewport_source==ViewportSource::CameraMatchedDraw ? "camera_matched_draw" : "unknown";
        std::size_t ready_lights=0,disabled_lights=0,incomplete_lights=0;
        for (const auto& light:snapshot.lights) {
            const auto status=ClassifyVisualLight(light);
            if (status==LightReadiness::Ready) ++ready_lights;
            else if (status==LightReadiness::Disabled) ++disabled_lights;
            else ++incomplete_lights;
        }
        std::uint64_t triangles=0,head_wall_triangles=0;
        std::size_t flame_draws=0,candlestick_draws=0;
        for (const auto& draw:frame.draws) triangles+=draw.index_count/3;
        for (const auto& draw:frame.draws) if (draw.wall_relief==2) head_wall_triangles+=draw.index_count/3;
        for (const auto& draw:frame.draws) if (draw.emission>0) ++flame_draws;
        for (const auto& draw:frame.draws)
            if (draw.resource && draw.resource->id=="preview:generated-candlestick") ++candlestick_draws;
        out<<"{\"event\":\"frame\"";WriteFrameIdentity(out,snapshot);
        out<<",\"elapsed_ms\":"<<(presented_at-started);
        out<<",\"identity_scope\":\""<<identity_scope<<"\""
           <<",\"render_pass\":"<<snapshot.render_pass<<",\"stage\":"<<snapshot.stage
           <<",\"map_epoch\":"<<snapshot.map_epoch<<",\"device_epoch\":"<<snapshot.device_epoch
           <<",\"continuity\":"<<snapshot.continuity
           <<",\"width\":"<<frame.camera.width<<",\"height\":"<<frame.camera.height
           <<",\"viewport\":{\"x\":"<<snapshot.camera.x<<",\"y\":"<<snapshot.camera.y
           <<",\"width\":"<<snapshot.camera.width<<",\"height\":"<<snapshot.camera.height
           <<",\"min_depth\":"<<snapshot.camera.min_depth<<",\"max_depth\":"<<snapshot.camera.max_depth
           <<",\"source\":\""<<viewport_source<<"\"}"
           <<",\"mesh_instances\":"<<snapshot.meshes.size()<<",\"bsp_blocks\":"<<snapshot.bsp.size()
           <<",\"light_coverage\":\""<<light_coverage<<"\",\"visual_lights\":"<<snapshot.lights.size()
           <<",\"lighting_mode\":\""<<lighting_mode<<"\""
           <<",\"ready_lights\":"<<ready_lights<<",\"disabled_lights\":"<<disabled_lights
           <<",\"incomplete_lights\":"<<incomplete_lights
           <<",\"draws\":"<<frame.draws.size()<<",\"triangles\":"<<triangles<<",\"resident_resources\":"<<cache.resources
           <<",\"preview_flame_draws\":"<<flame_draws
           <<",\"preview_candlestick_draws\":"<<candlestick_draws
           <<",\"candidate_head_wall_triangles\":"<<head_wall_triangles
           <<",\"wall_displacement_source_triangles\":"<<frame.wall_source_triangles
           <<",\"wall_displacement_triangles\":"<<frame.wall_displaced_triangles
           <<",\"wall_displacement_failed\":"<<(frame.wall_displacement_failed?"true":"false")
           <<",\"preview_gi_enabled\":"<<(cache.gi_enabled?"true":"false")
           <<",\"bed_override_instances\":"<<frame.override_instances
           <<",\"bedroom_material_runs\":"<<frame.bedroom_material_runs
           <<",\"textured_draws\":"<<cache.textured_draws
           <<",\"pbr_draws\":"<<cache.pbr_draws
           <<",\"shadowed_lights\":"<<cache.shadowed_lights<<",\"shadow_draws\":"<<cache.shadow_draws
           <<",\"shadow_budget_exceeded\":"<<(cache.shadow_budget_exceeded?"true":"false")
           <<",\"resident_bytes\":"<<cache.bytes<<",\"resource_uploads\":"<<cache.uploads
           <<",\"resource_evictions\":"<<cache.evictions<<",\"material_gpu_bytes\":"<<cache.material_bytes
           <<",\"gpu_frame_ms\":";
        if (std::isfinite(cache.gpu_frame_ms) && cache.gpu_frame_ms>=0) out<<cache.gpu_frame_ms;
        else out<<"null";
        out<<",\"gpu_timing_status\":"<<cache.gpu_timing_status
           <<",\"gpu_frame_includes_readback\":"<<(cache.gpu_frame_includes_readback?"true":"false")
           <<",\"video_memory_usage_bytes\":";
        if (cache.video_memory_usage_bytes==UINT64_MAX) out<<"null";
        else out<<cache.video_memory_usage_bytes;
        out<<",\"video_memory_sample_age_ms\":";
        if (cache.video_memory_usage_bytes==UINT64_MAX || !cache.video_memory_sampled_ms ||
            presented_at<cache.video_memory_sampled_ms) out<<"null";
        else out<<(presented_at-cache.video_memory_sampled_ms);
        out<<",\"image\":"<<(image?"true":"false")<<",\"view\":[";
        for (unsigned i=0;i<16;++i) { if (i) out<<',';out<<snapshot.camera.view[i]; }
        out<<"],\"projection\":[";
        for (unsigned i=0;i<16;++i) { if (i) out<<',';out<<snapshot.camera.projection[i]; }
        out<<']';WriteLightDetails(out,snapshot.lights);WriteRawLightDetails(out,snapshot,raw);
        out<<"}\n";const auto text=out.str();
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
        diagnostics.Open(state->provider.diagnostic_directory,state->provider.diagnostic_session_id,
                         state->provider.diagnostic_capture_trigger,state->provider.diagnostic_initial_images);
        diagnostics.Event("viewer_started");
        if (state->window) { window = CreateViewerWindow(); if (!window) failed = true; }
        auto active_materials=state->provider.bedroom_materials;
        const bool gpu_metrics=diagnostics.log!=INVALID_HANDLE_VALUE && state->provider.diagnostic_gpu_metrics;
        auto gpu = failed || state->stop.load() ? nullptr : FlatD3D12::Create(window,128u*1024u*1024u,active_materials,gpu_metrics,state->provider.preview_gi);
        if (!gpu && !state->stop.load()) failed = true;
        if (gpu) {
            diagnostics.Event("viewer_initialized");
            std::lock_guard<std::mutex> lock(state->mutex); state->stats.initialized = true;
        }
        std::shared_ptr<const Snapshot> last;
        scene::Frame last_frame;
        WallDisplacementCache displacement_cache;
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
                displacement_cache.Clear();
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
                    state->stats.bedroom_material_runs=state->stats.textured_draws=state->stats.pbr_draws=state->stats.shadowed_lights=0;
                    state->stats.wall_displaced_triangles=0;state->stats.wall_displacement_failed=false;
                    state->stats.preview_gi_active=false;
                }
                continue;
            }
            if (current == last) continue;
            VisualAssets assets{state->provider.bed_override,state->provider.bedroom_materials};
            if (state->provider.visual_assets &&
                !state->provider.visual_assets(state->provider.context.get(),current,assets)) {
                std::lock_guard<std::mutex> lock(state->mutex);++state->stats.dropped;continue;
            }
            if (assets.bedroom_materials!=active_materials) {
                // Texture descriptors are immutable for a backend. Rebuild on
                // the worker, after prior GPU work has completed.
                displacement_cache.Clear();gpu.reset();active_materials=assets.bedroom_materials;
                gpu=FlatD3D12::Create(window,128u*1024u*1024u,active_materials,gpu_metrics,state->provider.preview_gi);
                if (!gpu) {failed=true;break;}
                std::lock_guard<std::mutex> lock(state->mutex);++state->stats.renderer_rebuilds;
            }
            ResidentFlatFrame frame;
            const bool wants_image=diagnostics.WantsImage(*current);
            if (!BuildResidentFlatFrame(*current,frame,assets.bed_override.get(),
                                        state->provider.bedroom_material_ids || bool(assets.bedroom_materials),
                                        assets.bedroom_materials && bool(assets.bedroom_materials->wall_height))) {
                failed=true;break;
            }
            if (state->provider.wall_displacement &&
                std::any_of(frame.draws.begin(),frame.draws.end(),[](const auto& draw){return draw.wall_relief==2;})) {
                WallDisplacementStats displacement;
                if (displacement_cache.Apply(*current,frame,assets.bedroom_materials,displacement)) {
                    frame.wall_source_triangles=displacement.source_triangles;
                    frame.wall_displaced_triangles=displacement.displaced_triangles;
                } else frame.wall_displacement_failed=true; // retain POM for this frame
            }
            VerifiedLighting verified;
            const bool lit=BuildVerifiedLighting(*current,verified)==LightingResult::Ready &&
                           LightingMatchesSnapshot(*current,verified);
            const auto candle_raw=state->provider.preview_candle && state->provider.light_diagnostics ?
                state->provider.light_diagnostics(state->provider.context.get(),current) : nullptr;
            RenderLight candle_light;
            Vec3 preview_camera{};
            const bool preview_conventions=current->conventions.meters_per_unit &&
                std::isfinite(*current->conventions.meters_per_unit) && *current->conventions.meters_per_unit>0 &&
                current->conventions.length_unit==LengthUnit::T3Unit && current->conventions.axes==Axes::T3Xyz &&
                current->conventions.up_axis==UpAxis::Z && CameraPositionFromView(current->camera.view,preview_camera);
            const bool candle=!lit && preview_conventions && state->provider.preview_candle && candle_raw &&
                CandlePreviewFromRaw(*current,*candle_raw,candle_light);
            std::size_t triangles=0;
            for (const auto& draw:frame.draws) triangles+=draw.index_count/3;
            std::array<ResidentFlatDraw,2> candlestick;
            bool candlestick_drawn=false;
            if (!lit && preview_conventions && state->provider.preview_candle && candle_raw &&
                frame.draws.size()+candlestick.size()<=scene::kMaxDraws &&
                CandlePreviewCandlestick(*current,*candle_raw,candlestick)) {
                const auto added=(candlestick[0].index_count+candlestick[1].index_count)/3;
                if (triangles<=scene::kMaxTriangles && added<=scene::kMaxTriangles-triangles) {
                    triangles+=added;
                    for (auto& draw:candlestick) frame.draws.push_back(std::move(draw));
                    candlestick_drawn=true;
                }
            }
            ResidentFlatDraw flame;
            if (candle && candlestick_drawn && frame.draws.size()<scene::kMaxDraws &&
                CandlePreviewFlame(*current,*candle_raw,flame) && triangles<=scene::kMaxTriangles &&
                flame.index_count/3<=scene::kMaxTriangles-triangles) frame.draws.push_back(std::move(flame));
            const bool candle_zero=!lit && preview_conventions && state->provider.preview_candle && !candle && candle_raw &&
                CandlePreviewDirectZero(*current,*candle_raw);
            const bool candle_unknown=!lit && preview_conventions && state->provider.preview_candle && !candle && !candle_zero;
            const bool preview=!lit && preview_conventions && !state->provider.preview_candle && state->provider.preview_pbr;
            const char* lighting_mode=lit ? "verified" : candle ? "preview_candle_candidate" : candle_zero ?
                                      "preview_candle_zero_candidate" : candle_unknown ?
                                      "preview_candle_unknown" : preview ? "preview_headlight" : "unlit";
            if (!(lit ? gpu->PrepareLit(*current,frame,wants_image) : candle || candle_zero || candle_unknown ?
                  gpu->PrepareCandlePreview(*current,candle ? candle_raw.get() : nullptr,frame,wants_image) : preview ?
                  gpu->PreparePreviewLit(*current,frame,wants_image) : gpu->PrepareResident(frame,wants_image))) {
                failed=true;break;
            }
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
            const auto presented_at=GetTickCount64();
            const bool wrote_image=wants_image && diagnostics.Image(*current,frame.camera,pixels);
            const auto light_raw=candle_raw ? candle_raw :
                diagnostics.log!=INVALID_HANDLE_VALUE && state->provider.light_diagnostics ?
                state->provider.light_diagnostics(state->provider.context.get(),current) : nullptr;
            diagnostics.Frame(*current,frame,gpu->cache_stats(),wrote_image,light_raw.get(),presented_at,lighting_mode);
            last = current; last_frame = std::move(frame.camera);
            if (window) {
                wchar_t title[128]{};
                std::swprintf(title, std::size(title), candle ? L"T3Render — frame %llu (candela, anteprima)" :
                              candle_zero ? L"T3Render — frame %llu (candela, fattore zero)" :
                              candle_unknown ? L"T3Render — frame %llu (candela sconosciuta)" :
                              preview ? L"T3Render — frame %llu (PBR, luce di prova)" :
                              L"T3Render — frame %llu (parziale)", static_cast<unsigned long long>(current->frame_id));
                SetWindowTextW(window,title);
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                ++state->stats.frames; state->stats.map_epoch = current->map_epoch;
                state->stats.device_epoch = current->device_epoch; state->stats.frame_id = current->frame_id;
                state->stats.bedroom_material_runs=frame.bedroom_material_runs;
                state->stats.textured_draws=gpu->cache_stats().textured_draws;
                state->stats.pbr_draws=gpu->cache_stats().pbr_draws;
                state->stats.shadowed_lights=gpu->cache_stats().shadowed_lights;
                state->stats.wall_displaced_triangles=frame.wall_displaced_triangles;
                state->stats.wall_displacement_failed=frame.wall_displacement_failed;
                state->stats.preview_gi_active=gpu->cache_stats().gi_enabled;
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
