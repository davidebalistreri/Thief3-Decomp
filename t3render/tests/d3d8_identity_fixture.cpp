// Standalone D3D8 COM identity observations. No game assets or buffer wrappers.
#include <windows.h>
#include <d3d8.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
template<class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

const char* Bool(bool value) { return value ? "true" : "false"; }
unsigned long Hr(HRESULT value) { return static_cast<unsigned long>(static_cast<std::uint32_t>(value)); }
unsigned long Ptr(const void* value) { return static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(value)); }
void* Vtable(const void* object) {
    void* table = nullptr;
    if (object) std::memcpy(&table, object, sizeof(table));
    return table;
}

struct Context {
    IDirect3DDevice8* device;
    IUnknown* device_identity;
    bool invalid = false;

    bool Attempt(const char* path, HRESULT hr) {
        std::printf("{\"event\":\"operation\",\"path\":\"%s\",\"hr\":\"0x%08lx\",\"success\":%s}\n",
                    path, Hr(hr), Bool(SUCCEEDED(hr)));
        return SUCCEEDED(hr);
    }

    bool Created(const char* path, HRESULT hr, const void* object) {
        const bool success = SUCCEEDED(hr);
        if (success && !object) invalid = true;
        std::printf("{\"event\":\"create\",\"path\":\"%s\",\"hr\":\"0x%08lx\","
                    "\"success\":%s,\"object_nonnull\":%s}\n", path, Hr(hr), Bool(success), Bool(object != nullptr));
        return success && object;
    }

    bool Identity(IUnknown* object, ComPtr<IUnknown>& identity) {
        const HRESULT hr = object->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(identity.put()));
        if (FAILED(hr) || !identity.value) {
            std::fprintf(stderr, "QueryInterface(IUnknown) failed: 0x%08lx\n", Hr(hr));
            invalid = true;
            return false;
        }
        return true;
    }

    void DeviceResult(const char* path, HRESULT hr, IDirect3DDevice8* found, bool round_trip = true) {
        const bool success = SUCCEEDED(hr);
        if (!success || !found) {
            if (success) invalid = true;
            std::printf("{\"event\":\"device\",\"path\":\"%s\",\"hr\":\"0x%08lx\","
                        "\"success\":%s,\"interface\":null,\"same_interface\":null,"
                        "\"same_iunknown\":null,\"same_vtable\":null}\n", path, Hr(hr), Bool(success));
            return;
        }
        ComPtr<IUnknown> identity;
        if (!Identity(found, identity)) return;
        std::printf("{\"event\":\"device\",\"path\":\"%s\",\"hr\":\"0x%08lx\","
                    "\"success\":true,\"interface\":\"0x%08lx\",\"iunknown\":\"0x%08lx\","
                    "\"same_interface\":%s,\"same_iunknown\":%s,\"same_vtable\":%s}\n",
                    path, Hr(hr), Ptr(found), Ptr(identity.value), Bool(found == device),
                    Bool(identity.value == device_identity), Bool(Vtable(found) == Vtable(device)));
        if (round_trip) {
            char round_path[384]{};
            std::snprintf(round_path, sizeof(round_path), "%s:QI(IUnknown).QI(IDirect3DDevice8)", path);
            ComPtr<IDirect3DDevice8> round_device;
            const HRESULT round_hr = identity->QueryInterface(IID_IDirect3DDevice8,
                                                               reinterpret_cast<void**>(round_device.put()));
            DeviceResult(round_path, round_hr, round_device.value, false);
        }
    }

    template<class Resource> void GetDevice(const char* path, Resource* resource) {
        ComPtr<IDirect3DDevice8> found;
        const HRESULT hr = resource->GetDevice(found.put());
        DeviceResult(path, hr, found.value);
    }

    void ResourceIdentity(const char* path, IUnknown* resource, IUnknown* expected_parent) {
        ComPtr<IUnknown> identity;
        if (!Identity(resource, identity)) return;
        std::printf("{\"event\":\"resource_identity\",\"path\":\"%s\","
                    "\"hr\":\"0x00000000\",\"success\":true,\"iunknown\":\"0x%08lx\","
                    "\"same_parent_iunknown\":%s}\n", path, Ptr(identity.value), Bool(identity.value == expected_parent));
    }

    template<class Resource> void Container(const char* path, Resource* resource, REFIID iid,
                                             const char* iid_name, IUnknown* expected_parent) {
        void* raw = nullptr;
        const HRESULT hr = resource->GetContainer(iid, &raw);
        const bool success = SUCCEEDED(hr);
        if (!success || !raw) {
            if (success) invalid = true;
            std::printf("{\"event\":\"container\",\"path\":\"%s\",\"iid\":\"%s\","
                        "\"hr\":\"0x%08lx\",\"success\":%s,\"same_device_iunknown\":null,"
                        "\"same_parent_iunknown\":null}\n", path, iid_name, Hr(hr), Bool(success));
            return;
        }
        auto* object = reinterpret_cast<IUnknown*>(raw);
        ComPtr<IUnknown> identity;
        if (Identity(object, identity)) {
            std::printf("{\"event\":\"container\",\"path\":\"%s\",\"iid\":\"%s\","
                        "\"hr\":\"0x%08lx\",\"success\":true,\"iunknown\":\"0x%08lx\","
                        "\"same_device_iunknown\":%s,\"same_parent_iunknown\":",
                        path, iid_name, Hr(hr), Ptr(identity.value), Bool(identity.value == device_identity));
            if (expected_parent) std::printf("%s", Bool(identity.value == expected_parent));
            else std::printf("null");
            std::printf("}\n");
            if (IsEqualIID(iid, IID_IDirect3DDevice8)) {
                char device_path[256]{};
                std::snprintf(device_path, sizeof(device_path), "%s:IDirect3DDevice8", path);
                DeviceResult(device_path, hr, reinterpret_cast<IDirect3DDevice8*>(raw));
            } else if (IsEqualIID(iid, IID_IUnknown) && identity.value == device_identity) {
                char device_path[256]{};
                std::snprintf(device_path, sizeof(device_path), "%s:IUnknown.QueryInterface(IDirect3DDevice8)", path);
                ComPtr<IDirect3DDevice8> via_query;
                const HRESULT query_hr = object->QueryInterface(IID_IDirect3DDevice8,
                                                                  reinterpret_cast<void**>(via_query.put()));
                DeviceResult(device_path, query_hr, via_query.value);
            }
        }
        object->Release();
    }

    template<class Resource> void Containers(const char* path, Resource* resource, REFIID parent_iid,
                                              const char* parent_name, IUnknown* expected_parent) {
        Container(path, resource, IID_IUnknown, "IUnknown", expected_parent);
        Container(path, resource, IID_IDirect3DDevice8, "IDirect3DDevice8", expected_parent);
        Container(path, resource, parent_iid, parent_name, expected_parent);
    }
};

bool Probe(HWND window) {
    ComPtr<IDirect3D8> d3d;
    d3d.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d.value) { std::fprintf(stderr, "Direct3DCreate8 failed\n"); return false; }
    D3DDISPLAYMODE mode{};
    HRESULT hr = d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
    if (FAILED(hr)) { std::fprintf(stderr, "GetAdapterDisplayMode: 0x%08lx\n", Hr(hr)); return false; }
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    ComPtr<IDirect3DDevice8> device;
    hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                           D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put());
    if (FAILED(hr)) {
        pp.EnableAutoDepthStencil = FALSE;
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put());
    }
    if (FAILED(hr) || !device.value) { std::fprintf(stderr, "CreateDevice: 0x%08lx\n", Hr(hr)); return false; }
    ComPtr<IUnknown> device_identity;
    hr = device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(device_identity.put()));
    if (FAILED(hr) || !device_identity.value) { std::fprintf(stderr, "device QI(IUnknown): 0x%08lx\n", Hr(hr)); return false; }
    Context ctx{device.value, device_identity.value};
    std::printf("{\"event\":\"header\",\"schema\":1,\"device\":\"0x%08lx\","
                "\"device_iunknown\":\"0x%08lx\",\"auto_depth\":%s}\n",
                Ptr(device.value), Ptr(device_identity.value), Bool(pp.EnableAutoDepthStencil != FALSE));

    ComPtr<IDirect3DDevice8> queried;
    hr = device->QueryInterface(IID_IDirect3DDevice8, reinterpret_cast<void**>(queried.put()));
    ctx.DeviceResult("device.QueryInterface(IDirect3DDevice8)", hr, queried.value);

    ComPtr<IDirect3DSurface8> back, target, depth, image, render_target, depth_created;
    hr = device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put());
    if (ctx.Created("device.GetBackBuffer", hr, back.value)) {
        ctx.GetDevice("device.GetBackBuffer.GetDevice", back.value);
        ctx.Containers("device.GetBackBuffer.GetContainer", back.value, IID_IDirect3DSwapChain8, "IDirect3DSwapChain8", nullptr);
    }
    hr = device->GetRenderTarget(target.put());
    if (ctx.Created("device.GetRenderTarget", hr, target.value)) {
        ctx.GetDevice("device.GetRenderTarget.GetDevice", target.value);
        ctx.Containers("device.GetRenderTarget.GetContainer", target.value, IID_IDirect3DSwapChain8, "IDirect3DSwapChain8", nullptr);
    }
    hr = device->GetDepthStencilSurface(depth.put());
    if (ctx.Created("device.GetDepthStencilSurface", hr, depth.value)) {
        ctx.GetDevice("device.GetDepthStencilSurface.GetDevice", depth.value);
        ctx.Containers("device.GetDepthStencilSurface.GetContainer", depth.value, IID_IDirect3DSwapChain8, "IDirect3DSwapChain8", nullptr);
    }
    hr = device->CreateImageSurface(64, 64, D3DFMT_A8R8G8B8, image.put());
    if (ctx.Created("device.CreateImageSurface", hr, image.value)) {
        ctx.GetDevice("device.CreateImageSurface.GetDevice", image.value);
        ctx.Containers("device.CreateImageSurface.GetContainer", image.value, IID_IDirect3DSwapChain8, "IDirect3DSwapChain8", nullptr);
    }
    hr = device->CreateRenderTarget(64, 64, mode.Format, D3DMULTISAMPLE_NONE, TRUE, render_target.put());
    if (ctx.Created("device.CreateRenderTarget", hr, render_target.value)) {
        ctx.GetDevice("device.CreateRenderTarget.GetDevice", render_target.value);
        ctx.Containers("device.CreateRenderTarget.GetContainer", render_target.value, IID_IDirect3DSwapChain8, "IDirect3DSwapChain8", nullptr);
    }
    hr = device->CreateDepthStencilSurface(64, 64, D3DFMT_D16, D3DMULTISAMPLE_NONE, depth_created.put());
    if (ctx.Created("device.CreateDepthStencilSurface", hr, depth_created.value)) {
        ctx.GetDevice("device.CreateDepthStencilSurface.GetDevice", depth_created.value);
        ctx.Containers("device.CreateDepthStencilSurface.GetContainer", depth_created.value, IID_IDirect3DSwapChain8, "IDirect3DSwapChain8", nullptr);
    }

    ComPtr<IDirect3DTexture8> texture;
    hr = device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.put());
    if (ctx.Created("device.CreateTexture", hr, texture.value)) {
        ctx.GetDevice("texture.GetDevice", texture.value);
        ComPtr<IUnknown> parent; ctx.Identity(texture.value, parent);
        hr = device->SetTexture(0, texture.value);
        if (ctx.Attempt("device.SetTexture", hr)) {
            ComPtr<IDirect3DBaseTexture8> bound;
            hr = device->GetTexture(0, bound.put());
            if (ctx.Created("device.GetTexture", hr, bound.value)) {
                ctx.GetDevice("device.GetTexture.GetDevice", bound.value);
                ctx.ResourceIdentity("device.GetTexture.IUnknown", bound.value, parent.value);
            }
            ctx.Attempt("device.SetTexture(null)", device->SetTexture(0, nullptr));
        }
        ComPtr<IDirect3DSurface8> level;
        hr = texture->GetSurfaceLevel(0, level.put());
        if (ctx.Created("texture.GetSurfaceLevel", hr, level.value)) {
            ctx.GetDevice("texture.GetSurfaceLevel.GetDevice", level.value);
            ctx.Containers("texture.GetSurfaceLevel.GetContainer", level.value, IID_IDirect3DTexture8, "IDirect3DTexture8", parent.value);
        }
    }
    ComPtr<IDirect3DCubeTexture8> cube;
    hr = device->CreateCubeTexture(64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, cube.put());
    if (ctx.Created("device.CreateCubeTexture", hr, cube.value)) {
        ctx.GetDevice("cube.GetDevice", cube.value);
        ComPtr<IUnknown> parent; ctx.Identity(cube.value, parent);
        ComPtr<IDirect3DSurface8> face;
        hr = cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, face.put());
        if (ctx.Created("cube.GetCubeMapSurface", hr, face.value)) {
            ctx.GetDevice("cube.GetCubeMapSurface.GetDevice", face.value);
            ctx.Containers("cube.GetCubeMapSurface.GetContainer", face.value, IID_IDirect3DCubeTexture8, "IDirect3DCubeTexture8", parent.value);
        }
    }
    ComPtr<IDirect3DVolumeTexture8> volume_texture;
    hr = device->CreateVolumeTexture(16, 16, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, volume_texture.put());
    if (ctx.Created("device.CreateVolumeTexture", hr, volume_texture.value)) {
        ctx.GetDevice("volume_texture.GetDevice", volume_texture.value);
        ComPtr<IUnknown> parent; ctx.Identity(volume_texture.value, parent);
        ComPtr<IDirect3DVolume8> volume;
        hr = volume_texture->GetVolumeLevel(0, volume.put());
        if (ctx.Created("volume_texture.GetVolumeLevel", hr, volume.value)) {
            ctx.GetDevice("volume_texture.GetVolumeLevel.GetDevice", volume.value);
            ctx.Containers("volume_texture.GetVolumeLevel.GetContainer", volume.value, IID_IDirect3DVolumeTexture8, "IDirect3DVolumeTexture8", parent.value);
        }
    }
    ComPtr<IDirect3DSwapChain8> swap;
    pp.EnableAutoDepthStencil = FALSE;
    hr = device->CreateAdditionalSwapChain(&pp, swap.put());
    if (ctx.Created("device.CreateAdditionalSwapChain", hr, swap.value)) {
        ComPtr<IUnknown> parent; ctx.Identity(swap.value, parent);
        ComPtr<IDirect3DDevice8> via_query;
        hr = swap->QueryInterface(IID_IDirect3DDevice8, reinterpret_cast<void**>(via_query.put()));
        ctx.DeviceResult("swapchain.QueryInterface(IDirect3DDevice8)", hr, via_query.value);
        ComPtr<IDirect3DSurface8> swap_back;
        hr = swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, swap_back.put());
        if (ctx.Created("swapchain.GetBackBuffer", hr, swap_back.value)) {
            ctx.GetDevice("swapchain.GetBackBuffer.GetDevice", swap_back.value);
            ctx.Containers("swapchain.GetBackBuffer.GetContainer", swap_back.value, IID_IDirect3DSwapChain8, "IDirect3DSwapChain8", parent.value);
        }
    }
    // Clear and Present allow the optional proxy run to prove its hooks wrote
    // a complete diagnostic frame; the probe issues no draw calls.
    hr = device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    std::printf("{\"event\":\"clear\",\"hr\":\"0x%08lx\",\"success\":%s}\n", Hr(hr), Bool(SUCCEEDED(hr)));
    if (FAILED(hr)) ctx.invalid = true;
    hr = device->Present(nullptr, nullptr, nullptr, nullptr);
    std::printf("{\"event\":\"present\",\"hr\":\"0x%08lx\",\"success\":%s}\n", Hr(hr), Bool(SUCCEEDED(hr)));
    if (FAILED(hr)) ctx.invalid = true;
    std::printf("{\"event\":\"end\",\"valid\":%s}\n", Bool(!ctx.invalid));
    return !ctx.invalid;
}
} // namespace

int main() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderD3D8IdentityFixture";
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = instance;
    cls.lpszClassName = name;
    if (!RegisterClassW(&cls)) { std::fprintf(stderr, "RegisterClass failed: %lu\n", GetLastError()); return 1; }
    HWND window = CreateWindowW(name, L"T3Render D3D8 identity", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 128, 128, nullptr, nullptr, instance, nullptr);
    if (!window) { std::fprintf(stderr, "CreateWindow failed: %lu\n", GetLastError()); UnregisterClassW(name, instance); return 1; }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    const bool ok = Probe(window);
    DestroyWindow(window);
    UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
