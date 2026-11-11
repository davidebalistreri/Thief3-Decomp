// Opt-in, observational D3D8 proxy. Objects and unobserved methods remain the
// runtime's own; per-object vtables intercept only factory/device lifecycle and
// draw boundaries. See README for limitations and deployment precautions.
#include "capture.hpp"
#include "vtable_layout.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

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
using DrawPrimitive = Function<decltype(&IDirect3DDevice8::DrawPrimitive)>;
using DrawIndexed = Function<decltype(&IDirect3DDevice8::DrawIndexedPrimitive)>;
using DrawPrimitiveUp = Function<decltype(&IDirect3DDevice8::DrawPrimitiveUP)>;
using DrawIndexedUp = Function<decltype(&IDirect3DDevice8::DrawIndexedPrimitiveUP)>;

HRESULT STDMETHODCALLTYPE FactoryQuery(IDirect3D8*, REFIID, void**);
ULONG STDMETHODCALLTYPE FactoryRelease(IDirect3D8*);
HRESULT STDMETHODCALLTYPE FactoryCreateDevice(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD,
                                               D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
HRESULT STDMETHODCALLTYPE DeviceQuery(IDirect3DDevice8*, REFIID, void**);
ULONG STDMETHODCALLTYPE DeviceRelease(IDirect3DDevice8*);
HRESULT STDMETHODCALLTYPE DeviceReset(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
HRESULT STDMETHODCALLTYPE DevicePresent(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
HRESULT STDMETHODCALLTYPE DeviceDraw(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
HRESULT STDMETHODCALLTYPE DeviceDrawIndexed(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
HRESULT STDMETHODCALLTYPE DeviceDrawUp(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
HRESULT STDMETHODCALLTYPE DeviceDrawIndexedUp(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                               const void*, D3DFORMAT, const void*, UINT);

static_assert(std::is_same_v<QueryFactory, decltype(&FactoryQuery)>);
static_assert(std::is_same_v<ReleaseFactory, decltype(&FactoryRelease)>);
static_assert(std::is_same_v<CreateDevice, decltype(&FactoryCreateDevice)>);
static_assert(std::is_same_v<QueryDevice, decltype(&DeviceQuery)>);
static_assert(std::is_same_v<ReleaseDevice, decltype(&DeviceRelease)>);
static_assert(std::is_same_v<ResetDevice, decltype(&DeviceReset)>);
static_assert(std::is_same_v<PresentDevice, decltype(&DevicePresent)>);
static_assert(std::is_same_v<DrawPrimitive, decltype(&DeviceDraw)>);
static_assert(std::is_same_v<DrawIndexed, decltype(&DeviceDrawIndexed)>);
static_assert(std::is_same_v<DrawPrimitiveUp, decltype(&DeviceDrawUp)>);
static_assert(std::is_same_v<DrawIndexedUp, decltype(&DeviceDrawIndexedUp)>);

const T3VtableLayout& Layout() {
    static const T3VtableLayout layout = T3GetVtableLayout();
    return layout;
}

struct Binding {
    std::vector<void*> original;
    std::vector<void*> instrumented;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<void*, std::shared_ptr<Binding>> factories;
    std::unordered_map<void*, std::shared_ptr<Binding>> devices;
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
    return it == objects.end() ? std::shared_ptr<Binding>{} : it->second;
}

void Forget(void* object, bool device, const std::shared_ptr<Binding>& binding) noexcept {
    Registry* registry = GetRegistry();
    if (!registry) return;
    std::lock_guard<std::mutex> lock(registry->mutex);
    auto& objects = device ? registry->devices : registry->factories;
    const auto it = objects.find(object);
    // Avoid removing a new object that reused this address concurrently.
    if (it != objects.end() && it->second == binding) objects.erase(it);
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
bool Bind(void* object, bool device) noexcept {
    try {
        Registry* registry = GetRegistry();
        if (!registry || !WritableObject(object)) return false;
        const T3VtableLayout& l = Layout();
        const size_t count = device ? l.device_size : l.factory_size;
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
        auto& v = binding->instrumented;
        if (device) {
            v[l.device_query] = reinterpret_cast<void*>(&DeviceQuery);
            v[l.device_release] = reinterpret_cast<void*>(&DeviceRelease);
            v[l.device_reset] = reinterpret_cast<void*>(&DeviceReset);
            v[l.device_present] = reinterpret_cast<void*>(&DevicePresent);
            v[l.device_draw] = reinterpret_cast<void*>(&DeviceDraw);
            v[l.device_draw_indexed] = reinterpret_cast<void*>(&DeviceDrawIndexed);
            v[l.device_draw_up] = reinterpret_cast<void*>(&DeviceDrawUp);
            v[l.device_draw_indexed_up] = reinterpret_cast<void*>(&DeviceDrawIndexedUp);
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

HRESULT STDMETHODCALLTYPE FactoryQuery(IDirect3D8* object, REFIID iid, void** result) {
    ErrorState error;
    auto binding = Find(object, false);
    if (!binding) return E_FAIL;
    error.Restore();
    HRESULT hr = reinterpret_cast<QueryFactory>(binding->original[Layout().factory_query])(object, iid, result);
    error.Capture();
    if (SUCCEEDED(hr) && result && *result && IsEqualIID(iid, IID_IDirect3D8) && !Bind(*result, false)) {
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
    if (refs == 0) Forget(object, false, binding);
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
        if (Bind(*device, true)) {
            Observe([&] { t3render::CaptureDevice(*device, params); });
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
    if (SUCCEEDED(hr) && result && *result && IsEqualIID(iid, IID_IDirect3DDevice8) && !Bind(*result, true)) {
        Diagnostic("T3Render: could not instrument a Direct3DDevice8 interface alias\n");
    }
    return hr;
}

ULONG STDMETHODCALLTYPE DeviceRelease(IDirect3DDevice8* device) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return 0;
    error.Restore();
    ULONG refs = reinterpret_cast<ReleaseDevice>(binding->original[Layout().device_release])(device);
    error.Capture();
    if (refs == 0) {
        // The object is already destroyed; capture may compare the address but
        // must never dereference it.
        Observe([&] { t3render::CaptureDeviceReleased(device); });
        Forget(device, true, binding);
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE DeviceReset(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS* params) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    error.Restore();
    HRESULT hr = reinterpret_cast<ResetDevice>(binding->original[Layout().device_reset])(device, params);
    error.Capture();
    Observe([&] { t3render::CaptureReset(device, params, hr); });
    return hr;
}

HRESULT STDMETHODCALLTYPE DevicePresent(IDirect3DDevice8* device, const RECT* source, const RECT* dest,
                                        HWND window, const RGNDATA* dirty) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    error.Restore();
    HRESULT hr = reinterpret_cast<PresentDevice>(binding->original[Layout().device_present])(
        device, source, dest, window, dirty);
    error.Capture();
    Observe([&] { t3render::CapturePresent(device, hr); });
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDraw(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive, UINT start, UINT count) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    DrawCall call{};
    call.kind = DrawKind::Primitive;
    call.primitive_type = primitive;
    call.start_vertex = start;
    call.primitive_count = count;
    Observe([&] { t3render::CaptureDraw(device, call); });
    error.Restore();
    HRESULT hr = reinterpret_cast<DrawPrimitive>(binding->original[Layout().device_draw])(device, primitive, start, count);
    error.Capture();
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDrawIndexed(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive, UINT min_index,
                                             UINT vertices, UINT start_index, UINT count) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    DrawCall call{};
    call.kind = DrawKind::IndexedPrimitive;
    call.primitive_type = primitive;
    call.min_vertex_index = min_index;
    call.num_vertices = vertices;
    call.start_index = start_index;
    call.primitive_count = count;
    Observe([&] { t3render::CaptureDraw(device, call); });
    error.Restore();
    HRESULT hr = reinterpret_cast<DrawIndexed>(binding->original[Layout().device_draw_indexed])(
        device, primitive, min_index, vertices, start_index, count);
    error.Capture();
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDrawUp(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive, UINT count,
                                        const void* vertices, UINT stride) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
    DrawCall call{};
    call.kind = DrawKind::PrimitiveUP;
    call.primitive_type = primitive;
    call.primitive_count = count;
    call.vertex_stride = stride;
    call.vertex_data = vertices;
    Observe([&] { t3render::CaptureDraw(device, call); });
    error.Restore();
    HRESULT hr = reinterpret_cast<DrawPrimitiveUp>(binding->original[Layout().device_draw_up])(
        device, primitive, count, vertices, stride);
    error.Capture();
    return hr;
}

HRESULT STDMETHODCALLTYPE DeviceDrawIndexedUp(IDirect3DDevice8* device, D3DPRIMITIVETYPE primitive, UINT min_index,
                                               UINT vertices, UINT count, const void* indices, D3DFORMAT format,
                                               const void* vertex_data, UINT stride) {
    ErrorState error;
    auto binding = Find(device, true);
    if (!binding) return D3DERR_INVALIDCALL;
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
    Observe([&] { t3render::CaptureDraw(device, call); });
    error.Restore();
    HRESULT hr = reinterpret_cast<DrawIndexedUp>(binding->original[Layout().device_draw_indexed_up])(
        device, primitive, min_index, vertices, count, indices, format, vertex_data, stride);
    error.Capture();
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
    Observe([&] { configured = t3render::CaptureConfigured(); });
    // Without a valid opt-in, there is no change to the returned object.
    if (factory && configured && !Bind(factory, false)) {
        Diagnostic("T3Render: could not instrument IDirect3D8; rendering continues without capture\n");
    }
    return factory;
}
