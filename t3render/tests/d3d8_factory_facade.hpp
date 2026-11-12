#pragma once

// Fixture-only, incomplete D3D8 facade. CreateDevice deliberately fails:
// there is no device facade or resource identity boundary yet. Never use this
// in TDS or any application expecting a functioning D3D8 device.
#include <windows.h>
#include <d3d8.h>

#include <atomic>
#include <limits>
#include <new>

namespace t3render::fixture {

class FactoryFacade final : public IDirect3D8 {
public:
    // Consume the caller's native +1 reference only on success. On allocation
    // failure the caller retains it. No extra native AddRef is performed.
    // The fixture must adopt a native identity only once: no identity registry
    // or native alias deduplication is connected to this isolated component.
    static IDirect3D8* Adopt(IDirect3D8* native) noexcept {
        const DWORD last_error = GetLastError();
        IDirect3D8* result = native ? new (std::nothrow) FactoryFacade(native) : nullptr;
        SetLastError(last_error);
        return result;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3D8)) return E_NOINTERFACE;
        if (!AddRef()) return E_FAIL;
        *result = static_cast<IDirect3D8*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        ULONG value = refs_.load(std::memory_order_relaxed);
        for (;;) {
            // Saturation intentionally leaks instead of wrapping to zero.
            if (!value || value == std::numeric_limits<ULONG>::max()) return value;
            if (refs_.compare_exchange_weak(value, value + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) return value + 1;
        }
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG value = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!value || value == std::numeric_limits<ULONG>::max()) return value;
            if (refs_.compare_exchange_weak(value, value - 1, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) break;
        }
        if (value != 1) return value - 1;
        const DWORD last_error = GetLastError();
        IDirect3D8* native = native_;
        delete this;
        SetLastError(last_error);
        native->Release();
        return 0;
    }

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* callback) override {
        return native_->RegisterSoftwareDevice(callback);
    }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return native_->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER8* output) override {
        return native_->GetAdapterIdentifier(adapter, flags, output);
    }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter) override { return native_->GetAdapterModeCount(adapter); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, UINT mode, D3DDISPLAYMODE* output) override {
        return native_->EnumAdapterModes(adapter, mode, output);
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* output) override {
        return native_->GetAdapterDisplayMode(adapter, output);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display,
                                              D3DFORMAT backbuffer, BOOL windowed) override {
        return native_->CheckDeviceType(adapter, type, display, backbuffer, windowed);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT display,
                                                DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT format) override {
        return native_->CheckDeviceFormat(adapter, type, display, usage, resource, format);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format,
                                                         BOOL windowed, D3DMULTISAMPLE_TYPE sample) override {
        return native_->CheckDeviceMultiSampleType(adapter, type, format, windowed, sample);
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format,
                                                      D3DFORMAT target_format, D3DFORMAT depth_format) override {
        return native_->CheckDepthStencilMatch(adapter, type, adapter_format, target_format, depth_format);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS8* output) override {
        return native_->GetDeviceCaps(adapter, type, output);
    }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override { return native_->GetAdapterMonitor(adapter); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                            IDirect3DDevice8** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return E_NOTIMPL; // Do not expose a native device through this incomplete facade.
    }

private:
    explicit FactoryFacade(IDirect3D8* native) noexcept : native_(native) {}
    ~FactoryFacade() = default;
    IDirect3D8* const native_;
    std::atomic<ULONG> refs_{1};
};

}  // namespace t3render::fixture
