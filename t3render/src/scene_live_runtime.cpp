#include "scene_live_runtime.hpp"
#include "scene_live_catalog_win.hpp"
#include "scene_live_visible_lights.hpp"
#include "scene_live_viewer.hpp"

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
bool Query(IDirect3DDevice8* device,DrawObservation& observation) noexcept {
    if (!device) return false;
    QueryGuard guard;if (!guard.entered) return false;
    D3DMATRIX view{},projection{},world{};D3DVIEWPORT8 viewport{};
    static_assert(sizeof(D3DMATRIX)==sizeof(float)*16,"unexpected D3D8 matrix size");
    if (FAILED(device->GetTransform(D3DTS_VIEW,&view)) || FAILED(device->GetTransform(D3DTS_PROJECTION,&projection)) ||
        FAILED(device->GetTransform(D3DTS_WORLD,&world)) || FAILED(device->GetViewport(&viewport))) return false;
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
            LoadedCatalog catalog;std::string session;
            if (path.empty() || !LoadCatalogFile(path.c_str(),catalog) || !SessionId(session)) return;
            FrameProvenance source;source.origin=FrameOrigin::LiveProcess;source.session_id=session;
            auto bed_override=catalog.bed_override;
            auto bedroom_materials=catalog.bedroom_materials;
            state.map=catalog.binding.map;state.map_sha256=catalog.binding.map_sha256;
            auto driver=SceneDriver::Create(std::move(catalog),std::move(source),0,1);
            if (!driver) return;
            state.static_meshes_enabled=Environment(L"T3RENDER_SCENE_LIVE_STATIC_MESHES")==L"1";
            state.light_candidates_enabled=Environment(L"T3RENDER_SCENE_LIVE_LIGHTS")==L"1";
            const bool viewer_enabled=Environment(L"T3RENDER_SCENE_LIVE_DX12")==L"1";
            const auto diagnostic_directory=viewer_enabled ? Environment(L"T3RENDER_SCENE_LIVE_DIAGNOSTIC_DIR") : std::wstring{};
            state.light_raw_diagnostics_enabled=state.light_candidates_enabled && !diagnostic_directory.empty();
            state.session_id=std::move(session);
            state.owned=std::move(driver);state.driver.store(state.owned.get(),std::memory_order_release);
            if (viewer_enabled) {
                ViewerProvider provider;provider.context=std::shared_ptr<void>(&state,[](void*){});
                provider.current=ViewerCurrent;provider.submit_if_current=ViewerSubmit;
                provider.driver_stats=ViewerDriverStats;
                provider.light_diagnostics=state.light_raw_diagnostics_enabled ? ViewerLightDiagnostics : nullptr;
                provider.bed_override=std::move(bed_override);
                provider.bedroom_materials=std::move(bedroom_materials);
                provider.bedroom_material_ids=provider.bedroom_materials ||
                    Environment(L"T3RENDER_SCENE_LIVE_BEDROOM_MATERIAL_IDS")==L"1";
                provider.diagnostic_directory=diagnostic_directory;
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
        const auto raw=ready && state.light_raw_diagnostics_enabled ?
            BuildRawLightBatch(sample,state.map,state.map_sha256,generation,lights) : nullptr;
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
        if (probe) {
            const scene_bridge::ProbeReader reader{nullptr,ReadMemory};
            char name[64]{};
            if (!scene_bridge::ReadProbeName(reader,sample.owner_fname,name) || std::strcmp(name,"StaticMeshActor")!=0) probe.reset();
        }
        if (probe) {
            scope.generic=true;
            const scene_bridge::ProbeReader reader{nullptr,ReadMemory};
            const auto observed=scene_bridge::SampleVisibleStaticMesh(reader,*probe,sample);
            if (scene_bridge::AttachAssetCandidate(sample,observed.asset))
                scope.asset=std::make_unique<scene_bridge::AssetCandidate>(observed.asset);
            scope.element=element;scope.sampled=sample.valid;
            (void)driver->SetScopeSample(scope.ticket,CurrentThread(),sample,&observed);return;
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
        driver->EndScope(scope.ticket,CurrentThread(),after);
    } catch (...) {RuntimeAbandon();}
}
RuntimeDraw RuntimeDrawBegin(IDirect3DDevice8* device,const DrawCall& call) noexcept {
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
        const auto start=driver->BeginDraw(CurrentThread(),scope,args);
        RuntimeDraw result;result.ticket=start.ticket;result.needs_state=start.needs_state;
        if (result.needs_state) (void)Query(device,result.before);
        return result;
    } catch (...) {RuntimeAbandon();return {};}
}
void RuntimeDrawEnd(IDirect3DDevice8* device,const RuntimeDraw& draw,HRESULT result) noexcept {
    LastError saved;
    try {
        if (!draw.ticket) return;
        DrawObservation after;
        if (draw.needs_state && draw.before.available && IsDevice(device)) (void)Query(device,after);
        auto* driver=Driver(false);if (driver) driver->EndDraw(draw.ticket,CurrentThread(),SUCCEEDED(result),draw.before,after);
    } catch (...) {RuntimeAbandon();}
}
RuntimePresent RuntimePresentBegin(IDirect3DDevice8* device) noexcept {
    LastError saved;
    try {
        if (querying) {RuntimeDeferredLoss();return {};}
        auto* d=Driver(true);RuntimePresent present;
        if (d && IsDevice(device)) present.ticket=d->BeginPresent(CurrentThread(),&present.frame_id);
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
