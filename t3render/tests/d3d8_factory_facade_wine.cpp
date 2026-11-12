// Isolated Wine smoke test for the incomplete factory facade. No device is
// created; CreateDevice must fail closed. This is not a TDS test.
#include "d3d8_factory_facade.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {
template<typename T>
struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T* operator->() const { return value; }
    T** put() { return &value; }
};

bool EqualResult(const char* method, HRESULT native, HRESULT wrapped) {
    const bool equal = native == wrapped;
    std::printf("%s native=0x%08lx facade=0x%08lx %s\n", method,
                static_cast<unsigned long>(static_cast<std::uint32_t>(native)),
                static_cast<unsigned long>(static_cast<std::uint32_t>(wrapped)), equal ? "PASS" : "FAIL");
    return equal;
}
bool EqualMode(const D3DDISPLAYMODE& a, const D3DDISPLAYMODE& b) {
    return a.Width == b.Width && a.Height == b.Height && a.RefreshRate == b.RefreshRate && a.Format == b.Format;
}

bool Run() {
    ComPtr<IDirect3D8> native;
    native.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!native.value) { std::fprintf(stderr, "Direct3DCreate8 failed\n"); return false; }
    native->AddRef(); // separate +1 for the facade; native keeps the original
    ComPtr<IDirect3D8> facade;
    facade.value = t3render::fixture::FactoryFacade::Adopt(native.value);
    if (!facade.value) { native->Release(); std::fprintf(stderr, "Adopt failed\n"); return false; }

    ComPtr<IUnknown> unknown;
    HRESULT hr = facade->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(unknown.put()));
    if (FAILED(hr) || unknown.value != static_cast<IUnknown*>(facade.value)) return false;
    ComPtr<IDirect3D8> again;
    hr = unknown->QueryInterface(IID_IDirect3D8, reinterpret_cast<void**>(again.put()));
    if (FAILED(hr) || again.value != facade.value) return false;
    constexpr GUID unsupported{0x4d3d89aa, 0x1020, 0x3040, {0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0, 0xc0}};
    void* foreign = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    hr = facade->QueryInterface(unsupported, &foreign);
    if (hr != E_NOINTERFACE || foreign) return false;
    std::printf("facade_QI_identity=PASS unsupported_IID=E_NOINTERFACE\n");

    const UINT count = native->GetAdapterCount();
    if (!count || facade->GetAdapterCount() != count) return false;
    std::printf("GetAdapterCount=%u PASS\n", count);
    D3DADAPTER_IDENTIFIER8 native_id{}, wrapped_id{};
    HRESULT n = native->GetAdapterIdentifier(0, 0, &native_id);
    HRESULT f = facade->GetAdapterIdentifier(0, 0, &wrapped_id);
    if (!EqualResult("GetAdapterIdentifier", n, f)) return false;
    if (SUCCEEDED(n) && (std::strncmp(native_id.Driver, wrapped_id.Driver, sizeof(native_id.Driver)) ||
        std::strncmp(native_id.Description, wrapped_id.Description, sizeof(native_id.Description)) ||
        native_id.DriverVersion.QuadPart != wrapped_id.DriverVersion.QuadPart ||
        native_id.VendorId != wrapped_id.VendorId || native_id.DeviceId != wrapped_id.DeviceId ||
        native_id.SubSysId != wrapped_id.SubSysId || native_id.Revision != wrapped_id.Revision ||
        native_id.WHQLLevel != wrapped_id.WHQLLevel || !IsEqualGUID(native_id.DeviceIdentifier, wrapped_id.DeviceIdentifier))) return false;
    D3DADAPTER_IDENTIFIER8 bad_native{}, bad_wrapped{};
    n = native->GetAdapterIdentifier(UINT32_MAX, 0, &bad_native);
    f = facade->GetAdapterIdentifier(UINT32_MAX, 0, &bad_wrapped);
    if (!EqualResult("GetAdapterIdentifier_invalid", n, f)) return false;

    const UINT modes = native->GetAdapterModeCount(0);
    if (facade->GetAdapterModeCount(0) != modes) return false;
    std::printf("GetAdapterModeCount=%u PASS\n", modes);
    if (modes) {
        D3DDISPLAYMODE native_mode{}, wrapped_mode{};
        n = native->EnumAdapterModes(0, 0, &native_mode);
        f = facade->EnumAdapterModes(0, 0, &wrapped_mode);
        if (!EqualResult("EnumAdapterModes", n, f) || (SUCCEEDED(n) && !EqualMode(native_mode, wrapped_mode))) return false;
    } else std::printf("EnumAdapterModes=SKIP no modes\n");
    D3DDISPLAYMODE native_mode{}, wrapped_mode{};
    n = native->GetAdapterDisplayMode(0, &native_mode);
    f = facade->GetAdapterDisplayMode(0, &wrapped_mode);
    if (!EqualResult("GetAdapterDisplayMode", n, f) || (SUCCEEDED(n) && !EqualMode(native_mode, wrapped_mode))) return false;

    n = native->RegisterSoftwareDevice(nullptr);
    f = facade->RegisterSoftwareDevice(nullptr);
    if (!EqualResult("RegisterSoftwareDevice_null", n, f)) return false;
    n = native->CheckDeviceType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE);
    f = facade->CheckDeviceType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE);
    if (!EqualResult("CheckDeviceType", n, f)) return false;
    n = native->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    f = facade->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    if (!EqualResult("CheckDeviceFormat", n, f)) return false;
    n = native->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_NONE);
    f = facade->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_NONE);
    if (!EqualResult("CheckDeviceMultiSampleType", n, f)) return false;
    n = native->CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_D24S8);
    f = facade->CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_D24S8);
    if (!EqualResult("CheckDepthStencilMatch", n, f)) return false;
    D3DCAPS8 native_caps{}, wrapped_caps{};
    n = native->GetDeviceCaps(0, D3DDEVTYPE_HAL, &native_caps);
    f = facade->GetDeviceCaps(0, D3DDEVTYPE_HAL, &wrapped_caps);
    if (!EqualResult("GetDeviceCaps", n, f) || (SUCCEEDED(n) &&
        (native_caps.DeviceType != wrapped_caps.DeviceType || native_caps.AdapterOrdinal != wrapped_caps.AdapterOrdinal ||
         native_caps.MaxStreams != wrapped_caps.MaxStreams || native_caps.MaxTextureWidth != wrapped_caps.MaxTextureWidth ||
         native_caps.MaxTextureHeight != wrapped_caps.MaxTextureHeight))) return false;
    if (native->GetAdapterMonitor(0) != facade->GetAdapterMonitor(0)) return false;
    std::printf("GetAdapterMonitor=PASS\n");

    D3DPRESENT_PARAMETERS presentation{};
    auto* device = reinterpret_cast<IDirect3DDevice8*>(static_cast<std::uintptr_t>(1));
    hr = facade->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, 0, &presentation, &device);
    if (hr != E_NOTIMPL || device) return false;
    std::printf("CreateDevice=E_NOTIMPL no_native_device=PASS\n");
    again->Release(); again.value = nullptr;
    unknown->Release(); unknown.value = nullptr;
    const ULONG facade_remaining = facade->Release(); facade.value = nullptr;
    if (facade_remaining || native->GetAdapterCount() != count) return false;
    const ULONG native_remaining = native->Release(); native.value = nullptr;
    std::printf("facade_final_release=0 native_still_alive=PASS native_final_release=%lu\n",
                static_cast<unsigned long>(native_remaining));
    return true;
}
}  // namespace

int main() {
    const bool ok = Run();
    std::printf("result=%s; fixture-only factory, no TDS or device facade\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
