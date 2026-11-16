#include "scene_live_runtime.hpp"
#include "scene_live_catalog_win.hpp"

#include <windows.h>
#include <wincrypt.h>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace t3render::live {
namespace {
static_assert(std::atomic<bool>::is_always_lock_free,"nested probe loss must be lock-free");
struct RuntimeState {
    std::mutex device_mutex;
    std::unique_ptr<SceneDriver> owned;
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
struct Scope { std::uint64_t ticket=0;std::uint32_t element=0;bool sampled=false; };
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
bool ReadMemory(void*,std::uint32_t address,void* output,std::size_t size) noexcept {
    SIZE_T copied=0;
    return address && output && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),output,size,&copied) && copied==size;
}
scene_bridge::SubmissionSample Sample(std::uint32_t element) noexcept {
    const scene_bridge::ProbeReader reader{nullptr,ReadMemory};
    return scene_bridge::SampleSubmission(reader,element);
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
            FrameProvenance source;source.origin=FrameOrigin::LiveProcess;source.session_id=std::move(session);
            auto driver=SceneDriver::Create(std::move(catalog),std::move(source),0,1);
            if (!driver) return;
            state.owned=std::move(driver);state.driver.store(state.owned.get(),std::memory_order_release);
        });
        return Enabled(State());
    } catch (...) { return false; }
}
bool RuntimeEnabled() noexcept { try {return Enabled(State());} catch (...) {return false;} }
void RuntimeDeferredLoss() noexcept {
    // State is initialized before installing the hook; never lock or call game code here.
    try {State().deferred_loss.store(true,std::memory_order_release);} catch (...) {}
}
void RuntimeAbandon() noexcept {
    try {
        auto& state=State();state.terminal.store(true,std::memory_order_release);
        state.deferred_loss.store(true,std::memory_order_release);
        if (auto* driver=state.driver.load(std::memory_order_acquire)) driver->MarkLost();
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
        auto* driver=Driver(false);if (driver) driver->EndProbe(ticket,CurrentThread(),snapshot);
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
        const auto sample=stage==1 ? Sample(element) : scene_bridge::SubmissionSample{};
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
        const auto scope=scopes.items[depth];scopes.items[depth]={};
        if (!scope.ticket) return;
        const bool reentered=querying;
        if (reentered) RuntimeDeferredLoss();
        auto* driver=Driver(false);if (!driver) return;
        const auto after=scope.sampled && !reentered ? Sample(scope.element) : scene_bridge::SubmissionSample{};
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
std::uint64_t RuntimePresentBegin(IDirect3DDevice8* device) noexcept {
    LastError saved;
    try {if (querying) {RuntimeDeferredLoss();return 0;} auto* d=Driver(true);return d && IsDevice(device) ? d->BeginPresent(CurrentThread()) : 0;}
    catch (...) {RuntimeAbandon();return 0;}
}
void RuntimePresentEnd(std::uint64_t ticket,HRESULT result) noexcept {
    LastError saved;
    try {if (!ticket) return;auto* d=Driver(false);if (d) (void)d->EndPresent(ticket,CurrentThread(),SUCCEEDED(result));}
    catch (...) {RuntimeAbandon();}
}
std::uint64_t RuntimeResetBegin(IDirect3DDevice8* device) noexcept {
    LastError saved;
    try {if (querying) {RuntimeAbandon();return 0;} auto* d=Driver(true);return d && IsDevice(device) ? d->BeginReset(CurrentThread()) : 0;}
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
