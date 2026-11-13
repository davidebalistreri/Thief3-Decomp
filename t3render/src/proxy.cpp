// Opt-in, observational D3D8 proxy. Objects and unobserved methods remain the
// runtime's own; per-object vtables intercept only factory/device lifecycle and
// draw boundaries. See README for limitations and deployment precautions.
#include "capture.hpp"
#include "buffer_proxy.hpp"
#include "vtable_layout.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#if defined(T3RENDER_STATEBLOCK_TEST_FIXTURE) && !defined(T3RENDER_PROXY_TEST) && !defined(T3RENDER_STATEBLOCK_DLL_TEST)
#error The state-block experiment is restricted to explicitly designated test builds.
#endif

extern "C" IDirect3D8* WINAPI Proxy_Direct3DCreate8(UINT);

namespace {

using t3render::DrawCall;
using t3render::DrawKind;

// Derive call-through signatures directly from the SDK C++ interfaces. This
// preserves the x86 stdcall convention and rejects signature drift at compile time.
template <typename> struct MethodFunction;
template <typename R, typename C, typename... Args>
struct MethodFunction<R(STDMETHODCALLTYPE C::*)(Args...)> {
    using type = R(STDMETHODCALLTYPE*)(C*, Args...);
};
template <typename T> using Function = typename MethodFunction<T>::type;

using QueryFactory = Function<decltype(&IDirect3D8::QueryInterface)>;
using ReleaseFactory = Function<decltype(&IDirect3D8::Release)>;
using CreateDevice = Function<decltype(&IDirect3D8::CreateDevice)>;
using QueryDevice = Function<decltype(&IDirect3DDevice8::QueryInterface)>;
using ReleaseDevice = Function<decltype(&IDirect3DDevice8::Release)>;
using ResetDevice = Function<decltype(&IDirect3DDevice8::Reset)>;
using PresentDevice = Function<decltype(&IDirect3DDevice8::Present)>;
using ClearDevice = Function<decltype(&IDirect3DDevice8::Clear)>;
using DrawPrimitive = Function<decltype(&IDirect3DDevice8::DrawPrimitive)>;
using DrawIndexed = Function<decltype(&IDirect3DDevice8::DrawIndexedPrimitive)>;
using DrawPrimitiveUp = Function<decltype(&IDirect3DDevice8::DrawPrimitiveUP)>;
using DrawIndexedUp = Function<decltype(&IDirect3DDevice8::DrawIndexedPrimitiveUP)>;
using CreateVertexBuffer = Function<decltype(&IDirect3DDevice8::CreateVertexBuffer)>;
using CreateIndexBuffer = Function<decltype(&IDirect3DDevice8::CreateIndexBuffer)>;
using SetStreamSource = Function<decltype(&IDirect3DDevice8::SetStreamSource)>;
using GetStreamSource = Function<decltype(&IDirect3DDevice8::GetStreamSource)>;
using SetIndices = Function<decltype(&IDirect3DDevice8::SetIndices)>;
using GetIndices = Function<decltype(&IDirect3DDevice8::GetIndices)>;
using ProcessVertices = Function<decltype(&IDirect3DDevice8::ProcessVertices)>;
using BeginStateBlock = Function<decltype(&IDirect3DDevice8::BeginStateBlock)>;
using EndStateBlock = Function<decltype(&IDirect3DDevice8::EndStateBlock)>;
using ApplyStateBlock = Function<decltype(&IDirect3DDevice8::ApplyStateBlock)>;
using CaptureStateBlock = Function<decltype(&IDirect3DDevice8::CaptureStateBlock)>;
using DeleteStateBlock = Function<decltype(&IDirect3DDevice8::DeleteStateBlock)>;
using CreateStateBlock = Function<decltype(&IDirect3DDevice8::CreateStateBlock)>;
using CreateVertexShader = Function<decltype(&IDirect3DDevice8::CreateVertexShader)>;
using SetVertexShader = Function<decltype(&IDirect3DDevice8::SetVertexShader)>;
using GetVertexShader = Function<decltype(&IDirect3DDevice8::GetVertexShader)>;
using DeleteVertexShader = Function<decltype(&IDirect3DDevice8::DeleteVertexShader)>;

HRESULT STDMETHODCALLTYPE FactoryQuery(IDirect3D8*, REFIID, void**);
ULONG STDMETHODCALLTYPE FactoryRelease(IDirect3D8*);
HRESULT STDMETHODCALLTYPE FactoryCreateDevice(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD,
                                               D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
HRESULT STDMETHODCALLTYPE DeviceQuery(IDirect3DDevice8*, REFIID, void**);
ULONG STDMETHODCALLTYPE DeviceRelease(IDirect3DDevice8*);
HRESULT STDMETHODCALLTYPE DeviceReset(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
HRESULT STDMETHODCALLTYPE DevicePresent(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
HRESULT STDMETHODCALLTYPE DeviceClear(IDirect3DDevice8*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
HRESULT STDMETHODCALLTYPE DeviceDraw(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
HRESULT STDMETHODCALLTYPE DeviceDrawIndexed(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
HRESULT STDMETHODCALLTYPE DeviceDrawUp(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
HRESULT STDMETHODCALLTYPE DeviceDrawIndexedUp(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                               const void*, D3DFORMAT, const void*, UINT);
HRESULT STDMETHODCALLTYPE DeviceCreateVertexBuffer(IDirect3DDevice8*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer8**);
HRESULT STDMETHODCALLTYPE DeviceCreateIndexBuffer(IDirect3DDevice8*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer8**);
HRESULT STDMETHODCALLTYPE DeviceSetStreamSource(IDirect3DDevice8*, UINT, IDirect3DVertexBuffer8*, UINT);
HRESULT STDMETHODCALLTYPE DeviceGetStreamSource(IDirect3DDevice8*, UINT, IDirect3DVertexBuffer8**, UINT*);
HRESULT STDMETHODCALLTYPE DeviceSetIndices(IDirect3DDevice8*, IDirect3DIndexBuffer8*, UINT);
HRESULT STDMETHODCALLTYPE DeviceGetIndices(IDirect3DDevice8*, IDirect3DIndexBuffer8**, UINT*);
HRESULT STDMETHODCALLTYPE DeviceProcessVertices(IDirect3DDevice8*, UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD);
HRESULT STDMETHODCALLTYPE DeviceCreateVertexShader(IDirect3DDevice8*, const DWORD*, const DWORD*, DWORD*, DWORD);
HRESULT STDMETHODCALLTYPE DeviceSetVertexShader(IDirect3DDevice8*, DWORD);
HRESULT STDMETHODCALLTYPE DeviceDeleteVertexShader(IDirect3DDevice8*, DWORD);
HRESULT STDMETHODCALLTYPE DeviceBeginStateBlock(IDirect3DDevice8*);
HRESULT STDMETHODCALLTYPE DeviceEndStateBlock(IDirect3DDevice8*, DWORD*);
HRESULT STDMETHODCALLTYPE DeviceApplyStateBlock(IDirect3DDevice8*, DWORD);
HRESULT STDMETHODCALLTYPE DeviceCaptureStateBlock(IDirect3DDevice8*, DWORD);
HRESULT STDMETHODCALLTYPE DeviceDeleteStateBlock(IDirect3DDevice8*, DWORD);
HRESULT STDMETHODCALLTYPE DeviceCreateStateBlock(IDirect3DDevice8*, D3DSTATEBLOCKTYPE, DWORD*);

static_assert(std::is_same_v<QueryFactory, decltype(&FactoryQuery)>);
static_assert(std::is_same_v<ReleaseFactory, decltype(&FactoryRelease)>);
static_assert(std::is_same_v<CreateDevice, decltype(&FactoryCreateDevice)>);
static_assert(std::is_same_v<QueryDevice, decltype(&DeviceQuery)>);
static_assert(std::is_same_v<ReleaseDevice, decltype(&DeviceRelease)>);
static_assert(std::is_same_v<ResetDevice, decltype(&DeviceReset)>);
static_assert(std::is_same_v<PresentDevice, decltype(&DevicePresent)>);
static_assert(std::is_same_v<ClearDevice, decltype(&DeviceClear)>);
static_assert(std::is_same_v<DrawPrimitive, decltype(&DeviceDraw)>);
static_assert(std::is_same_v<DrawIndexed, decltype(&DeviceDrawIndexed)>);
static_assert(std::is_same_v<DrawPrimitiveUp, decltype(&DeviceDrawUp)>);
static_assert(std::is_same_v<DrawIndexedUp, decltype(&DeviceDrawIndexedUp)>);
static_assert(std::is_same_v<CreateVertexBuffer, decltype(&DeviceCreateVertexBuffer)>);
static_assert(std::is_same_v<CreateIndexBuffer, decltype(&DeviceCreateIndexBuffer)>);
static_assert(std::is_same_v<SetStreamSource, decltype(&DeviceSetStreamSource)>);
static_assert(std::is_same_v<GetStreamSource, decltype(&DeviceGetStreamSource)>);
static_assert(std::is_same_v<SetIndices, decltype(&DeviceSetIndices)>);
static_assert(std::is_same_v<GetIndices, decltype(&DeviceGetIndices)>);
static_assert(std::is_same_v<ProcessVertices, decltype(&DeviceProcessVertices)>);
static_assert(std::is_same_v<CreateVertexShader, decltype(&DeviceCreateVertexShader)>);
static_assert(std::is_same_v<SetVertexShader, decltype(&DeviceSetVertexShader)>);
static_assert(std::is_same_v<DeleteVertexShader, decltype(&DeviceDeleteVertexShader)>);
static_assert(std::is_same_v<BeginStateBlock, decltype(&DeviceBeginStateBlock)>);
static_assert(std::is_same_v<EndStateBlock, decltype(&DeviceEndStateBlock)>);
static_assert(std::is_same_v<ApplyStateBlock, decltype(&DeviceApplyStateBlock)>);
static_assert(std::is_same_v<CaptureStateBlock, decltype(&DeviceCaptureStateBlock)>);
static_assert(std::is_same_v<DeleteStateBlock, decltype(&DeviceDeleteStateBlock)>);
static_assert(std::is_same_v<CreateStateBlock, decltype(&DeviceCreateStateBlock)>);

const T3VtableLayout& Layout() {
    static const T3VtableLayout layout = T3GetVtableLayout();
    return layout;
}

struct Binding {
    std::vector<void*> original;
    std::vector<void*> instrumented;
    std::shared_ptr<t3render::buffers::BufferProxy> buffers;
    std::atomic<bool> buffer_observing{false};
    std::atomic<bool> retired{false}, deferred_buffer_invalidation{false}, deferred_capture_release{false};
    std::shared_ptr<t3render::ir::VertexDeclarationRegistry> declarations;
    std::uint64_t declaration_device = 0;
    bool capture_vertex_programs = false;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<void*, std::shared_ptr<Binding>> factories;
    std::unordered_map<void*, std::shared_ptr<Binding>> devices;
    std::shared_ptr<t3render::buffers::BufferProxy> buffers;
    std::shared_ptr<t3render::ir::VertexDeclarationRegistry> declarations;
    bool capture_vertex_programs = false;
};

// Deliberately process-lifetime: COM objects may still be released during
// shutdown of other modules. Each entry itself is removed at final Release.
Registry* GetRegistry() noexcept {
    static Registry* registry = []() noexcept -> Registry* {
        try {
            return new Registry;
        } catch (...) {
            return nullptr;
        }
    }();
    return registry;
}

using ObjectMap = std::unordered_map<void*, std::shared_ptr<Binding>>;

std::shared_ptr<Binding> Find(void* object, bool device) noexcept {
    Registry* registry = GetRegistry();
    if (!registry) return {};
    std::lock_guard<std::mutex> lock(registry->mutex);
    auto& objects = device ? registry->devices : registry->factories;
    const auto it = objects.find(object);
    return it == objects.end() || it->second->retired.load(std::memory_order_acquire)
        ? std::shared_ptr<Binding>{} : it->second;
}

void Forget(void* object, bool device, const std::shared_ptr<Binding>& binding) noexcept {
    Registry* registry = GetRegistry();
    if (!registry) return;
    ObjectMap::node_type retired;
    {
        std::lock_guard<std::mutex> lock(registry->mutex);
        auto& objects = device ? registry->devices : registry->factories;
        const auto it = objects.find(object);
        // Extract without freeing under the map mutex. Destruct after unlocking.
        if (it != objects.end() && it->second == binding) retired = objects.extract(it);
    }
}

void NotifyDeferredRelease(IDirect3DDevice8*, const std::shared_ptr<Binding>&) noexcept;
thread_local bool sweeping_retired = false;

void SweepRetiredBindings() noexcept {
    if (sweeping_retired) return;
    struct SweepGuard { SweepGuard() { sweeping_retired = true; } ~SweepGuard() { sweeping_retired = false; } } guard;
    Registry* registry = GetRegistry();
    if (!registry) return;
    for (;;) {
        ObjectMap::node_type retired;
        bool device = false;
        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            auto found = std::find_if(registry->devices.begin(), registry->devices.end(),
                [](const auto& item) { return item.second->retired.load(std::memory_order_acquire); });
            if (found != registry->devices.end()) {
                retired = registry->devices.extract(found);
                device = true;
            } else {
                auto factory = std::find_if(registry->factories.begin(), registry->factories.end(),
                    [](const auto& item) { return item.second->retired.load(std::memory_order_acquire); });
                if (factory != registry->factories.end()) retired = registry->factories.extract(factory);
            }
        }
        if (retired.empty()) return;
        auto binding = retired.mapped();
        if (device) {
            auto* device_id = static_cast<IDirect3DDevice8*>(retired.key());
            if (binding->buffers && binding->deferred_buffer_invalidation.exchange(false))
                binding->buffers->InvalidateDevice(device_id);
            if (binding->deferred_capture_release.exchange(false)) NotifyDeferredRelease(device_id, binding);
        }
        // Both node and binding destruct after the capture and map locks.
    }
}

bool WritableObject(void* object) noexcept {
    if (!object || (reinterpret_cast<uintptr_t>(object) % alignof(void*)) != 0) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(object, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = memory.Protect & 0xff;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(object);
    const uintptr_t end = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return start <= end && end - start >= sizeof(void*);
}

void Diagnostic(const char* message) noexcept {
    OutputDebugStringA(message);
}

// Prepare and register the copy before swapping it in, so a call racing the
// swap can always find its original slot. Never change memory protection on a
// runtime object; if the pointer is not writable, leave the object untouched.
bool Bind(void* object, bool device, bool track_declarations = true) noexcept {
    try {
        Registry* registry = GetRegistry();
        if (!registry || !WritableObject(object)) return false;
        const T3VtableLayout& l = Layout();
        const size_t count = device ? l.device_size : l.factory_size;
        std::shared_ptr<t3render::ir::VertexDeclarationRegistry> declarations;
        if (device && track_declarations) {
            std::lock_guard<std::mutex> lock(registry->mutex);
            if (!registry->buffers) declarations = registry->declarations;
        }
        // Open and retire tokens outside the object-map mutex: registry cleanup
        // can release payload memory and a user allocator can reenter us.
        struct DeviceToken {
            std::shared_ptr<t3render::ir::VertexDeclarationRegistry> registry;
            std::uint64_t id = 0;
            ~DeviceToken() { if (registry && id) registry->CloseDevice(id); }
        } declaration_token{declarations, declarations ? declarations->OpenDevice() : 0};
        auto destination = reinterpret_cast<void* volatile*>(object);
        std::lock_guard<std::mutex> lock(registry->mutex);
        auto& objects = device ? registry->devices : registry->factories;
        auto current = reinterpret_cast<void**>(InterlockedCompareExchangePointer(destination, nullptr, nullptr));
        if (!current) return false;
        const auto existing = objects.find(object);
        if (existing != objects.end()) {
            if (current == existing->second->instrumented.data()) return true;
            // An alias outlived its registry entry or another layer changed the
            // vtable. Do not replace an unknown, already-live hook chain.
            return false;
        }
        auto binding = std::make_shared<Binding>();
        binding->original.assign(current, current + count);
        binding->instrumented = binding->original;
        if (device) {
            binding->buffers = registry->buffers;
            binding->buffer_observing.store(binding->buffers != nullptr, std::memory_order_relaxed);
            // Buffer wrappers can return a distinct native device alias via
            // GetDevice without notifying this registry. Never combine the two
            // observers until alias propagation is implemented and verified.
            binding->declarations = binding->buffers ? nullptr : declarations;
            binding->declaration_device = binding->declarations ? declaration_token.id : 0;
            binding->capture_vertex_programs = binding->declarations && registry->capture_vertex_programs;
        }
        auto& v = binding->instrumented;
        if (device) {
            v[l.device_query] = reinterpret_cast<void*>(&DeviceQuery);
            v[l.device_release] = reinterpret_cast<void*>(&DeviceRelease);
            v[l.device_reset] = reinterpret_cast<void*>(&DeviceReset);
            v[l.device_present] = reinterpret_cast<void*>(&DevicePresent);
            v[l.device_clear] = reinterpret_cast<void*>(&DeviceClear);
            v[l.device_draw] = reinterpret_cast<void*>(&DeviceDraw);
            v[l.device_draw_indexed] = reinterpret_cast<void*>(&DeviceDrawIndexed);
            v[l.device_draw_up] = reinterpret_cast<void*>(&DeviceDrawUp);
            v[l.device_draw_indexed_up] = reinterpret_cast<void*>(&DeviceDrawIndexedUp);
            if (binding->buffers) {
                v[l.device_create_vertex_buffer] = reinterpret_cast<void*>(&DeviceCreateVertexBuffer);
                v[l.device_create_index_buffer] = reinterpret_cast<void*>(&DeviceCreateIndexBuffer);
                v[l.device_set_stream] = reinterpret_cast<void*>(&DeviceSetStreamSource);
                v[l.device_get_stream] = reinterpret_cast<void*>(&DeviceGetStreamSource);
                v[l.device_set_indices] = reinterpret_cast<void*>(&DeviceSetIndices);
                v[l.device_get_indices] = reinterpret_cast<void*>(&DeviceGetIndices);
                v[l.device_process_vertices] = reinterpret_cast<void*>(&DeviceProcessVertices);
            }
            if (binding->declarations) {
                v[l.device_create_vertex_shader] = reinterpret_cast<void*>(&DeviceCreateVertexShader);
                v[l.device_set_vertex_shader] = reinterpret_cast<void*>(&DeviceSetVertexShader);
                v[l.device_delete_vertex_shader] = reinterpret_cast<void*>(&DeviceDeleteVertexShader);
            }
            if (binding->buffers || binding->declarations) {
                v[l.device_begin_state_block] = reinterpret_cast<void*>(&DeviceBeginStateBlock);
                v[l.device_end_state_block] = reinterpret_cast<void*>(&DeviceEndStateBlock);
                v[l.device_apply_state_block] = reinterpret_cast<void*>(&DeviceApplyStateBlock);
                v[l.device_capture_state_block] = reinterpret_cast<void*>(&DeviceCaptureStateBlock);
                v[l.device_delete_state_block] = reinterpret_cast<void*>(&DeviceDeleteStateBlock);
                v[l.device_create_state_block] = reinterpret_cast<void*>(&DeviceCreateStateBlock);
            }
        } else {
            v[l.factory_query] = reinterpret_cast<void*>(&FactoryQuery);
            v[l.factory_release] = reinterpret_cast<void*>(&FactoryRelease);
            v[l.factory_create_device] = reinterpret_cast<void*>(&FactoryCreateDevice);
        }
        objects.emplace(object, binding);
        if (InterlockedCompareExchangePointer(destination, v.data(), current) != current) {
            objects.erase(object);
            return false;
        }
        if (binding->declarations) declaration_token.id = 0; // binding owns this lifetime
        return true;
    } catch (...) {
        return false;
    }
}

// Capture must not change the result of the call the application made.
template <typename Function>
void Observe(Function&& function) noexcept {
    const DWORD previous_error = GetLastError();
    try {
        function();
    } catch (...) {
        Diagnostic("T3Render: capture raised an exception; continuing with the real runtime\n");
    }
    SetLastError(previous_error);
}

// Preserve the caller's last-error across binding lookup and restore the real
// method's last-error after capture and shared-pointer cleanup.
struct ErrorState {
    DWORD value = GetLastError();
    ~ErrorState() { SetLastError(value); }
    void Restore() const { SetLastError(value); }
    void Capture() { value = GetLastError(); }
};

// Buffer observation is independent of the normal capture opt-in. The reader
// never dereferences an application's lock pointer; incomplete kernel copies
// are rejected by the buffer manager. No additional D3D buffer locks occur.
bool ReadBufferSnapshot(void*, const void* source, void* destination, std::size_t length) {
    if (!source || !destination) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), source, destination, length, &copied) && copied == length;
}

bool ReadObservedVertex(void* context, IDirect3DDevice8* device, IDirect3DVertexBuffer8* native,
                        std::size_t offset, void* destination, std::size_t length) {
    auto* manager = static_cast<t3render::buffers::BufferProxy*>(context);
    return manager && manager->ReadVertex(device, native, offset, destination, length) == t3render::ir::RegistryResult::Ok;
}
bool ReadObservedIndex(void* context, IDirect3DDevice8* device, IDirect3DIndexBuffer8* native,
                       std::size_t offset, void* destination, std::size_t length) {
    auto* manager = static_cast<t3render::buffers::BufferProxy*>(context);
    return manager && manager->ReadIndex(device, native, offset, destination, length) == t3render::ir::RegistryResult::Ok;
}

void EnableBufferObserver() noexcept {
    try {
        wchar_t option[2]{};
        if (GetEnvironmentVariableW(L"T3RENDER_CAPTURE_BUFFERS", option, 2) != 1 || option[0] != L'1') return;
        Registry* registry = GetRegistry();
        if (!registry) return;
        auto candidate = t3render::buffers::BufferProxy::Create(ReadBufferSnapshot, nullptr);
        if (!candidate) return;
        std::lock_guard<std::mutex> lock(registry->mutex);
        if (!registry->buffers) registry->buffers = std::move(candidate);
    } catch (...) {
        Diagnostic("T3Render: buffer observation could not be enabled\n");
    }
}

// Declaration observation is independently opt-in. It samples application
// memory; neither the scanner nor the output reader dereferences game pointers.
bool ReadDeclarationSnapshot(void*, std::uintptr_t address, void* out, std::size_t length) {
    if (!address || !out || !length || length > std::numeric_limits<std::uintptr_t>::max() - address) return false;
    std::uintptr_t current = address;
    std::size_t remaining = length;
    while (remaining) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        switch (info.Protect & 0xff) {
        case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
        default: return false;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize > std::numeric_limits<std::uintptr_t>::max() - start) return false;
        const auto end = start + info.RegionSize;
        if (current < start || current >= end) return false;
        const std::size_t step = std::min<std::size_t>(remaining, end - current);
        if (!step) return false;
        current += step;
        remaining -= step;
    }
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, length, &copied) && copied == length;
}

void EnableDeclarationObserver() noexcept {
    try {
        wchar_t option[2]{};
        if (GetEnvironmentVariableW(L"T3RENDER_CAPTURE_DECLARATIONS", option, 2) != 1 || option[0] != L'1') return;
        Registry* registry = GetRegistry();
        if (!registry) return;
        wchar_t history[2]{};
        const bool history_requested = GetEnvironmentVariableW(L"T3RENDER_CAPTURE_DECLARATION_HISTORY", history, 2) == 1 &&
                                       history[0] == L'1';
        wchar_t programs[2]{};
        const bool programs_requested = GetEnvironmentVariableW(L"T3RENDER_CAPTURE_VERTEX_PROGRAMS", programs, 2) == 1 &&
                                        programs[0] == L'1';
        t3render::ir::ShaderRegistryLimits limits;
        if (history_requested) {
            limits.preserve_reset_history = true;
            // History mode's entire per-device payload budget (including live,
            // pinned and retired words) is 32 KiB; no parallel history cache.
            limits.words_per_device = 8192;
        }
        auto candidate = std::make_shared<t3render::ir::VertexDeclarationRegistry>(limits);
        std::lock_guard<std::mutex> lock(registry->mutex);
        if (!registry->declarations) {
            registry->declarations = std::move(candidate);
            registry->capture_vertex_programs = programs_requested;
        }
    } catch (...) {
        Diagnostic("T3Render: declaration observation could not be enabled\n");
    }
}

thread_local bool shader_getter_active = false;

struct ShaderDraw {
    bool active = false;
    t3render::ir::ShaderTicket ticket{};
    t3render::ShaderSnapshot snapshot{};
};

// CaptureDraw queries buffers while it holds CaptureState's mutex. Rewrapping
// an internal getter could release a wrapper there and reenter DeviceRelease.
// Return the native COM refs to these internal queries instead. If a final
// DeviceRelease still occurs during a query, defer only its capture notification
// until that mutex is no longer held. The saved pointer is an ID, not a live
// object; a final release concurrent with Draw remains invalid caller lifetime.
thread_local IDirect3DDevice8* capture_query_device = nullptr;
thread_local bool capture_query_active = false, capture_query_abandon = false, abandoning_capture = false;

struct DeferredDeclaration {
    std::shared_ptr<t3render::ir::VertexDeclarationRegistry> registry;
    std::uint64_t id = 0;
    bool close = false;
};
// There are at most eight live tokens in the single process registry. Poisoned
// tokens stay live until the outer capture scope drains this array.
thread_local std::array<DeferredDeclaration, 8> deferred_declarations;
thread_local std::size_t deferred_declaration_count = 0;

void InvalidateDeclaration(const std::shared_ptr<Binding>& binding, bool close = false) noexcept {
    if (!binding || !binding->declarations || !binding->declaration_device) return;
    auto& registry = binding->declarations;
    const auto id = binding->declaration_device;
    if (!capture_query_active) {
        if (close) registry->CloseDevice(id);
        else registry->Invalidate(id);
        return;
    }
    // Do not drain/free payloads while CaptureState's mutex may be held:
    // a custom allocator can reenter capture. Marking uses only fixed metadata.
    registry->MarkUnsafe(id);
    for (std::size_t i = 0; i < deferred_declaration_count; ++i) {
        auto& entry = deferred_declarations[i];
        if (entry.registry == registry && entry.id == id) { entry.close = entry.close || close; return; }
    }
    if (deferred_declaration_count < deferred_declarations.size())
        deferred_declarations[deferred_declaration_count++] = {registry, id, close};
}

void DrainDeferredDeclarations() noexcept {
    std::array<DeferredDeclaration, 8> pending;
    const std::size_t count = deferred_declaration_count;
    for (std::size_t i = 0; i < count; ++i) pending[i] = std::move(deferred_declarations[i]);
    deferred_declaration_count = 0; // allocator callbacks may reenter this scope
    for (std::size_t i = 0; i < count; ++i) {
        if (pending[i].close) pending[i].registry->CloseDevice(pending[i].id);
        else pending[i].registry->Invalidate(pending[i].id);
    }
}

void StopBufferObservationIfCaptureInactive(const std::shared_ptr<Binding>& binding) noexcept;

struct CaptureQueryScope {
    IDirect3DDevice8* const previous_device;
    const bool previous_active, previous_abandon;
    IDirect3DDevice8* const device;
    explicit CaptureQueryScope(IDirect3DDevice8* value) noexcept
        : previous_device(capture_query_device), previous_active(capture_query_active),
          previous_abandon(capture_query_abandon), device(value) {
        capture_query_device = value;
        capture_query_active = true;
        capture_query_abandon = false;
    }
    ~CaptureQueryScope() noexcept {
        const bool abandon = capture_query_abandon;
        capture_query_device = previous_device;
        capture_query_active = previous_active;
        capture_query_abandon = previous_abandon || (previous_active && abandon);
        if (!previous_active && !abandoning_capture) {
            if (abandon) {
                // Abandon before freeing payloads: allocator callbacks may
                // otherwise add apparently complete records to this trace.
                abandoning_capture = true;
                Observe([&] { CaptureQueryScope scope(device); t3render::CaptureAbandonDevice(device); });
                abandoning_capture = false;
            }
            DrainDeferredDeclarations();
            SweepRetiredBindings();
        }
    }
};

// Check only after a capture call has returned and released CaptureState's
// mutex. Existing wrappers must remain alive for device forwarding, but their
// shadow copies can be abandoned immediately after capture ends or fails.
void StopBufferObservationIfCaptureInactive(const std::shared_ptr<Binding>& binding) noexcept {
    if (!binding->buffers || !binding->buffer_observing.load(std::memory_order_acquire)) return;
    bool configured = true;
    Observe([&] { CaptureQueryScope scope(nullptr); configured = t3render::CaptureConfigured(); });
    if (!configured && binding->buffer_observing.exchange(false, std::memory_order_acq_rel))
        binding->buffers->StopObservation();
}

void NotifyDeferredRelease(IDirect3DDevice8* device, const std::shared_ptr<Binding>& binding) noexcept {
    Observe([&] { CaptureQueryScope scope(device); t3render::CaptureDeviceReleased(device); });
    StopBufferObservationIfCaptureInactive(binding);
}

void PrepareShaderDraw(const std::shared_ptr<Binding>& binding, IDirect3DDevice8* device,
                       DrawCall& call, ShaderDraw& state) noexcept {
    if (!binding->declarations) return;
    Observe([&] {
        bool wanted = false;
        { CaptureQueryScope scope(device); wanted = t3render::CaptureWantsDraw(device); }
        if (!wanted || Find(device, true) != binding) return;
        state.active = true;
        state.ticket = binding->declarations->BeginDraw(binding->declaration_device);
        if (!shader_getter_active) {
            struct GetterGuard {
                GetterGuard() { shader_getter_active = true; }
                ~GetterGuard() { shader_getter_active = false; }
            } getter_guard;
            state.snapshot.getter_result = reinterpret_cast<GetVertexShader>(binding->original[Layout().device_get_vertex_shader])(
                device, &state.snapshot.value);
        }
        if (SUCCEEDED(state.snapshot.getter_result) && state.ticket)
            state.snapshot.declaration = binding->declarations->Observe(state.ticket, state.snapshot.value);
        call.shader_snapshot = &state.snapshot;
    });
}

void FinishShaderDraw(const std::shared_ptr<Binding>& binding, IDirect3DDevice8* device,
                      const ShaderDraw& state, std::uint64_t token, HRESULT hr, int buffer_stable) noexcept {
    if (state.active) {
        const bool ticket_stable = binding->declarations->End(state.ticket);
        Observe([&] { CaptureQueryScope scope(device); t3render::CaptureShaderDrawResult(device, token, hr, buffer_stable,
                                                       SUCCEEDED(hr) && SUCCEEDED(state.snapshot.getter_result) && ticket_stable); });
    } else if (buffer_stable >= 0) {
        Observe([&] { CaptureQueryScope scope(device); t3render::CaptureBufferedDrawResult(device, token, hr, buffer_stable != 0); });
    } else {
        Observe([&] { CaptureQueryScope scope(device); t3render::CaptureDrawResult(device, token, hr); });
    }
}

HRESULT STDMETHODCALLTYPE FactoryQuery(IDirect3D8* object, REFIID iid, void** result) {
    ErrorState error;
    auto binding = Find(object, false);
    if (!binding) return E_FAIL;
    error.Restore();
    HRESULT hr = reinterpret_cast<QueryFactory>(binding->original[Layout().factory_query])(object, iid, result);
    error.Capture();
    if (SUCCEEDED(hr) && result && *result && IsEqualIID(iid, IID_IDirect3D8) &&
        !capture_query_active && !Bind(*result, false)) {
        Diagnostic("T3Render: could not instrument a Direct3D8 interface alias\n");
    }
    return hr;
}

ULONG STDMETHODCALLTYPE FactoryRelease(IDirect3D8* object) {
    ErrorState error;
    auto binding = Find(object, false);
    if (!binding) return 0;
    error.Restore();
    ULONG refs = reinterpret_cast<ReleaseFactory>(binding->original[Layout().factory_release])(object);
    error.Capture();
    if (refs == 0) {
        binding->retired.store(true, std::memory_order_release);
        if (!capture_query_active) Forget(object, false, binding);
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE FactoryCreateDevice(IDirect3D8* object, UINT adapter, D3DDEVTYPE type, HWND focus,
                                               DWORD behavior, D3DPRESENT_PARAMETERS* params,
                                               IDirect3DDevice8** device) {
    ErrorState error;
    auto binding = Find(object, false);
    if (!binding) return D3DERR_INVALIDCALL;
    error.Restore();
    HRESULT hr = reinterpret_cast<CreateDevice>(binding->original[Layout().factory_create_device])(
        object, adapter, type, focus, behavior, params, device);
    error.Capture();
    if (SUCCEEDED(hr) && device && *device) {
        if (capture_query_active) { capture_query_abandon = true; }
        else if (Bind(*device, true)) {
            Observe([&] { CaptureQueryScope scope(*device); t3render::CaptureDevice(*device, params); });
        } else {
            Diagnostic("T3Render: could not instrument Direct3DDevice8; rendering continues without capture\n");
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceQuery(IDirect3DDevice8* device, REFIID iid, void** result) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return E_FAIL;
    error.Restore();
    HRESULT hr = reinterpret_cast<QueryDevice>(binding->original[Layout().device_query])(device, iid, result);
    error.Capture();
    const bool escaped = SUCCEEDED(hr) && result && *result &&
        (*result != device || (!IsEqualIID(iid, IID_IDirect3DDevice8) && !IsEqualIID(iid, IID_IUnknown)));
    if (escaped) {
        if (binding->buffers) {
            binding->buffers->InvalidateDevice(device);
            if (*result != device && IsEqualIID(iid, IID_IDirect3DDevice8))
                binding->buffers->InvalidateDevice(static_cast<IDirect3DDevice8*>(*result));
        }
        // Final Release may occur through the escaped alias and never reach
        // this binding. Retire its poisoned, non-reusable token.
        InvalidateDeclaration(binding, true);
    }
    if (SUCCEEDED(hr) && result && *result && IsEqualIID(iid, IID_IDirect3DDevice8)) {
        // Binding allocates and can invoke an allocator while the capture
        // writer is locked; leave such aliases unhooked and evidence unsafe.
        if (!capture_query_active && !Bind(*result, true, !escaped))
            Diagnostic("T3Render: could not instrument a Direct3DDevice8 interface alias\n");
        if (escaped) {
            auto alias = Find(*result, true);
            InvalidateDeclaration(alias, true);
        }
    }
    return hr;
}

ULONG STDMETHODCALLTYPE DeviceRelease(IDirect3DDevice8* device) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return 0;
    error.Restore();
    const ULONG refs = reinterpret_cast<ReleaseDevice>(binding->original[Layout().device_release])(device);
    error.Capture();
    if (refs == 0) {
        binding->retired.store(true, std::memory_order_release);
        InvalidateDeclaration(binding, true);
        if (capture_query_active) {
            if (binding->buffers) binding->deferred_buffer_invalidation.store(true, std::memory_order_release);
            binding->deferred_capture_release.store(true, std::memory_order_release);
        } else {
            if (binding->buffers) binding->buffers->InvalidateDevice(device);
            NotifyDeferredRelease(device, binding);
            Forget(device, true, binding);
        }
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE DeviceReset(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* params) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding); capture_query_abandon = true;
        error.Restore();
        const HRESULT hr = reinterpret_cast<ResetDevice>(binding->original[Layout().device_reset])(device, params);
        error.Capture(); return hr;
    }
    if (binding->buffers) binding->buffers->InvalidateDevice(device);
    const auto ticket = binding->declarations ? binding->declarations->BeginReset(binding->declaration_device)
                                               : t3render::ir::ShaderTicket{};
    error.Restore();
    const HRESULT hr = reinterpret_cast<ResetDevice>(binding->original[Layout().device_reset])(device, params);
    error.Capture();
    if (binding->declarations) (void)binding->declarations->EndReset(ticket, static_cast<std::uint32_t>(hr));
    Observe([&] { CaptureQueryScope scope(device); t3render::CaptureReset(device, params, hr); });
    return hr;
}

HRESULT STDMETHODCALLTYPE DevicePresent(IDirect3DDevice8* device, const RECT* source, const RECT* dest,
                                        HWND window, const RGNDATA* dirty) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding); capture_query_abandon = true;
        error.Restore();
        const HRESULT hr = reinterpret_cast<PresentDevice>(binding->original[Layout().device_present])(
            device, source, dest, window, dirty);
        error.Capture(); return hr;
    }
    error.Restore();
    HRESULT hr = reinterpret_cast<PresentDevice>(binding->original[Layout().device_present])(
        device, source, dest, window, dirty);
    error.Capture();
    Observe([&] { CaptureQueryScope scope(device); t3render::CapturePresent(device, hr); });
    StopBufferObservationIfCaptureInactive(binding);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceCreateVertexBuffer(IDirect3DDevice8* device, UINT size, DWORD usage, DWORD fvf,
                                                     D3DPOOL pool, IDirect3DVertexBuffer8** output) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    error.Restore();
    const HRESULT hr = reinterpret_cast<CreateVertexBuffer>(binding->original[Layout().device_create_vertex_buffer])(
        device, size, usage, fvf, pool, output);
    error.Capture();
    if (binding->buffers && SUCCEEDED(hr) && output && *output) {
        auto* wrapped = binding->buffers->AdoptVertex(device, *output, size, true);
        if (wrapped) *output = wrapped;
        else binding->buffers->InvalidateDevice(device);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceCreateIndexBuffer(IDirect3DDevice8* device, UINT size, DWORD usage, D3DFORMAT format,
                                                    D3DPOOL pool, IDirect3DIndexBuffer8** output) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    error.Restore();
    const HRESULT hr = reinterpret_cast<CreateIndexBuffer>(binding->original[Layout().device_create_index_buffer])(
        device, size, usage, format, pool, output);
    error.Capture();
    if (binding->buffers && SUCCEEDED(hr) && output && *output) {
        auto* wrapped = binding->buffers->AdoptIndex(device, *output, size, true);
        if (wrapped) *output = wrapped;
        else binding->buffers->InvalidateDevice(device);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceSetStreamSource(IDirect3DDevice8* device, UINT stream,
                                                 IDirect3DVertexBuffer8* buffer, UINT stride) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding || !binding->buffers) return D3DERR_INVALIDCALL;
    auto pin = binding->buffers->UnwrapVertex(device, buffer);
    const std::uint64_t mutation = binding->buffers->BeginBindingMutation(device);
    error.Restore();
    const HRESULT hr = reinterpret_cast<SetStreamSource>(binding->original[Layout().device_set_stream])(
        device, stream, pin.native(), stride);
    error.Capture();
    if (mutation) binding->buffers->EndBindingMutation(device, mutation);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceGetStreamSource(IDirect3DDevice8* device, UINT stream,
                                                 IDirect3DVertexBuffer8** output, UINT* stride) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding || !binding->buffers) return D3DERR_INVALIDCALL;
    error.Restore();
    const HRESULT hr = reinterpret_cast<GetStreamSource>(binding->original[Layout().device_get_stream])(
        device, stream, output, stride);
    error.Capture();
    if (capture_query_device != device && SUCCEEDED(hr) && output && *output) {
        auto* wrapped = binding->buffers->AdoptVertex(device, *output, 0, false);
        if (wrapped) *output = wrapped;
        else binding->buffers->InvalidateDevice(device);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceSetIndices(IDirect3DDevice8* device, IDirect3DIndexBuffer8* buffer, UINT base) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding || !binding->buffers) return D3DERR_INVALIDCALL;
    auto pin = binding->buffers->UnwrapIndex(device, buffer);
    const std::uint64_t mutation = binding->buffers->BeginBindingMutation(device);
    error.Restore();
    const HRESULT hr = reinterpret_cast<SetIndices>(binding->original[Layout().device_set_indices])(
        device, pin.native(), base);
    error.Capture();
    if (mutation) binding->buffers->EndBindingMutation(device, mutation);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceGetIndices(IDirect3DDevice8* device, IDirect3DIndexBuffer8** output, UINT* base) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding || !binding->buffers) return D3DERR_INVALIDCALL;
    error.Restore();
    const HRESULT hr = reinterpret_cast<GetIndices>(binding->original[Layout().device_get_indices])(
        device, output, base);
    error.Capture();
    if (capture_query_device != device && SUCCEEDED(hr) && output && *output) {
        auto* wrapped = binding->buffers->AdoptIndex(device, *output, 0, false);
        if (wrapped) *output = wrapped;
        else binding->buffers->InvalidateDevice(device);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceProcessVertices(IDirect3DDevice8* device, UINT source, UINT destination, UINT count,
                                                 IDirect3DVertexBuffer8* target, DWORD flags) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding || !binding->buffers) return D3DERR_INVALIDCALL;
    auto pin = binding->buffers->UnwrapVertex(device, target);
    const std::uint64_t mutation = binding->buffers->BeginBindingMutation(device);
    binding->buffers->InvalidateVertex(device, target);
    error.Restore();
    const HRESULT hr = reinterpret_cast<ProcessVertices>(binding->original[Layout().device_process_vertices])(
        device, source, destination, count, pin.native(), flags);
    error.Capture();
    if (mutation) binding->buffers->EndBindingMutation(device, mutation);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceCreateVertexShader(IDirect3DDevice8* device, const DWORD* declaration,
                                                     const DWORD* function, DWORD* handle, DWORD usage) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding || !binding->declarations) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding);
        error.Restore();
        const HRESULT hr = reinterpret_cast<CreateVertexShader>(binding->original[Layout().device_create_vertex_shader])(
            device, declaration, function, handle, usage);
        error.Capture();
        return hr;
    }
    auto ticket = binding->declarations->BeginCreate(binding->declaration_device);
    auto snapshot = ticket ? t3render::ir::ScanDeclaration(reinterpret_cast<std::uintptr_t>(declaration),
                                                           ReadDeclarationSnapshot, nullptr)
                           : t3render::ir::DeclarationSnapshot{};
    auto program = ticket && binding->capture_vertex_programs
        ? t3render::ir::ScanVertexShader11(reinterpret_cast<std::uintptr_t>(function), ReadDeclarationSnapshot, nullptr, 1024)
        : t3render::ir::ShaderProgramSnapshot{};
    error.Restore();
    const HRESULT hr = reinterpret_cast<CreateVertexShader>(binding->original[Layout().device_create_vertex_shader])(
        device, declaration, function, handle, usage);
    error.Capture();
    std::uint32_t value = 0;
    const bool known = SUCCEEDED(hr) && ReadDeclarationSnapshot(nullptr, reinterpret_cast<std::uintptr_t>(handle),
                                                               &value, sizeof(value));
    (void)binding->declarations->EndCreate(ticket, SUCCEEDED(hr), known, value, std::move(snapshot), std::move(program));
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceSetVertexShader(IDirect3DDevice8* device, DWORD value) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding || !binding->declarations) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding);
        error.Restore();
        const HRESULT hr = reinterpret_cast<SetVertexShader>(binding->original[Layout().device_set_vertex_shader])(device, value);
        error.Capture();
        return hr;
    }
    const auto ticket = binding->declarations->BeginSet(binding->declaration_device);
    error.Restore();
    const HRESULT hr = reinterpret_cast<SetVertexShader>(binding->original[Layout().device_set_vertex_shader])(device, value);
    error.Capture();
    (void)binding->declarations->EndSet(ticket, static_cast<std::uint32_t>(hr), value);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDeleteVertexShader(IDirect3DDevice8* device, DWORD value) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding || !binding->declarations) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding);
        error.Restore();
        const HRESULT hr = reinterpret_cast<DeleteVertexShader>(binding->original[Layout().device_delete_vertex_shader])(device, value);
        error.Capture();
        return hr;
    }
    const auto ticket = binding->declarations->BeginDelete(binding->declaration_device, value);
    error.Restore();
    const HRESULT hr = reinterpret_cast<DeleteVertexShader>(binding->original[Layout().device_delete_vertex_shader])(device, value);
    error.Capture();
    (void)binding->declarations->EndDelete(ticket, SUCCEEDED(hr));
    return hr;
}

// State blocks can retain or restore native bindings without passing through
// SetStreamSource/SetIndices. The production proxy invalidates evidence before
// the call even if the runtime later rejects it. Only a separately built test
// DLL explores a narrower binding-mutation window; that DLL is not safe for TDS.
template <typename Method, typename... Args>
HRESULT ForwardBufferInvalidating(IDirect3DDevice8* device, std::size_t slot, Args... args) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    InvalidateDeclaration(binding);
#ifdef T3RENDER_STATEBLOCK_TEST_FIXTURE
    const std::uint64_t mutation = binding->buffers ? binding->buffers->BeginBindingMutation(device) : 0;
#else
    if (binding->buffers) binding->buffers->InvalidateDevice(device);
#endif
    error.Restore();
    const HRESULT hr = reinterpret_cast<Method>(binding->original[slot])(device, args...);
    error.Capture();
#ifdef T3RENDER_STATEBLOCK_TEST_FIXTURE
    if (mutation) binding->buffers->EndBindingMutation(device, mutation);
#endif
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceBeginStateBlock(IDirect3DDevice8* device) {
    return ForwardBufferInvalidating<BeginStateBlock>(device, Layout().device_begin_state_block);
}
HRESULT STDMETHODCALLTYPE DeviceEndStateBlock(IDirect3DDevice8* device, DWORD* token) {
    return ForwardBufferInvalidating<EndStateBlock>(device, Layout().device_end_state_block, token);
}
HRESULT STDMETHODCALLTYPE DeviceApplyStateBlock(IDirect3DDevice8* device, DWORD token) {
    return ForwardBufferInvalidating<ApplyStateBlock>(device, Layout().device_apply_state_block, token);
}
HRESULT STDMETHODCALLTYPE DeviceCaptureStateBlock(IDirect3DDevice8* device, DWORD token) {
    return ForwardBufferInvalidating<CaptureStateBlock>(device, Layout().device_capture_state_block, token);
}
HRESULT STDMETHODCALLTYPE DeviceDeleteStateBlock(IDirect3DDevice8* device, DWORD token) {
    return ForwardBufferInvalidating<DeleteStateBlock>(device, Layout().device_delete_state_block, token);
}
HRESULT STDMETHODCALLTYPE DeviceCreateStateBlock(IDirect3DDevice8* device, D3DSTATEBLOCKTYPE type, DWORD* token) {
    return ForwardBufferInvalidating<CreateStateBlock>(device, Layout().device_create_state_block, type, token);
}

HRESULT STDMETHODCALLTYPE DeviceClear(IDirect3DDevice8* device, DWORD count, const D3DRECT* rectangles,
                                      DWORD flags, D3DCOLOR color, float depth, DWORD stencil) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding); capture_query_abandon = true;
        error.Restore();
        const HRESULT hr = reinterpret_cast<ClearDevice>(binding->original[Layout().device_clear])(
            device, count, rectangles, flags, color, depth, stencil);
        error.Capture(); return hr;
    }
    std::uint64_t token = 0;
    Observe([&] { CaptureQueryScope scope(device); token = t3render::CaptureClear(device, count, rectangles, flags, color, depth, stencil); });
    if (Find(device, true) != binding) return D3DERR_INVALIDCALL;
    error.Restore();
    HRESULT hr = reinterpret_cast<ClearDevice>(binding->original[Layout().device_clear])(
        device, count, rectangles, flags, color, depth, stencil);
    error.Capture();
    Observe([&] { CaptureQueryScope scope(device); t3render::CaptureClearResult(device, token, hr); });
    StopBufferObservationIfCaptureInactive(binding);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDraw(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive, UINT start, UINT count) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding); capture_query_abandon = true;
        error.Restore();
        const HRESULT hr = reinterpret_cast<DrawPrimitive>(binding->original[Layout().device_draw])(device, primitive, start, count);
        error.Capture(); return hr;
    }
    DrawCall call{};
    call.kind = DrawKind::Primitive;
    call.primitive_type = primitive;
    call.start_vertex = start;
    call.primitive_count = count;
    const bool observing = binding->buffers && binding->buffer_observing.load(std::memory_order_acquire);
    const std::uint64_t buffer_token = observing ? binding->buffers->BeginDraw(device) : 0;
    t3render::BufferObserver observer{binding->buffers.get(), buffer_token != 0, ReadObservedVertex, ReadObservedIndex};
    if (observing) call.buffer_observer = &observer;
    ShaderDraw shader;
    PrepareShaderDraw(binding, device, call, shader);
    // A reentrant getter can release the last native reference. In that invalid
    // caller-lifetime case, never query or submit through a destroyed object.
    if (Find(device, true) != binding) {
        if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
        if (shader.active && binding->declarations) (void)binding->declarations->End(shader.ticket);
        return D3DERR_INVALIDCALL;
    }
    std::uint64_t token = 0;
    Observe([&] { CaptureQueryScope scope(device); token = t3render::CaptureDraw(device, call); });
    if (Find(device, true) != binding) {
        if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
        if (shader.active && binding->declarations) (void)binding->declarations->End(shader.ticket);
        return D3DERR_INVALIDCALL;
    }
    error.Restore();
    HRESULT hr = reinterpret_cast<DrawPrimitive>(binding->original[Layout().device_draw])(device, primitive, start, count);
    error.Capture();
    const int buffer_stable = observing ? static_cast<int>(buffer_token && binding->buffers->EndDraw(device, buffer_token)) : -1;
    FinishShaderDraw(binding, device, shader, token, hr, buffer_stable);
    StopBufferObservationIfCaptureInactive(binding);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDrawIndexed(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive, UINT min_index,
                                             UINT vertices, UINT start_index, UINT count) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding); capture_query_abandon = true;
        error.Restore();
        const HRESULT hr = reinterpret_cast<DrawIndexed>(binding->original[Layout().device_draw_indexed])(
            device, primitive, min_index, vertices, start_index, count);
        error.Capture(); return hr;
    }
    DrawCall call{};
    call.kind = DrawKind::IndexedPrimitive;
    call.primitive_type = primitive;
    call.min_vertex_index = min_index;
    call.num_vertices = vertices;
    call.start_index = start_index;
    call.primitive_count = count;
    const bool observing = binding->buffers && binding->buffer_observing.load(std::memory_order_acquire);
    const std::uint64_t buffer_token = observing ? binding->buffers->BeginDraw(device) : 0;
    t3render::BufferObserver observer{binding->buffers.get(), buffer_token != 0, ReadObservedVertex, ReadObservedIndex};
    if (observing) call.buffer_observer = &observer;
    ShaderDraw shader;
    PrepareShaderDraw(binding, device, call, shader);
    // A reentrant getter can release the last native reference. In that invalid
    // caller-lifetime case, never query or submit through a destroyed object.
    if (Find(device, true) != binding) {
        if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
        if (shader.active && binding->declarations) (void)binding->declarations->End(shader.ticket);
        return D3DERR_INVALIDCALL;
    }
    std::uint64_t token = 0;
    Observe([&] { CaptureQueryScope scope(device); token = t3render::CaptureDraw(device, call); });
    if (Find(device, true) != binding) {
        if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
        if (shader.active && binding->declarations) (void)binding->declarations->End(shader.ticket);
        return D3DERR_INVALIDCALL;
    }
    error.Restore();
    HRESULT hr = reinterpret_cast<DrawIndexed>(binding->original[Layout().device_draw_indexed])(
        device, primitive, min_index, vertices, start_index, count);
    error.Capture();
    const int buffer_stable = observing ? static_cast<int>(buffer_token && binding->buffers->EndDraw(device, buffer_token)) : -1;
    FinishShaderDraw(binding, device, shader, token, hr, buffer_stable);
    StopBufferObservationIfCaptureInactive(binding);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDrawUp(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive, UINT count,
                                        const void* vertices, UINT stride) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding); capture_query_abandon = true;
        error.Restore();
        const HRESULT hr = reinterpret_cast<DrawPrimitiveUp>(binding->original[Layout().device_draw_up])(
            device, primitive, count, vertices, stride);
        error.Capture(); return hr;
    }
    DrawCall call{};
    call.kind = DrawKind::PrimitiveUP;
    call.primitive_type = primitive;
    call.primitive_count = count;
    call.vertex_stride = stride;
    call.vertex_data = vertices;
    const std::uint64_t buffer_token = binding->buffers && binding->buffer_observing.load(std::memory_order_acquire)
        ? binding->buffers->BeginDraw(device) : 0;
    ShaderDraw shader;
    PrepareShaderDraw(binding, device, call, shader);
    // A reentrant getter can release the last native reference. In that invalid
    // caller-lifetime case, never query or submit through a destroyed object.
    if (Find(device, true) != binding) {
        if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
        if (shader.active && binding->declarations) (void)binding->declarations->End(shader.ticket);
        return D3DERR_INVALIDCALL;
    }
    std::uint64_t token = 0;
    Observe([&] { CaptureQueryScope scope(device); token = t3render::CaptureDraw(device, call); });
    if (Find(device, true) != binding) {
        if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
        if (shader.active && binding->declarations) (void)binding->declarations->End(shader.ticket);
        return D3DERR_INVALIDCALL;
    }
    error.Restore();
    HRESULT hr = reinterpret_cast<DrawPrimitiveUp>(binding->original[Layout().device_draw_up])(
        device, primitive, count, vertices, stride);
    error.Capture();
    if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
    FinishShaderDraw(binding, device, shader, token, hr, -1);
    StopBufferObservationIfCaptureInactive(binding);
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDrawIndexedUp(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive, UINT min_index,
                                               UINT vertices, UINT count, const void* indices, D3DFORMAT format,
                                               const void* vertex_data, UINT stride) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    if (capture_query_active) {
        InvalidateDeclaration(binding); capture_query_abandon = true;
        error.Restore();
        const HRESULT hr = reinterpret_cast<DrawIndexedUp>(binding->original[Layout().device_draw_indexed_up])(
            device, primitive, min_index, vertices, count, indices, format, vertex_data, stride);
        error.Capture(); return hr;
    }
    DrawCall call{};
    call.kind = DrawKind::IndexedPrimitiveUP;
    call.primitive_type = primitive;
    call.min_vertex_index = min_index;
    call.num_vertices = vertices;
    call.primitive_count = count;
    call.index_format = format;
    call.vertex_stride = stride;
    call.vertex_data = vertex_data;
    call.index_data = indices;
    const std::uint64_t buffer_token = binding->buffers && binding->buffer_observing.load(std::memory_order_acquire)
        ? binding->buffers->BeginDraw(device) : 0;
    ShaderDraw shader;
    PrepareShaderDraw(binding, device, call, shader);
    // A reentrant getter can release the last native reference. In that invalid
    // caller-lifetime case, never query or submit through a destroyed object.
    if (Find(device, true) != binding) {
        if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
        if (shader.active && binding->declarations) (void)binding->declarations->End(shader.ticket);
        return D3DERR_INVALIDCALL;
    }
    std::uint64_t token = 0;
    Observe([&] { CaptureQueryScope scope(device); token = t3render::CaptureDraw(device, call); });
    if (Find(device, true) != binding) {
        if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
        if (shader.active && binding->declarations) (void)binding->declarations->End(shader.ticket);
        return D3DERR_INVALIDCALL;
    }
    error.Restore();
    HRESULT hr = reinterpret_cast<DrawIndexedUp>(binding->original[Layout().device_draw_indexed_up])(
        device, primitive, min_index, vertices, count, indices, format, vertex_data, stride);
    error.Capture();
    if (binding->buffers && buffer_token) (void)binding->buffers->EndDraw(device, buffer_token);
    FinishShaderDraw(binding, device, shader, token, hr, -1);
    StopBufferObservationIfCaptureInactive(binding);
    return hr;
}

using Direct3DCreate8Fn = decltype(&Direct3DCreate8);
static_assert(std::is_same_v<Direct3DCreate8Fn, decltype(&Proxy_Direct3DCreate8)>,
              "Proxy export must have the SDK's Direct3DCreate8 calling convention and signature");

Direct3DCreate8Fn RealCreate8() noexcept {
    static Direct3DCreate8Fn function = []() noexcept -> Direct3DCreate8Fn {
        try {
            wchar_t directory[MAX_PATH + 1]{};
            UINT length = GetSystemDirectoryW(directory, MAX_PATH);
            if (!length || length >= MAX_PATH) return nullptr;
            std::wstring path(directory, length);
            path += L"\\d3d8.dll";
            HMODULE module = LoadLibraryW(path.c_str());
            if (!module) return nullptr;
            HMODULE self = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(&Proxy_Direct3DCreate8), &self) || module == self) {
                FreeLibrary(module);
                return nullptr;
            }
            FARPROC address = GetProcAddress(module, "Direct3DCreate8");
            Direct3DCreate8Fn create = nullptr;
            static_assert(sizeof(create) == sizeof(address), "Unexpected Windows function pointer size");
            std::memcpy(&create, &address, sizeof(create));
            return create;
        } catch (...) {
            return nullptr;
        }
    }();
    return function;
}

}  // namespace

#ifdef T3RENDER_PROXY_TEST
// Host tests inspect the manager without adding an export to the production
// DLL. `native` is the runtime resource, not a wrapper pointer.
t3render::ir::RegistryResult T3ProxyTestReadVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                                   std::size_t offset, void* output, std::size_t length) {
    auto binding = Find(owner, true);
    return binding && binding->buffers ? binding->buffers->ReadVertex(owner, native, offset, output, length)
                                       : t3render::ir::RegistryResult::UnknownResource;
}
t3render::ir::RegistryResult T3ProxyTestReadIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                                  std::size_t offset, void* output, std::size_t length) {
    auto binding = Find(owner, true);
    return binding && binding->buffers ? binding->buffers->ReadIndex(owner, native, offset, output, length)
                                       : t3render::ir::RegistryResult::UnknownResource;
}
#endif

extern "C" IDirect3D8* WINAPI Proxy_Direct3DCreate8(UINT sdk_version) {
    ErrorState error;
    Direct3DCreate8Fn real = RealCreate8();
    if (!real) {
        Diagnostic("T3Render: system d3d8.dll or Direct3DCreate8 unavailable\n");
        return nullptr;
    }
    // Reject an indirect proxy cycle as well as the direct self-load above.
    static thread_local bool creating = false;
    if (creating) {
        Diagnostic("T3Render: recursive Direct3DCreate8 chain rejected\n");
        return nullptr;
    }
    struct Guard {
        bool& active;
        ~Guard() { active = false; }
    } guard{creating};
    creating = true;
    error.Restore();
    IDirect3D8* factory = real(sdk_version);
    error.Capture();
    bool configured = false;
    Observe([&] { CaptureQueryScope scope(nullptr); configured = t3render::CaptureConfigured(); });
    if (configured) Observe([&] { EnableBufferObserver(); EnableDeclarationObserver(); });
    // Without a valid opt-in, there is no change to the returned object.
    if (factory && configured && !Bind(factory, false)) {
        Diagnostic("T3Render: could not instrument IDirect3D8; rendering continues without capture\n");
    }
    return factory;
}
