#include "scene_live_runtime.hpp"
#include "scene_live_catalog_win.hpp"
#include "scene_live_visible_lights.hpp"
#include "scene_live_viewer.hpp"
#include "scene_live_stack_win.hpp"

#include <windows.h>
#include <wincrypt.h>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace t3render::live {
namespace {
static_assert(std::atomic<bool>::is_always_lock_free,"nested probe loss must be lock-free");
struct RuntimeState {
    std::mutex device_mutex;
    std::unique_ptr<SceneDriver> owned;
    std::unique_ptr<SceneViewer> viewer;
    std::string session_id; // initialized before publishing driver; never changed afterwards
    bool static_meshes_enabled=false; // written before driver publication
    bool light_candidates_enabled=false;
    bool light_raw_diagnostics_enabled=false;
    bool candle_link_diagnostics_enabled=false;
    std::uint32_t candle_actor_number=6; // bounded bedroom choice, FName number
    bool present_clock_enabled=false;
    std::string map, map_sha256; // catalog fingerprint, not a runtime package measurement
    std::atomic<SceneDriver*> driver{nullptr};
    std::atomic<IDirect3DDevice8*> device{nullptr};
    std::atomic<bool> terminal{false},deferred_loss{false};
};
RuntimeState& State() {
    // Hooks and their trampolines are pinned until process exit. Deliberately
    // retain the driver as well so an in-flight callback cannot outlive it.
    static auto* state=new RuntimeState;
    return *state;
}
struct LastError { DWORD value=GetLastError();~LastError(){SetLastError(value);} };
thread_local bool querying=false;
struct Scope {
    std::uint64_t ticket=0;std::uint32_t element=0;bool sampled=false,generic=false;
    std::unique_ptr<scene_bridge::AssetCandidate> asset;
    std::shared_ptr<const scene_bridge::ProbeSnapshot> prop_probe;
    scene_bridge::BedroomProp prop=scene_bridge::BedroomProp::None;
};
struct ScopeStack { unsigned depth=0;Scope items[16]{}; };
thread_local ScopeStack scopes;

ThreadToken CurrentThread() noexcept {
    thread_local const ThreadToken token=[]() noexcept {
        FILETIME created{},exited{},kernel{},user{};
        ThreadToken result;result.id=static_cast<std::uint32_t>(GetCurrentThreadId());
        if (!result.id || !GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user)) return ThreadToken{};
        result.birth=(std::uint64_t{created.dwHighDateTime}<<32)|created.dwLowDateTime;
        return result.birth ? result : ThreadToken{};
    }();
    return token;
}
bool Enabled(const RuntimeState& state) noexcept {
    return !state.terminal.load(std::memory_order_acquire) && state.driver.load(std::memory_order_acquire);
}
SceneDriver* Driver(bool require_device) {
    auto& state=State();auto* driver=state.driver.load(std::memory_order_acquire);
    if (!driver || state.terminal.load(std::memory_order_acquire) ||
        (require_device && !state.device.load(std::memory_order_acquire))) return nullptr;
    if (state.deferred_loss.exchange(false,std::memory_order_acq_rel)) driver->MarkLost();
    return driver;
}
bool IsDevice(IDirect3DDevice8* device) {
    return device && State().device.load(std::memory_order_acquire)==device;
}
void NotifyViewer() noexcept {
    try { if (State().viewer) State().viewer->Notify(); } catch (...) {}
}
std::shared_ptr<const Snapshot> ViewerCurrent(void*) noexcept { return RuntimeCurrent(); }
std::shared_ptr<const RawLightFrame> ViewerLightDiagnostics(void*,const std::shared_ptr<const Snapshot>& expected) noexcept {
    try {auto* driver=Driver(true);return driver ? driver->LightDiagnosticsIfCurrent(expected) : nullptr;}
    catch (...) {return {};}
}
DriverStats ViewerDriverStats(void*) noexcept {
    try { auto* driver=State().driver.load(std::memory_order_acquire);return driver ? driver->stats() : DriverStats{}; }
    catch (...) { return {}; }
}
bool ViewerVisualAssets(void*,const std::shared_ptr<const Snapshot>& expected,VisualAssets& output) noexcept {
    try {auto* driver=Driver(true);return driver && driver->VisualAssetsIfCurrent(expected,output);}
    catch (...) {return false;}
}
struct GuardedSubmission { RuntimeState* state; SubmitOperation operation; void* argument; };
bool SubmitIfSafe(void* context) noexcept {
    auto& item=*static_cast<GuardedSubmission*>(context);
    if (item.state->terminal.load(std::memory_order_acquire) ||
        item.state->deferred_loss.load(std::memory_order_acquire) ||
        !item.state->device.load(std::memory_order_acquire)) return false;
    return item.operation(item.argument);
}
bool ViewerSubmit(void*,const std::shared_ptr<const Snapshot>& expected,
                  SubmitOperation operation,void* argument) noexcept {
    try {
        auto& state=State();auto* driver=Driver(true);
        if (!driver || !operation) return false;
        GuardedSubmission guarded{&state,operation,argument};
        return driver->SubmitIfCurrent(expected,SubmitIfSafe,&guarded);
    } catch (...) { RuntimeAbandon();return false; }
}
bool ReadMemory(void*,std::uint32_t address,void* output,std::size_t size) noexcept {
    SIZE_T copied=0;
    return address && output && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),output,size,&copied) && copied==size;
}
scene_bridge::SubmissionSample Sample(std::uint32_t element,bool generic=false) noexcept {
    const scene_bridge::ProbeReader reader{nullptr,ReadMemory};
    return scene_bridge::SampleSubmission(reader,element,generic);
}
std::wstring Environment(const wchar_t* name) {
    const DWORD required=GetEnvironmentVariableW(name,nullptr,0);
    if (!required || required>32767) return {};
    std::vector<wchar_t> value(required);
    const DWORD length=GetEnvironmentVariableW(name,value.data(),required);
    return length && length<required ? std::wstring(value.data(),length) : std::wstring{};
}
bool SessionId(std::string& output) {
    HCRYPTPROV provider=0;std::uint8_t bytes[16]{};
    if (!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT|CRYPT_SILENT)) return false;
    const bool ok=CryptGenRandom(provider,sizeof(bytes),bytes)!=FALSE;
    CryptReleaseContext(provider,0);
    if (!ok) return false;
    constexpr char hex[]="0123456789abcdef";std::string value="live:";
    for (auto byte:bytes) {value.push_back(hex[byte>>4]);value.push_back(hex[byte&15]);}
    output=std::move(value);return true;
}
struct QueryGuard {
    bool entered=false;
    QueryGuard() noexcept { if (!querying) {querying=true;entered=true;} else RuntimeDeferredLoss(); }
    ~QueryGuard() { if (entered) querying=false; }
};
bool Query(IDirect3DDevice8* device,DrawObservation& observation,bool needs_world) noexcept {
    if (!device) return false;
    QueryGuard guard;if (!guard.entered) return false;
    D3DMATRIX view{},projection{},world{};D3DVIEWPORT8 viewport{};
    static_assert(sizeof(D3DMATRIX)==sizeof(float)*16,"unexpected D3D8 matrix size");
    if (FAILED(device->GetTransform(D3DTS_VIEW,&view)) || FAILED(device->GetTransform(D3DTS_PROJECTION,&projection)) ||
        (needs_world && FAILED(device->GetTransform(D3DTS_WORLD,&world))) || FAILED(device->GetViewport(&viewport))) return false;
    DrawObservation result;result.available=true;
    std::memcpy(result.view.data(),&view,sizeof(view));std::memcpy(result.projection.data(),&projection,sizeof(projection));
    std::memcpy(result.world.data(),&world,sizeof(world));
    result.viewport={viewport.X,viewport.Y,viewport.Width,viewport.Height,viewport.MinZ,viewport.MaxZ};
    observation=result;return true;
}
} // namespace

bool RuntimeInitialize() noexcept {
    LastError saved;
    try {
        static std::once_flag once;
        std::call_once(once,[]{
            auto& state=State();
            if (Environment(L"T3RENDER_SCENE_LIVE")!=L"1") return;
            const auto path=Environment(L"T3RENDER_LIVE_CATALOG");
            const auto candle_number=Environment(L"T3RENDER_SCENE_LIVE_CANDLE_NUMBER");
            if (!candle_number.empty() && candle_number!=L"6" && candle_number!=L"14") return;
            state.candle_actor_number=candle_number==L"14" ? 14u : 6u;
            LoadedCatalog catalog;std::string session;
            if (path.empty() || !LoadCatalogFile(path.c_str(),catalog) || !SessionId(session)) return;
            FrameProvenance source;source.origin=FrameOrigin::LiveProcess;source.session_id=session;
            auto bed_override=catalog.bed_override;
            auto bedroom_materials=catalog.bedroom_materials;
            state.map=catalog.binding.map;state.map_sha256=catalog.binding.map_sha256;
            const bool viewer_enabled=Environment(L"T3RENDER_SCENE_LIVE_DX12")==L"1";
            const auto diagnostic_directory=viewer_enabled ? Environment(L"T3RENDER_SCENE_LIVE_DIAGNOSTIC_DIR") : std::wstring{};
            LARGE_INTEGER frequency{};
            const bool clock_ready=!diagnostic_directory.empty() && QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0;
            auto driver=SceneDriver::Create(std::move(catalog),std::move(source),0,1,
                                            clock_ready ? static_cast<std::uint64_t>(frequency.QuadPart) : 0);
            if (!driver) return;
            state.present_clock_enabled=clock_ready;
            state.static_meshes_enabled=Environment(L"T3RENDER_SCENE_LIVE_STATIC_MESHES")==L"1";
            state.light_candidates_enabled=Environment(L"T3RENDER_SCENE_LIVE_LIGHTS")==L"1";
            state.light_raw_diagnostics_enabled=state.light_candidates_enabled && !diagnostic_directory.empty();
            state.candle_link_diagnostics_enabled=state.light_raw_diagnostics_enabled &&
                Environment(L"T3RENDER_SCENE_LIVE_CANDLE_LINK")==L"1" &&
                state.map=="SouthQuarter1_int1" &&
                state.map_sha256=="ed7d475b1efdc713d28b2a978e11fb0f662bdb17c5c2769d77c96e1f74b7ceba";
            state.session_id=std::move(session);
            state.owned=std::move(driver);state.driver.store(state.owned.get(),std::memory_order_release);
            if (viewer_enabled) {
                ViewerProvider provider;provider.context=std::shared_ptr<void>(&state,[](void*){});
                provider.current=ViewerCurrent;provider.submit_if_current=ViewerSubmit;
                provider.driver_stats=ViewerDriverStats;
                provider.visual_assets=ViewerVisualAssets;
                provider.light_diagnostics=state.light_raw_diagnostics_enabled ? ViewerLightDiagnostics : nullptr;
                provider.bed_override=std::move(bed_override);
                provider.bedroom_materials=std::move(bedroom_materials);
                provider.bedroom_material_ids=Environment(L"T3RENDER_SCENE_LIVE_BEDROOM_MATERIAL_IDS")==L"1";
                provider.preview_pbr=Environment(L"T3RENDER_SCENE_LIVE_PREVIEW_PBR")==L"1";
                provider.preview_candle=Environment(L"T3RENDER_SCENE_LIVE_PREVIEW_CANDLE")==L"1";
                provider.wall_displacement=Environment(L"T3RENDER_SCENE_LIVE_WALL_DISPLACEMENT")==L"1";
                provider.preview_gi=Environment(L"T3RENDER_SCENE_LIVE_PREVIEW_GI")==L"1";
                provider.diagnostic_directory=diagnostic_directory;
                provider.diagnostic_capture_trigger=Environment(L"T3RENDER_CAPTURE_TRIGGER_PATH");
                provider.remaster_toggle_path=Environment(L"T3RENDER_SCENE_LIVE_REMASTER_TOGGLE");
                if (!Environment(L"T3RENDER_CAPTURE_INTERVAL_MS").empty())
                    provider.diagnostic_capture_frame=[](void*,std::uint64_t frame) noexcept {
                        return CaptureSampledLiveFrame(frame);
                    };
                provider.diagnostic_initial_images=Environment(L"T3RENDER_SCENE_LIVE_INITIAL_IMAGES")!=L"0";
                provider.diagnostic_gpu_metrics=Environment(L"T3RENDER_SCENE_LIVE_GPU_METRICS")==L"1";
                provider.diagnostic_session_id=state.session_id;
                state.viewer=SceneViewer::Start(std::move(provider),true);
                if (!state.viewer) OutputDebugStringW(L"T3Render: live DX12 viewer failed to start\n");
            }
        });
        return Enabled(State());
    } catch (...) { return false; }
}
bool RuntimeEnabled() noexcept { try {return Enabled(State());} catch (...) {return false;} }
const char* RuntimeSessionId() noexcept {
    try { auto& state=State();return state.driver.load(std::memory_order_acquire) ? state.session_id.c_str() : nullptr; }
    catch (...) { return nullptr; }
}
void RuntimeDeferredLoss() noexcept {
    // State is initialized before installing the hook; never lock or call game code here.
    try {State().deferred_loss.store(true,std::memory_order_release);} catch (...) {}
}
void RuntimeAbandon() noexcept {
    try {
        auto& state=State();state.terminal.store(true,std::memory_order_release);
        state.deferred_loss.store(true,std::memory_order_release);
        if (auto* driver=state.driver.load(std::memory_order_acquire)) driver->MarkLost();
        if (state.viewer) state.viewer->Stop();
    } catch (...) {}
}
void RuntimeDeviceCreated(IDirect3DDevice8* device) noexcept {
    LastError saved;
    try {
        auto& state=State();std::lock_guard<std::mutex> lock(state.device_mutex);
        if (!Enabled(state) || !device) return;
        auto* old=state.device.load(std::memory_order_acquire);
        if (old && old!=device) {RuntimeAbandon();return;}
        state.device.store(device,std::memory_order_release);
    } catch (...) {RuntimeAbandon();}
}
void RuntimeDeviceReleased(IDirect3DDevice8* device) noexcept {
    LastError saved;
    try {
        auto& state=State();std::lock_guard<std::mutex> lock(state.device_mutex);
        if (state.device.load(std::memory_order_acquire)!=device) return;
        state.device.store(nullptr,std::memory_order_release);
        if (auto* driver=state.driver.load(std::memory_order_acquire)) {
            // Device destruction retires the consumer epoch even without a D3D8 Reset.
            const auto ticket=driver->BeginReset(CurrentThread());
            if (!ticket) {RuntimeAbandon();return;}
            driver->EndReset(ticket,CurrentThread(),true);
        }
        NotifyViewer();
    } catch (...) {RuntimeAbandon();}
}
std::uint64_t RuntimeProbeBegin(std::uint32_t thread_id) noexcept {
    LastError saved;
    try {
        if (querying) {RuntimeDeferredLoss();return 0;}
        auto* driver=Driver(true);const auto thread=CurrentThread();
        if (!driver) return 0;
        if (!thread.id || thread.id!=thread_id) {driver->MarkLost();return 0;}
        return driver->BeginProbe(thread);
    } catch (...) {RuntimeAbandon();return 0;}
}
void RuntimeProbeEnd(std::uint64_t ticket,const scene_bridge::ProbeSnapshot& snapshot) noexcept {
    LastError saved;
    try {
        if (!ticket) return;
        if (querying) RuntimeDeferredLoss();
        auto* driver=Driver(false);if (!driver) return;
        const auto thread=CurrentThread();driver->EndProbe(ticket,thread,snapshot);
        auto& state=State();
        if (!state.light_candidates_enabled) return;
        std::uint64_t generation=0;
        const auto light_ticket=driver->BeginLightSample(thread,&generation);
        if (!light_ticket) return;
        const scene_bridge::ProbeReader reader{nullptr,ReadMemory};
        const auto sample=generation ? scene_bridge::SampleVisibleLightCandidates(reader,snapshot.self,state.map.c_str()) :
                                      scene_bridge::VisibleLightCandidates{};
        std::vector<VisualLight> lights;
        const bool ready=BuildVisibleLightCandidates(sample,state.map,state.map_sha256,generation,lights);
        const auto candle_link=ready && state.candle_link_diagnostics_enabled ?
            scene_bridge::SampleBedroomCandleLink(reader,snapshot,state.map.c_str(),state.candle_actor_number) : scene_bridge::CandleLinkObservation{};
        const auto other_candle_link=ready && state.candle_link_diagnostics_enabled ?
            scene_bridge::SampleBedroomCandleLink(reader,snapshot,state.map.c_str(),state.candle_actor_number==6 ? 14u : 6u) :
            scene_bridge::CandleLinkObservation{};
        const auto raw=ready && state.light_raw_diagnostics_enabled ?
            BuildRawLightBatch(sample,state.map,state.map_sha256,generation,lights,candle_link,state.candle_actor_number,other_candle_link) : nullptr;
        // A nested callback may have marked loss while memory was being read.
        driver=Driver(false);if (!driver) return;
        if (ready) (void)driver->EndLightSample(light_ticket,thread,true,LightCoverage::Partial,lights,raw);
        else (void)driver->CancelLightSample(light_ticket,thread);
    } catch (...) {RuntimeAbandon();}
}
void RuntimeSubmissionEnter(std::uint32_t element,std::uint32_t stage,std::uint32_t section) noexcept {
    LastError saved;
    try {
        if (scopes.depth==std::numeric_limits<unsigned>::max()) {RuntimeDeferredLoss();return;}
        const auto depth=scopes.depth++;
        if (depth>=std::size(scopes.items)) {RuntimeDeferredLoss();return;}
        auto& scope=scopes.items[depth];scope={};
        if (querying) {RuntimeDeferredLoss();return;}
        auto* driver=Driver(true);if (!driver) return;
        scope.ticket=driver->BeginScope(CurrentThread(),stage,section);
        if (!scope.ticket) return;
        auto sample=stage==1 ? Sample(element) : scene_bridge::SubmissionSample{};
        std::shared_ptr<const scene_bridge::ProbeSnapshot> probe;
        if (State().static_meshes_enabled && sample.valid)
            probe=driver->GenericProbe(scope.ticket,CurrentThread(),sample);
        auto prop=scene_bridge::BedroomProp::None;
        if (probe) {
            const scene_bridge::ProbeReader reader{nullptr,ReadMemory};
            char name[64]{};
            if (!scene_bridge::ReadProbeName(reader,sample.owner_fname,name)) probe.reset();
            else {
                prop=scene_bridge::IdentifyBedroomProp(sample.owner_fname>>16,name);
                if (prop==scene_bridge::BedroomProp::None && std::strcmp(name,"StaticMeshActor")!=0) probe.reset();
            }
        }
        if (probe) {
            scope.generic=true;
            const scene_bridge::ProbeReader reader{nullptr,ReadMemory};
            const auto observed=scene_bridge::SampleVisibleStaticMesh(reader,*probe,sample,prop);
            if (scene_bridge::AttachAssetCandidate(sample,observed.asset))
                scope.asset=std::make_unique<scene_bridge::AssetCandidate>(observed.asset);
            if (prop!=scene_bridge::BedroomProp::None) {scope.prop_probe=probe;scope.prop=prop;}
            scope.element=element;scope.sampled=sample.valid;
            (void)driver->SetScopeSample(scope.ticket,CurrentThread(),sample,&observed,prop);return;
        }
        scope.element=element;scope.sampled=sample.valid;
        (void)driver->SetScopeSample(scope.ticket,CurrentThread(),sample);
    } catch (...) {RuntimeAbandon();}
}
void RuntimeSubmissionLeave() noexcept {
    LastError saved;
    try {
        if (!scopes.depth) {RuntimeDeferredLoss();return;}
        const auto depth=--scopes.depth;
        if (depth>=std::size(scopes.items)) return;
        const auto scope=std::move(scopes.items[depth]);scopes.items[depth]={};
        if (!scope.ticket) return;
        const bool reentered=querying;
        if (reentered) RuntimeDeferredLoss();
        auto* driver=Driver(false);if (!driver) return;
        auto after=scope.sampled && !reentered ? Sample(scope.element) : scene_bridge::SubmissionSample{};
        if (scope.generic && after.valid && scope.asset) {
            const scene_bridge::AssetReader reader{nullptr,ReadMemory};
            if (scene_bridge::MatchesAssetCandidate(reader,after.owner,*scope.asset))
                (void)scene_bridge::AttachAssetCandidate(after,*scope.asset);
        }
        if (scope.prop_probe && after.valid) {
            const scene_bridge::ProbeReader reader{nullptr,ReadMemory};
            const auto observed=scene_bridge::SampleVisibleStaticMesh(reader,*scope.prop_probe,after,scope.prop);
            driver->EndScope(scope.ticket,CurrentThread(),after,&observed);
        } else driver->EndScope(scope.ticket,CurrentThread(),after);
    } catch (...) {RuntimeAbandon();}
}
RuntimeDraw RuntimeDrawBegin(IDirect3DDevice8* device,const DrawCall& call,bool clear,const volatile void* stack_marker) noexcept {
    LastError saved;
    try {
        if (querying) {RuntimeDeferredLoss();return {};}
        auto* driver=Driver(true);if (!driver || !IsDevice(device)) return {};
        std::uint64_t scope=0;
        if (scopes.depth) {
            if (scopes.depth>std::size(scopes.items)) {RuntimeDeferredLoss();return {};}
            scope=scopes.items[scopes.depth-1].ticket;
        }
        IndexedDraw args;args.indexed=call.kind==DrawKind::IndexedPrimitive;
        args.triangle_list=call.primitive_type==D3DPT_TRIANGLELIST;
        args.first_index=call.start_index;args.triangle_count=call.primitive_count;
        args.min_vertex=call.min_vertex_index;args.vertex_count=call.num_vertices;
        ObservedDrawKind kind=ObservedDrawKind::Unknown;
        if (clear) kind=ObservedDrawKind::Clear;
        else switch (call.kind) {
        case DrawKind::Primitive:kind=ObservedDrawKind::Primitive;break;
        case DrawKind::IndexedPrimitive:kind=ObservedDrawKind::IndexedPrimitive;break;
        case DrawKind::PrimitiveUP:kind=ObservedDrawKind::PrimitiveUP;break;
        case DrawKind::IndexedPrimitiveUP:kind=ObservedDrawKind::IndexedPrimitiveUP;break;
        }
        const auto start=driver->BeginDraw(CurrentThread(),scope,args,kind,CaptureStackCursor(stack_marker));
        RuntimeDraw result;result.ticket=start.ticket;result.needs_state=start.needs_state;result.needs_world=start.needs_world;
        if (result.needs_state) (void)Query(device,result.before,result.needs_world);
        return result;
    } catch (...) {RuntimeAbandon();return {};}
}
void RuntimeDrawEnd(IDirect3DDevice8* device,const RuntimeDraw& draw,HRESULT result) noexcept {
    LastError saved;
    try {
        if (!draw.ticket) return;
        DrawObservation after;
        if (draw.needs_state && draw.before.available && IsDevice(device)) (void)Query(device,after,draw.needs_world);
        auto* driver=Driver(false);if (driver) driver->EndDraw(draw.ticket,CurrentThread(),SUCCEEDED(result),draw.before,after);
    } catch (...) {RuntimeAbandon();}
}
RuntimePresent RuntimePresentBegin(IDirect3DDevice8* device,const volatile void* stack_marker) noexcept {
    LastError saved;
    try {
        if (querying) {RuntimeDeferredLoss();return {};}
        auto* d=Driver(true);RuntimePresent present;
        if (d && IsDevice(device)) {
            PresentClockSample clock;
            if (State().present_clock_enabled) {
                LARGE_INTEGER value{};
                clock.available=QueryPerformanceCounter(&value) && value.QuadPart>=0;
                if (clock.available) clock.ticks=static_cast<std::uint64_t>(value.QuadPart);
            }
            present.ticket=d->BeginPresent(CurrentThread(),&present.frame_id,clock,CaptureStackCursor(stack_marker));
        }
        return present;
    } catch (...) {RuntimeAbandon();return {};}
}
void RuntimePresentEnd(RuntimePresent present,HRESULT result) noexcept {
    LastError saved;
    try {if (!present.ticket) return;auto* d=Driver(false);if (d) (void)d->EndPresent(present.ticket,CurrentThread(),SUCCEEDED(result));NotifyViewer();}
    catch (...) {RuntimeAbandon();}
}
std::uint64_t RuntimeResetBegin(IDirect3DDevice8* device) noexcept {
    LastError saved;
    try {if (querying) {RuntimeAbandon();return 0;} auto* d=Driver(true);const auto ticket=d && IsDevice(device) ? d->BeginReset(CurrentThread()) : 0;NotifyViewer();return ticket;}
    catch (...) {RuntimeAbandon();return 0;}
}
void RuntimeResetEnd(std::uint64_t ticket,HRESULT result) noexcept {
    LastError saved;
    try {if (!ticket) return;auto* d=Driver(false);if (d) d->EndReset(ticket,CurrentThread(),SUCCEEDED(result));}
    catch (...) {RuntimeAbandon();}
}
std::shared_ptr<const Snapshot> RuntimeCurrent() noexcept {
    LastError saved;
    try {auto* d=Driver(true);return d ? d->current() : nullptr;} catch (...) {return {};}
}
} // namespace t3render::live
