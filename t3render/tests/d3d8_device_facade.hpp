#pragma once

// Fixture only. This deliberately incomplete COM facade forwards native
// resources unchanged. A resource GetDevice/GetContainer may expose the real
// device. Never use it with resource wrappers, in TDS, or as a COM boundary.
#include <windows.h>
#include <d3d8.h>

#include <atomic>
#include <new>

#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
#include "combined_capture_observer.hpp"
#endif

#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
#include "../src/buffer_proxy.hpp"
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
#include "d3d8_resource_facade.hpp"
#ifndef T3RENDER_BUFFER_FACADE_FIXTURE
#error "The resource experiment requires the isolated buffer facade fixture"
#endif
#endif

namespace t3render::fixture {

#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
inline std::atomic<unsigned> buffer_facade_close_successes{0};
inline std::atomic<unsigned> buffer_facade_close_failures{0};
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
inline std::atomic<unsigned> resource_facade_close_successes{0};
inline std::atomic<unsigned> resource_facade_close_failures{0};
#endif

#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
// Guard the complete native Create/Get plus adoption, so a reset cannot finish
// between the native call and publication of a supposedly fresh buffer.
class FixtureBufferOperation final {
public:
    FixtureBufferOperation(t3render::buffers::BufferProxy* buffers, IDirect3DDevice8* device) noexcept
        : buffers_(buffers), device_(device) {
        const DWORD error = GetLastError();
        ticket_ = buffers_->BeginBindingMutation(device_);
        SetLastError(error);
    }
    ~FixtureBufferOperation() {
        const DWORD error = GetLastError();
        if (ticket_) buffers_->EndBindingMutation(device_, ticket_);
        SetLastError(error);
    }
    FixtureBufferOperation(const FixtureBufferOperation&) = delete;
    FixtureBufferOperation& operator=(const FixtureBufferOperation&) = delete;
private:
    t3render::buffers::BufferProxy* const buffers_;
    IDirect3DDevice8* const device_;
    std::uint64_t ticket_ = 0;
};
#endif

class FixtureReferenceCount final {
public:
    struct Decrement { ULONG count; bool final; };
    ULONG Add() noexcept {
        ULONG value = refs_.load(std::memory_order_relaxed);
        for (;;) {
            // Saturation intentionally leaks instead of wrapping.
            if (!value || value == kSaturated) return value;
            if (refs_.compare_exchange_weak(value, value + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) return value + 1;
        }
    }
    Decrement Release() noexcept {
        ULONG value = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!value || value == kSaturated) return {value, false};
            if (refs_.compare_exchange_weak(value, value - 1, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) return {value - 1, value == 1};
        }
    }
private:
    static constexpr ULONG kSaturated = static_cast<ULONG>(~ULONG{0});
    std::atomic<ULONG> refs_{1};
};

#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
class FixturePrivateObjectBridge final : public t3render::resource_fixture::PrivateObjectBridge {
public:
    FixturePrivateObjectBridge(std::shared_ptr<t3render::buffers::BufferProxy> buffers,
                               IDirect3DDevice8* native, IDirect3DDevice8* facade) noexcept
        : buffers_(std::move(buffers)), native_(native), facade_(facade) {}
    void Attach(const std::shared_ptr<t3render::resource_fixture::Router>& router) noexcept { resources_ = router; }
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
    void SetCombinedObserver(CombinedCaptureObserver* observer) noexcept { observer_ = observer; }
#endif
    void ExternalObserved() noexcept override {
        const DWORD error = GetLastError();
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (observer_) observer_->Unsupported();
        else buffers_->InvalidateDevice(native_);
#else
        buffers_->InvalidateDevice(native_);
#endif
        SetLastError(error);
    }
    bool Pin(const void* candidate, t3render::resource_fixture::PrivateObjectPin& pin) noexcept override {
        bool vertex = false;
        pin.guard = buffers_->PinPrivateBuffer(native_, facade_, candidate, &pin.native, &vertex);
        if (pin.guard && pin.native) {
            pin.kind = vertex ? t3render::buffers::PrivateDataKind::VertexBuffer :
                                t3render::buffers::PrivateDataKind::IndexBuffer;
            return true;
        }
        auto router = resources_.lock();
        return router && router->PinPrivateObject(candidate, pin);
    }
    IUnknown* Adopt(t3render::buffers::PrivateDataKind kind, IUnknown* native) noexcept override {
        if (!native) return nullptr;
        if (t3render::resource_fixture::PrivateResourceIid(kind)) {
            auto router = resources_.lock();
            if (!router) { native->Release(); return nullptr; }
            return router->AdoptPrivateObject(kind, native);
        }
        const bool vertex_kind = kind == t3render::buffers::PrivateDataKind::VertexBuffer;
        if (!vertex_kind && kind != t3render::buffers::PrivateDataKind::IndexBuffer) { native->Release(); return nullptr; }
        // Verify the returned interface and identity before entering the buffer
        // registry. No raw native pointer is ever returned to the caller.
        void* typed = nullptr;
        const HRESULT hr = native->QueryInterface(vertex_kind ? IID_IDirect3DVertexBuffer8 : IID_IDirect3DIndexBuffer8, &typed);
        void* original_identity = nullptr;
        void* typed_identity = nullptr;
        if (!FAILED(hr) && typed) {
            original_identity = t3render::resource_fixture::Identity(native);
            typed_identity = t3render::resource_fixture::Identity(static_cast<IUnknown*>(typed));
        }
        const bool same = original_identity && original_identity == typed_identity;
        native->Release();
        if (FAILED(hr) || !typed) return nullptr;
        if (!same) { static_cast<IUnknown*>(typed)->Release(); return nullptr; }
        if (vertex_kind) {
            auto* vertex = static_cast<IDirect3DVertexBuffer8*>(typed);
            D3DVERTEXBUFFER_DESC desc{};
            if (FAILED(vertex->GetDesc(&desc))) { vertex->Release(); return nullptr; }
            return buffers_->AdoptVertexForFacade(native_, facade_, vertex, desc.Size, false);
        }
        auto* index = static_cast<IDirect3DIndexBuffer8*>(typed);
        D3DINDEXBUFFER_DESC desc{};
        if (FAILED(index->GetDesc(&desc))) { index->Release(); return nullptr; }
        return buffers_->AdoptIndexForFacade(native_, facade_, index, desc.Size, false);
    }
private:
    std::shared_ptr<t3render::buffers::BufferProxy> buffers_;
    std::weak_ptr<t3render::resource_fixture::Router> resources_;
    IDirect3DDevice8* const native_; // borrowed, kept alive by the device facade
    IDirect3DDevice8* const facade_;
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
    CombinedCaptureObserver* observer_ = nullptr; // borrowed; sequential fixture only
#endif
};
#endif

class DeviceFixtureFactory;

class DeviceFixture final : public IDirect3DDevice8 {
public:
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
    // Borrowed fixture-only observer; detach before it or this device dies.
    void SetCombinedObserver(CombinedCaptureObserver* observer) noexcept {
        combined_observer_ = observer;
        if (auto bridge = combined_bridge_.lock()) bridge->SetCombinedObserver(observer);
    }
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    // Test-only borrowed pointer for checking the buffer shadow registry; it
    // must never be handed to an application or used with wrapped inputs.
    IDirect3DDevice8* NativeForFixture() const noexcept { return native_; }
#endif
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3DDevice8)) return E_NOINTERFACE;
        if (!AddRef()) return E_FAIL;
        *output = static_cast<IDirect3DDevice8*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.Add(); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto result = refs_.Release();
        if (!result.final) return result.count;
        const DWORD before = GetLastError();
        IDirect3DDevice8* native = native_;
        IDirect3D8* parent = factory_;
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        SetCombinedObserver(nullptr);
        owned_combined_observer_.reset();
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            if (resources_->Close()) ++resource_facade_close_successes;
            else ++resource_facade_close_failures;
        }
#endif
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        if (buffers_) {
            if (buffers_->CloseFacadeDevice(native_, this)) ++buffer_facade_close_successes;
            else ++buffer_facade_close_failures;
        }
#endif
        delete this;
        SetLastError(before);
        native->Release();
        const DWORD after_native = GetLastError();
        parent->Release();
        SetLastError(after_native);
        return 0;
    }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        IDirect3D8* native_parent = nullptr;
        const HRESULT hr = native_->GetDirect3D(&native_parent);
        const DWORD after_native = GetLastError();
        if (FAILED(hr) || !native_parent) {
            // An output from a failed COM call has no ownership guarantee.
            SetLastError(after_native);
            return FAILED(hr) ? hr : E_FAIL;
        }
        // Compare canonical COM identities while both native references are
        // alive. A different parent must never be silently represented by
        // this factory facade, even if GetDirect3D itself succeeded.
        IUnknown* actual_identity = nullptr;
        IUnknown* expected_identity = nullptr;
        const HRESULT actual_hr = native_parent->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&actual_identity));
        const HRESULT expected_hr = native_factory_->QueryInterface(
            IID_IUnknown, reinterpret_cast<void**>(&expected_identity));
        const bool same = !FAILED(actual_hr) && actual_identity && !FAILED(expected_hr) &&
                          expected_identity && actual_identity == expected_identity;
        // Failed QI outputs have no ownership guarantee.
        if (!FAILED(actual_hr) && actual_identity) actual_identity->Release();
        if (!FAILED(expected_hr) && expected_identity) expected_identity->Release();
        native_parent->Release();
        if (!same) { SetLastError(after_native); return E_FAIL; }
        if (!factory_->AddRef()) { SetLastError(after_native); return E_FAIL; }
        *output = factory_;
        SetLastError(after_native);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override {
        return native_->TestCooperativeLevel();
    }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override {
        return native_->GetAvailableTextureMem();
    }
    HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD Bytes) override {
        return native_->ResourceManagerDiscardBytes(Bytes);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8 * pCaps) override {
        return native_->GetDeviceCaps(pCaps);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE * pMode) override {
        return native_->GetDisplayMode(pMode);
    }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS * pParameters) override {
        return native_->GetCreationParameters(pParameters);
    }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface8 * pCursorBitmap) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const DWORD before = GetLastError();
            auto pin = resources_->UnwrapSurface(pCursorBitmap);
            if (pCursorBitmap && !pin.wrapped()) { SetLastError(before); return D3DERR_INVALIDCALL; }
            SetLastError(before);
            return native_->SetCursorProperties(XHotSpot, YHotSpot, pin.native());
        }
#endif
        return native_->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
    }
    void STDMETHODCALLTYPE SetCursorPosition(UINT XScreenSpace, UINT YScreenSpace,DWORD Flags) override {
        return native_->SetCursorPosition(XScreenSpace, YScreenSpace, Flags);
    }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override {
        return native_->ShowCursor(bShow);
    }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS * pPresentationParameters, IDirect3DSwapChain8 ** pSwapChain) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const HRESULT hr = native_->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && pSwapChain && *pSwapChain) {
                *pSwapChain = resources_->AdoptSwapChain(*pSwapChain);
                SetLastError(error);
                if (!*pSwapChain) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
    }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS * pPresentationParameters) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        auto* observer = combined_observer_;
        const auto shader_ticket = observer ? observer->BeginReset() : t3render::ir::ShaderTicket{};
#endif
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        const DWORD before = GetLastError();
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        const auto ticket = buffers_ ? buffers_->BeginFacadeReset(native_, this) : 0;
#else
        // Native resources in the buffer-only fixture can expose the device;
        // that boundary cannot justify a new observation epoch.
        const std::uint64_t ticket = 0;
#endif
        if (buffers_ && !ticket) buffers_->RetireFacadeDormantForReset(native_, this);
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) resources_->RetireDormantForReset();
#endif
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        SetLastError(before);
        const HRESULT hr = native_->Reset(pPresentationParameters);
        const DWORD after = GetLastError();
        if (buffers_ && ticket) (void)buffers_->EndFacadeReset(native_, ticket, SUCCEEDED(hr));
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (observer) observer->EndReset(shader_ticket, hr, pPresentationParameters);
#endif
        SetLastError(after);
        return hr;
#else
        return native_->Reset(pPresentationParameters);
#endif
    }
    HRESULT STDMETHODCALLTYPE Present(const RECT *src_rect, const RECT *dst_rect, HWND dst_window_override, const RGNDATA *dirty_region) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->Present(src_rect, dst_rect, dst_window_override, dirty_region);
#endif
        return native_->Present(src_rect, dst_rect, dst_window_override, dirty_region);
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT BackBuffer,D3DBACKBUFFER_TYPE Type,IDirect3DSurface8 ** ppBackBuffer) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) return WrapSurfaceResult(native_->GetBackBuffer(BackBuffer, Type, ppBackBuffer), ppBackBuffer);
#endif
        return native_->GetBackBuffer(BackBuffer, Type, ppBackBuffer);
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS * pRasterStatus) override {
        return native_->GetRasterStatus(pRasterStatus);
    }
    void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const D3DGAMMARAMP *ramp) override {
        return native_->SetGammaRamp(flags, ramp);
    }
    void STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP * pRamp) override {
        return native_->GetGammaRamp(pRamp);
    }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width,UINT Height,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DTexture8 ** ppTexture) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const HRESULT hr = native_->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && ppTexture && *ppTexture) {
                *ppTexture = resources_->AdoptTexture(*ppTexture);
                SetLastError(error);
                if (!*ppTexture) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture);
    }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width,UINT Height,UINT Depth,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DVolumeTexture8 ** ppVolumeTexture) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const HRESULT hr = native_->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && ppVolumeTexture && *ppVolumeTexture) {
                *ppVolumeTexture = resources_->AdoptVolumeTexture(*ppVolumeTexture);
                SetLastError(error);
                if (!*ppVolumeTexture) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture);
    }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength,UINT Levels,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DCubeTexture8 ** ppCubeTexture) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const HRESULT hr = native_->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && ppCubeTexture && *ppCubeTexture) {
                *ppCubeTexture = resources_->AdoptCube(*ppCubeTexture);
                SetLastError(error);
                if (!*ppCubeTexture) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture);
    }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length,DWORD Usage,DWORD FVF,D3DPOOL Pool,IDirect3DVertexBuffer8 ** ppVertexBuffer) override {
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        if (buffers_) {
            FixtureBufferOperation operation(buffers_.get(), native_);
            const HRESULT hr = native_->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && ppVertexBuffer && *ppVertexBuffer) {
                *ppVertexBuffer = buffers_->AdoptVertexForFacade(native_, this, *ppVertexBuffer, Length, true);
                SetLastError(error);
                if (!*ppVertexBuffer) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer);
    }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length,DWORD Usage,D3DFORMAT Format,D3DPOOL Pool,IDirect3DIndexBuffer8 ** ppIndexBuffer) override {
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        if (buffers_) {
            FixtureBufferOperation operation(buffers_.get(), native_);
            const HRESULT hr = native_->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && ppIndexBuffer && *ppIndexBuffer) {
                *ppIndexBuffer = buffers_->AdoptIndexForFacade(native_, this, *ppIndexBuffer, Length, true);
                SetLastError(error);
                if (!*ppIndexBuffer) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer);
    }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,BOOL Lockable,IDirect3DSurface8 ** ppSurface) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) return WrapSurfaceResult(native_->CreateRenderTarget(Width, Height, Format, MultiSample, Lockable, ppSurface), ppSurface);
#endif
        return native_->CreateRenderTarget(Width, Height, Format, MultiSample, Lockable, ppSurface);
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width,UINT Height,D3DFORMAT Format,D3DMULTISAMPLE_TYPE MultiSample,IDirect3DSurface8 ** ppSurface) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) return WrapSurfaceResult(native_->CreateDepthStencilSurface(Width, Height, Format, MultiSample, ppSurface), ppSurface);
#endif
        return native_->CreateDepthStencilSurface(Width, Height, Format, MultiSample, ppSurface);
    }
    HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT Width,UINT Height,D3DFORMAT Format,IDirect3DSurface8 ** ppSurface) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) return WrapSurfaceResult(native_->CreateImageSurface(Width, Height, Format, ppSurface), ppSurface);
#endif
        return native_->CreateImageSurface(Width, Height, Format, ppSurface);
    }
    HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8 *src_surface, const RECT *src_rects, UINT rect_count, IDirect3DSurface8 *dst_surface, const POINT *dst_points) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const DWORD before = GetLastError();
            auto source = resources_->UnwrapSurface(src_surface);
            auto destination = resources_->UnwrapSurface(dst_surface);
            if ((src_surface && !source.wrapped()) || (dst_surface && !destination.wrapped())) {
                SetLastError(before); return D3DERR_INVALIDCALL;
            }
            SetLastError(before);
            return native_->CopyRects(source.native(), src_rects, rect_count, destination.native(), dst_points);
        }
#endif
        return native_->CopyRects(src_surface, src_rects, rect_count, dst_surface, dst_points);
    }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8 * pSourceTexture,IDirect3DBaseTexture8 * pDestinationTexture) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const DWORD before = GetLastError();
            auto source = resources_->UnwrapTexture(pSourceTexture);
            auto destination = resources_->UnwrapTexture(pDestinationTexture);
            if ((pSourceTexture && !source.wrapped()) || (pDestinationTexture && !destination.wrapped())) {
                SetLastError(before); return D3DERR_INVALIDCALL;
            }
            SetLastError(before);
            return native_->UpdateTexture(source.native(), destination.native());
        }
#endif
        return native_->UpdateTexture(pSourceTexture, pDestinationTexture);
    }
    HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8 * pDestSurface) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const DWORD before = GetLastError();
            auto pin = resources_->UnwrapSurface(pDestSurface);
            if (pDestSurface && !pin.wrapped()) { SetLastError(before); return D3DERR_INVALIDCALL; }
            SetLastError(before);
            return native_->GetFrontBuffer(pin.native());
        }
#endif
        return native_->GetFrontBuffer(pDestSurface);
    }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8 * pRenderTarget,IDirect3DSurface8 * pNewZStencil) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const DWORD before = GetLastError();
            auto target = resources_->UnwrapSurface(pRenderTarget);
            auto depth = resources_->UnwrapSurface(pNewZStencil);
            if ((pRenderTarget && !target.wrapped()) || (pNewZStencil && !depth.wrapped())) {
                SetLastError(before); return D3DERR_INVALIDCALL;
            }
            SetLastError(before);
            return native_->SetRenderTarget(target.native(), depth.native());
        }
#endif
        return native_->SetRenderTarget(pRenderTarget, pNewZStencil);
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8 ** ppRenderTarget) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) return WrapSurfaceResult(native_->GetRenderTarget(ppRenderTarget), ppRenderTarget);
#endif
        return native_->GetRenderTarget(ppRenderTarget);
    }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8 ** ppZStencilSurface) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) return WrapSurfaceResult(native_->GetDepthStencilSurface(ppZStencilSurface), ppZStencilSurface);
#endif
        return native_->GetDepthStencilSurface(ppZStencilSurface);
    }
    HRESULT STDMETHODCALLTYPE BeginScene() override {
        return native_->BeginScene();
    }
    HRESULT STDMETHODCALLTYPE EndScene() override {
        return native_->EndScene();
    }
    HRESULT STDMETHODCALLTYPE Clear(DWORD rect_count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->Clear(rect_count, rects, flags, color, z, stencil);
#endif
        return native_->Clear(rect_count, rects, flags, color, z, stencil);
    }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override {
        return native_->SetTransform(state, matrix);
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State,D3DMATRIX * pMatrix) override {
        return native_->GetTransform(State, pMatrix);
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX *matrix) override {
        return native_->MultiplyTransform(state, matrix);
    }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT8 *viewport) override {
        return native_->SetViewport(viewport);
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8 * pViewport) override {
        return native_->GetViewport(pViewport);
    }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL8 *material) override {
        return native_->SetMaterial(material);
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8 *pMaterial) override {
        return native_->GetMaterial(pMaterial);
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT8 *light) override {
        return native_->SetLight(index, light);
    }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index,D3DLIGHT8 * pLight) override {
        return native_->GetLight(Index, pLight);
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index,BOOL Enable) override {
        return native_->LightEnable(Index, Enable);
    }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index,BOOL * pEnable) override {
        return native_->GetLightEnable(Index, pEnable);
    }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD index, const float *plane) override {
        return native_->SetClipPlane(index, plane);
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index,float * pPlane) override {
        return native_->GetClipPlane(Index, pPlane);
    }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State,DWORD Value) override {
        return native_->SetRenderState(State, Value);
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State,DWORD * pValue) override {
        return native_->GetRenderState(State, pValue);
    }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        return StateBlockCall([&] { return native_->BeginStateBlock(); });
#elif defined(T3RENDER_BUFFER_FACADE_FIXTURE)
        InvalidateBuffers();
#endif
        return native_->BeginStateBlock();
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD * pToken) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        return StateBlockCall([&] { return native_->EndStateBlock(pToken); });
#elif defined(T3RENDER_BUFFER_FACADE_FIXTURE)
        InvalidateBuffers();
#endif
        return native_->EndStateBlock(pToken);
    }
    HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD Token) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        return StateBlockCall([&] { return native_->ApplyStateBlock(Token); }, CombinedCaptureObserver::StateBlockAction::Apply, Token);
#else
        return StateBlockCall([&] { return native_->ApplyStateBlock(Token); });
#endif
#elif defined(T3RENDER_BUFFER_FACADE_FIXTURE)
        InvalidateBuffers();
#endif
        return native_->ApplyStateBlock(Token);
    }
    HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD Token) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        return StateBlockCall([&] { return native_->CaptureStateBlock(Token); }, CombinedCaptureObserver::StateBlockAction::Capture, Token);
#else
        return StateBlockCall([&] { return native_->CaptureStateBlock(Token); });
#endif
#elif defined(T3RENDER_BUFFER_FACADE_FIXTURE)
        InvalidateBuffers();
#endif
        return native_->CaptureStateBlock(Token);
    }
    HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD Token) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        return StateBlockCall([&] { return native_->DeleteStateBlock(Token); }, CombinedCaptureObserver::StateBlockAction::Delete, Token);
#else
        return StateBlockCall([&] { return native_->DeleteStateBlock(Token); });
#endif
#elif defined(T3RENDER_BUFFER_FACADE_FIXTURE)
        InvalidateBuffers();
#endif
        return native_->DeleteStateBlock(Token);
    }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type,DWORD * pToken) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        return StateBlockCall([&] { return native_->CreateStateBlock(Type, pToken); }, CombinedCaptureObserver::StateBlockAction::Create, 0, pToken, Type);
#else
        return StateBlockCall([&] { return native_->CreateStateBlock(Type, pToken); });
#endif
#elif defined(T3RENDER_BUFFER_FACADE_FIXTURE)
        InvalidateBuffers();
#endif
        return native_->CreateStateBlock(Type, pToken);
    }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS8 *clip_status) override {
        return native_->SetClipStatus(clip_status);
    }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS8 * pClipStatus) override {
        return native_->GetClipStatus(pClipStatus);
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage,IDirect3DBaseTexture8 ** ppTexture) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const HRESULT hr = native_->GetTexture(Stage, ppTexture);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && ppTexture && *ppTexture) {
                *ppTexture = resources_->AdoptBase(*ppTexture);
                SetLastError(error);
                if (!*ppTexture) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->GetTexture(Stage, ppTexture);
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage,IDirect3DBaseTexture8 * pTexture) override {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (resources_) {
            const DWORD before = GetLastError();
            auto pin = resources_->UnwrapTexture(pTexture);
            if (pTexture && !pin.wrapped()) { SetLastError(before); return D3DERR_INVALIDCALL; }
            SetLastError(before);
            return native_->SetTexture(Stage, pin.native());
        }
#endif
        return native_->SetTexture(Stage, pTexture);
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD * pValue) override {
        return native_->GetTextureStageState(Stage, Type, pValue);
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage,D3DTEXTURESTAGESTATETYPE Type,DWORD Value) override {
        return native_->SetTextureStageState(Stage, Type, Value);
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD * pNumPasses) override {
        return native_->ValidateDevice(pNumPasses);
    }
    HRESULT STDMETHODCALLTYPE GetInfo(DWORD DevInfoID,void * pDevInfoStruct,DWORD DevInfoStructSize) override {
        return native_->GetInfo(DevInfoID, pDevInfoStruct, DevInfoStructSize);
    }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT palette_idx, const PALETTEENTRY *entries) override {
        return native_->SetPaletteEntries(palette_idx, entries);
    }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber,PALETTEENTRY * pEntries) override {
        return native_->GetPaletteEntries(PaletteNumber, pEntries);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override {
        return native_->SetCurrentTexturePalette(PaletteNumber);
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT * PaletteNumber) override {
        return native_->GetCurrentTexturePalette(PaletteNumber);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType,UINT StartVertex,UINT PrimitiveCount) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
#endif
        return native_->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType,UINT minIndex,UINT NumVertices,UINT startIndex,UINT primCount) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->DrawIndexed(PrimitiveType, minIndex, NumVertices, startIndex, primCount);
#endif
        return native_->DrawIndexedPrimitive(PrimitiveType, minIndex, NumVertices, startIndex, primCount);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, const void *data, UINT stride) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->DrawPrimitiveUP(primitive_type, primitive_count, data, stride);
#endif
        return native_->DrawPrimitiveUP(primitive_type, primitive_count, data, stride);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count, UINT primitive_count, const void *index_data, D3DFORMAT index_format, const void *data, UINT stride) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->DrawIndexedPrimitiveUP(primitive_type, min_vertex_idx, vertex_count,
                                                                                  primitive_count, index_data, index_format, data, stride);
#endif
        return native_->DrawIndexedPrimitiveUP(primitive_type, min_vertex_idx, vertex_count, primitive_count, index_data, index_format, data, stride);
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex,UINT DestIndex,UINT VertexCount,IDirect3DVertexBuffer8 * pDestBuffer,DWORD Flags) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) combined_observer_->Unsupported();
#endif
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        if (buffers_) {
            const DWORD before = GetLastError();
            auto pin = buffers_->UnwrapVertex(native_, pDestBuffer);
            if (pDestBuffer && !pin.wrapped()) { SetLastError(before); return D3DERR_INVALIDCALL; }
            const auto token = buffers_->BeginBindingMutation(native_);
            buffers_->InvalidateVertex(native_, pDestBuffer);
            SetLastError(before);
            const HRESULT hr = native_->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pin.native(), Flags);
            const DWORD after = GetLastError();
            if (token) buffers_->EndBindingMutation(native_, token);
            SetLastError(after);
            return hr;
        }
#endif
        return native_->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, Flags);
    }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *declaration, const DWORD *byte_code, DWORD *shader, DWORD usage) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->CreateVertexShader(declaration, byte_code, shader, usage);
#endif
        return native_->CreateVertexShader(declaration, byte_code, shader, usage);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD Handle) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->SetVertexShader(Handle);
#endif
        return native_->SetVertexShader(Handle);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD * pHandle) override {
        return native_->GetVertexShader(pHandle);
    }
    HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD Handle) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->DeleteVertexShader(Handle);
#endif
        return native_->DeleteVertexShader(Handle);
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg_idx, const void *data, DWORD count) override {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (combined_observer_) return combined_observer_->SetVertexShaderConstant(reg_idx, data, count);
#endif
        return native_->SetVertexShaderConstant(reg_idx, data, count);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD Register,void * pConstantData,DWORD ConstantCount) override {
        return native_->GetVertexShaderConstant(Register, pConstantData, ConstantCount);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD Handle,void * pData,DWORD * pSizeOfData) override {
        return native_->GetVertexShaderDeclaration(Handle, pData, pSizeOfData);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD Handle,void * pData,DWORD * pSizeOfData) override {
        return native_->GetVertexShaderFunction(Handle, pData, pSizeOfData);
    }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer8 * pStreamData,UINT Stride) override {
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        if (buffers_) {
            const DWORD before = GetLastError();
            auto pin = buffers_->UnwrapVertex(native_, pStreamData);
            if (pStreamData && !pin.wrapped()) { SetLastError(before); return D3DERR_INVALIDCALL; }
            const auto token = buffers_->BeginBindingMutation(native_);
            SetLastError(before);
            const HRESULT hr = native_->SetStreamSource(StreamNumber, pin.native(), Stride);
            const DWORD after = GetLastError();
            if (token) buffers_->EndBindingMutation(native_, token);
            SetLastError(after);
            return hr;
        }
#endif
        return native_->SetStreamSource(StreamNumber, pStreamData, Stride);
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber,IDirect3DVertexBuffer8 ** ppStreamData,UINT * pStride) override {
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        if (buffers_) {
            FixtureBufferOperation operation(buffers_.get(), native_);
            const HRESULT hr = native_->GetStreamSource(StreamNumber, ppStreamData, pStride);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && ppStreamData && *ppStreamData) {
                *ppStreamData = buffers_->AdoptVertexForFacade(native_, this, *ppStreamData, 0, false);
                SetLastError(error);
                if (!*ppStreamData) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->GetStreamSource(StreamNumber, ppStreamData, pStride);
    }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8 * pIndexData,UINT BaseVertexIndex) override {
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        if (buffers_) {
            const DWORD before = GetLastError();
            auto pin = buffers_->UnwrapIndex(native_, pIndexData);
            if (pIndexData && !pin.wrapped()) { SetLastError(before); return D3DERR_INVALIDCALL; }
            const auto token = buffers_->BeginBindingMutation(native_);
            SetLastError(before);
            const HRESULT hr = native_->SetIndices(pin.native(), BaseVertexIndex);
            const DWORD after = GetLastError();
            if (token) buffers_->EndBindingMutation(native_, token);
            SetLastError(after);
            return hr;
        }
#endif
        return native_->SetIndices(pIndexData, BaseVertexIndex);
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8 ** ppIndexData,UINT * pBaseVertexIndex) override {
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        if (buffers_) {
            FixtureBufferOperation operation(buffers_.get(), native_);
            const HRESULT hr = native_->GetIndices(ppIndexData, pBaseVertexIndex);
            const DWORD error = GetLastError();
            if (SUCCEEDED(hr) && ppIndexData && *ppIndexData) {
                *ppIndexData = buffers_->AdoptIndexForFacade(native_, this, *ppIndexData, 0, false);
                SetLastError(error);
                if (!*ppIndexData) return E_FAIL;
            }
            return hr;
        }
#endif
        return native_->GetIndices(ppIndexData, pBaseVertexIndex);
    }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *byte_code, DWORD *shader) override {
        return native_->CreatePixelShader(byte_code, shader);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD Handle) override {
        return native_->SetPixelShader(Handle);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD * pHandle) override {
        return native_->GetPixelShader(pHandle);
    }
    HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD Handle) override {
        return native_->DeletePixelShader(Handle);
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg_idx, const void *data, DWORD count) override {
        return native_->SetPixelShaderConstant(reg_idx, data, count);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD Register,void * pConstantData,DWORD ConstantCount) override {
        return native_->GetPixelShaderConstant(Register, pConstantData, ConstantCount);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD Handle,void * pData,DWORD * pSizeOfData) override {
        return native_->GetPixelShaderFunction(Handle, pData, pSizeOfData);
    }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT handle, const float *segment_count, const D3DRECTPATCH_INFO *patch_info) override {
        return native_->DrawRectPatch(handle, segment_count, patch_info);
    }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT handle, const float *segment_count, const D3DTRIPATCH_INFO *patch_info) override {
        return native_->DrawTriPatch(handle, segment_count, patch_info);
    }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override {
        return native_->DeletePatch(Handle);
    }

private:
    friend class DeviceFixtureFactory;
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
    bool AttachExperimentalObserver(const D3DPRESENT_PARAMETERS* presentation) noexcept {
        if (!buffers_ || !presentation) return false;
        auto observer = std::unique_ptr<CombinedCaptureObserver>(
            new (std::nothrow) CombinedCaptureObserver(native_, *buffers_, *presentation));
        if (!observer || !observer->valid() || !observer->EnableFixtureStateBlocks()) return false;
        SetCombinedObserver(observer.get());
        owned_combined_observer_ = std::move(observer);
        return true;
    }
#endif
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    DeviceFixture(IDirect3DDevice8* native, IDirect3D8* factory, IDirect3D8* native_factory,
                  std::shared_ptr<t3render::buffers::BufferProxy> buffers, bool retain_resources,
                  bool retain_buffers, bool preserve_buffer_shadows) noexcept
        : native_(native), factory_(factory), native_factory_(native_factory), buffers_(std::move(buffers)) {
        factory_->AddRef();
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        try {
            auto bridge = std::make_shared<FixturePrivateObjectBridge>(buffers_, native_, this);
            resources_ = t3render::resource_fixture::Router::Create(native_, this, 4096, bridge, retain_resources);
            if (resources_) {
                bridge->Attach(resources_);
                auto markers = preserve_buffer_shadows ? resources_->PrivateMarkers() : nullptr;
                if (!buffers_->InstallFacadePrivateBridge(native_, this, bridge, retain_buffers, markers)) resources_.reset();
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
                if (resources_) combined_bridge_ = bridge;
#endif
            }
        } catch (...) {}
#else
        (void)retain_resources;
        (void)retain_buffers;
        (void)preserve_buffer_shadows;
#endif
    }
    void InvalidateBuffers() noexcept {
        if (!buffers_) return;
        const DWORD error = GetLastError();
        buffers_->InvalidateDevice(native_);
        SetLastError(error);
    }
#else
    DeviceFixture(IDirect3DDevice8* native, IDirect3D8* factory, IDirect3D8* native_factory) noexcept
        : native_(native), factory_(factory), native_factory_(native_factory) {
        factory_->AddRef();
    }
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    template<class Call>
    HRESULT StateBlockCall(Call call
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        , CombinedCaptureObserver::StateBlockAction action = CombinedCaptureObserver::StateBlockAction::Unsupported,
        DWORD token = 0, DWORD* output = nullptr, D3DSTATEBLOCKTYPE type = D3DSBT_ALL
#endif
    ) {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        auto* observer = combined_observer_;
        const auto shader_ticket = observer ? observer->BeginStateBlock(action, token, type) : t3render::ir::ShaderTicket{};
#endif
        if (!buffers_) {
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
            if (observer) observer->Unsupported();
#endif
            return call();
        }
        FixtureBufferOperation operation(buffers_.get(), native_);
        const HRESULT hr = call();
        const DWORD error = GetLastError();
        // The native failure may have partially changed bindings or recording.
        if (FAILED(hr)) buffers_->InvalidateDevice(native_);
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
        if (observer) observer->EndStateBlock(shader_ticket, action, token, output, hr);
#endif
        SetLastError(error);
        return hr;
    }
    HRESULT WrapSurfaceResult(HRESULT hr, IDirect3DSurface8** output) noexcept {
        const DWORD error = GetLastError();
        if (SUCCEEDED(hr) && output && *output) {
            *output = resources_->AdoptSurface(*output);
            SetLastError(error);
            if (!*output) return E_FAIL;
        }
        return hr;
    }
#endif
    ~DeviceFixture() = default;
    IDirect3DDevice8* const native_;
    IDirect3D8* const factory_;
    IDirect3D8* const native_factory_; // borrowed; kept alive by factory_
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    std::shared_ptr<t3render::buffers::BufferProxy> buffers_;
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    std::shared_ptr<t3render::resource_fixture::Router> resources_;
#endif
    FixtureReferenceCount refs_;
#ifdef T3RENDER_COMBINED_CAPTURE_FIXTURE
    CombinedCaptureObserver* combined_observer_ = nullptr;
    std::weak_ptr<FixturePrivateObjectBridge> combined_bridge_;
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
    std::unique_ptr<CombinedCaptureObserver> owned_combined_observer_;
#endif
#endif
};

class DeviceFixtureFactory final : public IDirect3D8 {
public:
    static IDirect3D8* Adopt(IDirect3D8* native) noexcept {
        const DWORD before = GetLastError();
        IDirect3D8* facade = native ? new (std::nothrow) DeviceFixtureFactory(native) : nullptr;
        SetLastError(before);
        return facade;
    }
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    static IDirect3D8* AdoptBuffers(IDirect3D8* native,
                                    std::shared_ptr<t3render::buffers::BufferProxy> buffers,
                                    bool retain_resources = true, bool retain_buffers = true,
                                    bool preserve_buffer_shadows = false) noexcept {
        const DWORD before = GetLastError();
        auto* facade = native && buffers ? new (std::nothrow) DeviceFixtureFactory(native) : nullptr;
        if (facade) {
            facade->buffers_ = std::move(buffers);
            facade->retain_resources_ = retain_resources; facade->retain_buffers_ = retain_buffers;
            facade->preserve_buffer_shadows_ = preserve_buffer_shadows;
        }
        SetLastError(before);
        return facade;
    }
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
    static IDirect3D8* AdoptExperimental(IDirect3D8* native,
                                         std::shared_ptr<t3render::buffers::BufferProxy> buffers) noexcept {
        auto* facade = static_cast<DeviceFixtureFactory*>(AdoptBuffers(native, std::move(buffers), false, false, true));
        if (facade) facade->auto_combined_observer_ = true;
        return facade;
    }
#endif
#endif
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3D8)) return E_NOINTERFACE;
        if (!AddRef()) return E_FAIL;
        *output = static_cast<IDirect3D8*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.Add(); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto result = refs_.Release();
        if (!result.final) return result.count;
        const DWORD before = GetLastError();
        IDirect3D8* native = native_;
        delete this;
        SetLastError(before);
        native->Release();
        return 0;
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior,
                                            D3DPRESENT_PARAMETERS* presentation, IDirect3DDevice8** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        IDirect3DDevice8* native_device = nullptr;
        const HRESULT hr = native_->CreateDevice(adapter, type, focus, behavior, presentation, &native_device);
        const DWORD after_native = GetLastError();
        if (FAILED(hr) || !native_device) {
            // An output from a failed COM call has no ownership guarantee.
            SetLastError(after_native);
            return FAILED(hr) ? hr : E_FAIL;
        }
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        auto* facade = new (std::nothrow) DeviceFixture(native_device, this, native_, buffers_, retain_resources_, retain_buffers_,
                                                        preserve_buffer_shadows_);
#else
        auto* facade = new (std::nothrow) DeviceFixture(native_device, this, native_);
#endif
        if (!facade) {
            native_device->Release();
            SetLastError(after_native);
            return E_OUTOFMEMORY;
        }
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        if (!facade->resources_) {
            facade->Release();
            SetLastError(after_native);
            return E_OUTOFMEMORY;
        }
#endif
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
        if (auto_combined_observer_ && !facade->AttachExperimentalObserver(presentation)) {
            facade->Release();
            SetLastError(after_native);
            return E_OUTOFMEMORY;
        }
#endif
        *output = facade;
        SetLastError(after_native);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void * pInitializeFunction) override {
        return native_->RegisterSoftwareDevice(pInitializeFunction);
    }
    UINT STDMETHODCALLTYPE GetAdapterCount() override {
        return native_->GetAdapterCount();
    }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 * pIdentifier) override {
        return native_->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
    }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter) override {
        return native_->GetAdapterModeCount(Adapter);
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE * pMode) override {
        return native_->EnumAdapterModes(Adapter, Mode, pMode);
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE * pMode) override {
        return native_->GetAdapterDisplayMode(Adapter, pMode);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL Windowed) override {
        return native_->CheckDeviceType(Adapter, CheckType, DisplayFormat, BackBufferFormat, Windowed);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override {
        return native_->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType) override {
        return native_->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType);
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override {
        return native_->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8 * pCaps) override {
        return native_->GetDeviceCaps(Adapter, DeviceType, pCaps);
    }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override {
        return native_->GetAdapterMonitor(Adapter);
    }

private:
    explicit DeviceFixtureFactory(IDirect3D8* native) noexcept : native_(native) {}
    ~DeviceFixtureFactory() = default;
    IDirect3D8* const native_;
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    std::shared_ptr<t3render::buffers::BufferProxy> buffers_;
    bool retain_resources_ = true;
    bool retain_buffers_ = true;
    bool preserve_buffer_shadows_ = false;
#endif
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
    bool auto_combined_observer_ = false;
#endif
    FixtureReferenceCount refs_;
};

// On success adopts the caller's native +1 reference. On null/allocation
// failure returns null and leaves that reference owned by the caller. Call
// only once per native factory identity; there is no alias deduplication.
inline IDirect3D8* CreateDeviceFacadeFactory(IDirect3D8* native) noexcept {
    return DeviceFixtureFactory::Adopt(native);
}

#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
inline IDirect3D8* CreateBufferDeviceFacadeFactory(IDirect3D8* native,
                                                   std::shared_ptr<t3render::buffers::BufferProxy> buffers) noexcept {
    return DeviceFixtureFactory::AdoptBuffers(native, std::move(buffers));
}
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
inline IDirect3D8* CreateEagerResourceFacadeFactory(IDirect3D8* native,
                                                    std::shared_ptr<t3render::buffers::BufferProxy> buffers) noexcept {
    return DeviceFixtureFactory::AdoptBuffers(native, std::move(buffers), false);
}
inline IDirect3D8* CreateEagerAllFacadeFactory(IDirect3D8* native,
                                               std::shared_ptr<t3render::buffers::BufferProxy> buffers) noexcept {
    return DeviceFixtureFactory::AdoptBuffers(native, std::move(buffers), false, false);
}
inline IDirect3D8* CreatePersistentEagerFacadeFactory(IDirect3D8* native,
                                                      std::shared_ptr<t3render::buffers::BufferProxy> buffers) noexcept {
    return DeviceFixtureFactory::AdoptBuffers(native, std::move(buffers), false, false, true);
}
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
inline IDirect3D8* CreateExperimentalCombinedFacadeFactory(IDirect3D8* native,
                                                           std::shared_ptr<t3render::buffers::BufferProxy> buffers) noexcept {
    return DeviceFixtureFactory::AdoptExperimental(native, std::move(buffers));
}
#endif
#endif
#endif

}  // namespace t3render::fixture
