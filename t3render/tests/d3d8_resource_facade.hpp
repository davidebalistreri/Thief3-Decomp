#pragma once

// Isolated COM experiment. This is not a complete device/resource boundary
// and must not be installed in TDS. Unknown private-data provenance, most COM
// private data and unknown IIDs fail closed.
#include <windows.h>
#include <d3d8.h>
#include "../src/private_blob.hpp"
#include "../src/private_marker.hpp"

#ifdef max
#pragma push_macro("max")
#undef max
#define T3RENDER_RESOURCE_RESTORE_MAX
#endif
#ifdef min
#pragma push_macro("min")
#undef min
#define T3RENDER_RESOURCE_RESTORE_MIN
#endif
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#ifdef T3RENDER_RESOURCE_RESTORE_MIN
#pragma pop_macro("min")
#undef T3RENDER_RESOURCE_RESTORE_MIN
#endif
#ifdef T3RENDER_RESOURCE_RESTORE_MAX
#pragma pop_macro("max")
#undef T3RENDER_RESOURCE_RESTORE_MAX
#endif

namespace t3render::resource_fixture {

struct LastError final {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
    void restore() const { SetLastError(value); }
    void capture() { value = GetLastError(); }
};

using t3render::buffers::PrivateObjectBridge;
using t3render::buffers::PrivateObjectPin;
using t3render::markers::kPrivateMarkerGuid;
using t3render::markers::PrivateMarkerCache;
using t3render::markers::PrivateMarkerStore;

inline void* Identity(IUnknown* value) {
    if (!value) return nullptr;
    void* result = nullptr;
    if (FAILED(value->QueryInterface(IID_IUnknown, &result)) || !result) return nullptr;
    static_cast<IUnknown*>(result)->Release();
    return result;
}

enum class Kind { Surface, Texture, Cube, VolumeTexture, Volume, SwapChain };

inline bool Supported(Kind kind, REFIID iid) {
    if (IsEqualIID(iid, IID_IUnknown)) return true;
    if (kind == Kind::Surface) return IsEqualIID(iid, IID_IDirect3DSurface8);
    if (kind == Kind::Volume) return IsEqualIID(iid, IID_IDirect3DVolume8);
    if (kind == Kind::SwapChain) return IsEqualIID(iid, IID_IDirect3DSwapChain8);
    if (IsEqualIID(iid, IID_IDirect3DResource8) || IsEqualIID(iid, IID_IDirect3DBaseTexture8)) return true;
    if (kind == Kind::Texture) return IsEqualIID(iid, IID_IDirect3DTexture8);
    if (kind == Kind::Cube) return IsEqualIID(iid, IID_IDirect3DCubeTexture8);
    return kind == Kind::VolumeTexture && IsEqualIID(iid, IID_IDirect3DVolumeTexture8);
}

struct Entry {
    Kind kind;
    void* identity;
    void* exposed;
    Entry* next = nullptr;
    virtual bool TryAddRef() noexcept = 0;
    virtual bool TryRevive() noexcept = 0;
    virtual bool Dormant() const noexcept = 0;
    virtual IUnknown* Unknown() noexcept = 0;
    virtual IUnknown* NativeUnknown() noexcept = 0;
    virtual IDirect3DSurface8* NativeSurface() noexcept = 0;
    virtual IDirect3DBaseTexture8* NativeTexture() noexcept = 0;
    virtual void DestroyDormant() noexcept = 0;
protected:
    ~Entry() = default;
};

struct State {
    State(IDirect3DDevice8* owner_, IDirect3DDevice8* facade_, std::size_t maximum_,
          std::shared_ptr<PrivateObjectBridge> bridge_, bool retain_dormant_)
        : owner(owner_), facade(facade_), maximum(maximum_), bridge(std::move(bridge_)),
          retain_dormant(retain_dormant_), markers(retain_dormant_ ? nullptr : std::make_shared<PrivateMarkerStore>(maximum_)) {}
    std::mutex mutex;
    IDirect3DDevice8* const owner; // facade must retain this native device
    IDirect3DDevice8* const facade;
    const std::size_t maximum;
    const std::shared_ptr<PrivateObjectBridge> bridge;
    const bool retain_dormant;
    const std::shared_ptr<PrivateMarkerStore> markers;
    bool closing = false;
    std::unordered_map<void*, Entry*> identities;
    std::unordered_map<void*, Entry*> wrappers;
};

template<class Interface> class Wrapper;
template<class Interface>
Interface* Adopt(const std::shared_ptr<State>& state, Interface* native, Kind kind) noexcept;
HRESULT MapContainer(const std::shared_ptr<State>& state, REFIID iid, IUnknown* native, void** output) noexcept;

inline t3render::buffers::PrivateDataKind PrivateKind(Kind kind) noexcept {
    using K = t3render::buffers::PrivateDataKind;
    switch (kind) {
        case Kind::Surface: return K::Surface;
        case Kind::Texture: return K::Texture;
        case Kind::Cube: return K::Cube;
        case Kind::VolumeTexture: return K::VolumeTexture;
        case Kind::Volume: return K::Volume;
        case Kind::SwapChain: return K::SwapChain;
    }
    return K::Blob;
}

inline const IID* PrivateResourceIid(t3render::buffers::PrivateDataKind kind) noexcept {
    using K = t3render::buffers::PrivateDataKind;
    switch (kind) {
        case K::Surface: return &IID_IDirect3DSurface8;
        case K::Texture: return &IID_IDirect3DTexture8;
        case K::Cube: return &IID_IDirect3DCubeTexture8;
        case K::VolumeTexture: return &IID_IDirect3DVolumeTexture8;
        case K::Volume: return &IID_IDirect3DVolume8;
        case K::SwapChain: return &IID_IDirect3DSwapChain8;
        default: return nullptr;
    }
}

// Recognize only a live wrapper from this router, by pointer lookup; never
// dereference an unknown candidate or call COM while holding the map lock.
inline bool PinPrivateResource(const std::shared_ptr<State>& state, const void* candidate,
                               PrivateObjectPin& pin) noexcept {
    if (!candidate) return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->wrappers.find(const_cast<void*>(candidate));
    if (state->closing || found == state->wrappers.end() || !found->second->TryAddRef()) return false;
    pin.guard = found->second->Unknown();
    pin.native = found->second->NativeUnknown();
    pin.kind = PrivateKind(found->second->kind);
    return true;
}

template<class Interface>
class CommonIdentity : public Interface, public Entry {
public:
    CommonIdentity(std::shared_ptr<State> state, Interface* native, Kind kind_, void* identity_) noexcept
        : state_(std::move(state)), real_(native) {
        kind = kind_; identity = identity_; exposed = static_cast<Interface*>(this);
    }
    bool TryAddRef() noexcept override {
        ULONG count = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!count) return false;
            if (count == (std::numeric_limits<ULONG>::max)()) return true;
            if (refs_.compare_exchange_weak(count, count + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) return true;
        }
    }
    bool TryRevive() noexcept override {
        ULONG zero = 0;
        return refs_.compare_exchange_strong(zero, 1, std::memory_order_acq_rel,
                                             std::memory_order_relaxed);
    }
    bool Dormant() const noexcept override { return refs_.load(std::memory_order_acquire) == 0; }
    IUnknown* Unknown() noexcept override { return static_cast<Interface*>(this); }
    IUnknown* NativeUnknown() noexcept override { return real_; }
    void DestroyDormant() noexcept override {
        Interface* native = real_;
        delete static_cast<Wrapper<Interface>*>(this);
        native->Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!Supported(kind, iid)) return E_NOINTERFACE;
        if (!AddRef()) return E_FAIL;
        *output = static_cast<Interface*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        ULONG count = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!count || count == (std::numeric_limits<ULONG>::max)()) return count;
            if (refs_.compare_exchange_weak(count, count + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) return count + 1;
        }
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LastError error;
        ULONG count = 0;
        IDirect3DDevice8* parent = nullptr;
        bool retire = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            count = refs_.load(std::memory_order_relaxed);
            for (;;) {
                if (!count || count == (std::numeric_limits<ULONG>::max)()) return count;
                if (refs_.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) break;
            }
            if (count == 1) {
                parent = state_->facade;
                if (!state_->retain_dormant) {
                    state_->identities.erase(this->identity);
                    state_->wrappers.erase(this->exposed);
                    retire = true;
                }
            }
        }
        // In eager mode no registry-owned native reference survives the last
        // facade reference. Native destruction may reenter the device facade.
        if (retire) this->DestroyDormant();
        // The parent may destroy the registry and this wrapper on final release.
        if (parent) parent->Release();
        return count - 1;
    }
    void DiscardUnpublished() noexcept { delete static_cast<Wrapper<Interface>*>(this); }
protected:
    std::shared_ptr<State> state_;
    Interface* const real_;
private:
    std::atomic<ULONG> refs_{1};
};

template<class Interface>
class Common : public CommonIdentity<Interface> {
public:
    using CommonIdentity<Interface>::CommonIdentity;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** output) override {
        LastError error;
        IDirect3DDevice8* actual = nullptr;
        error.restore();
        const HRESULT hr = this->real_->GetDevice(output ? &actual : nullptr);
        error.capture();
        if (FAILED(hr)) return hr;
        if (output) *output = nullptr;
        void* actual_id = nullptr;
        void* owner_id = nullptr;
        if (actual) {
            actual_id = Identity(actual);
            owner_id = Identity(this->state_->owner);
            actual->Release();
        }
        if (!output || !actual_id || actual_id != owner_id || !this->state_->facade->AddRef()) return E_FAIL;
        *output = this->state_->facade;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
        const GUID key = guid;
        if (!this->state_->retain_dormant && IsEqualIID(key, kPrivateMarkerGuid)) return E_NOTIMPL;
        if (flags == D3DSPD_IUNKNOWN && size == sizeof(IUnknown*)) {
            LastError error;
            PrivateObjectPin pin;
            bool recognized = PinPrivateResource(this->state_, data, pin);
            if (!recognized && this->state_->bridge) recognized = this->state_->bridge->Pin(data, pin);
            if (!recognized) {
                if (this->state_->retain_dormant || !this->state_->bridge || !data) return E_NOTIMPL;
                auto* provenance = PrivateState(true);
                if (!provenance) return E_NOTIMPL;
                this->state_->bridge->ExternalObserved();
                error.restore();
                const HRESULT hr = provenance->SetTyped(key, t3render::buffers::PrivateDataKind::ExternalUnknown,
                    [&] { return this->real_->SetPrivateData(key, data, size, flags); });
                error.capture();
                return hr;
            }
            if (!pin.native || !pin.guard || pin.kind == t3render::buffers::PrivateDataKind::Blob) return E_NOTIMPL;
            auto* provenance = PrivateState(true);
            if (!provenance) return E_NOTIMPL;
            error.restore();
            const HRESULT hr = provenance->SetTyped(key, pin.kind,
                [&] { return this->real_->SetPrivateData(key, pin.native, size, flags); });
            error.capture();
            return hr;
        }
        if (flags != 0) return E_NOTIMPL;
        LastError error;
        auto* provenance = PrivateState(true);
        if (!provenance) return E_NOTIMPL;
        error.restore();
        const HRESULT hr = provenance->Set(key, flags, [&] { return this->real_->SetPrivateData(key, data, size, flags); });
        error.capture(); return hr;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
        const GUID key = guid;
        if (!this->state_->retain_dormant && IsEqualIID(key, kPrivateMarkerGuid)) return E_NOTIMPL;
        LastError outer_error;
        auto* provenance = PrivateState(false);
        if (!provenance) return E_NOTIMPL;
        outer_error.restore();
        const HRESULT result = provenance->GetTyped(key, [&](t3render::buffers::PrivateDataKind kind) {
            if (kind == t3render::buffers::PrivateDataKind::Blob)
                return this->real_->GetPrivateData(key, data, size);
            if (kind == t3render::buffers::PrivateDataKind::ExternalUnknown)
                return this->real_->GetPrivateData(key, data, size);
            const IID* resource_iid = PrivateResourceIid(kind);
            if (!resource_iid && !this->state_->bridge) return E_NOTIMPL;
            // A native resource pointer must never be exposed to the caller.
            // Use local storage even for short buffers or a null size pointer.
            IUnknown* native = nullptr;
            const HRESULT hr = this->real_->GetPrivateData(key, data ? &native : nullptr, size);
            LastError error;
            if (FAILED(hr) || !data) return hr;
            if (!native || !size || *size != sizeof(IUnknown*)) {
                if (native) native->Release();
                return E_FAIL;
            }
            IUnknown* mapped = nullptr;
            if (resource_iid) {
                void* result = nullptr;
                const HRESULT mapped_hr = MapContainer(this->state_, *resource_iid, native, &result);
                if (FAILED(mapped_hr)) return E_FAIL;
                mapped = static_cast<IUnknown*>(result);
            } else mapped = this->state_->bridge->Adopt(kind, native);
            if (!mapped) return E_FAIL;
            std::memcpy(data, &mapped, sizeof(mapped));
            return hr;
        });
        outer_error.capture(); return result;
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        const GUID key = guid;
        if (!this->state_->retain_dormant && IsEqualIID(key, kPrivateMarkerGuid)) return E_NOTIMPL;
        LastError error;
        auto* provenance = PrivateState(false);
        if (!provenance) return E_NOTIMPL;
        error.restore();
        const HRESULT hr = provenance->Free(key, [&] { return this->real_->FreePrivateData(key); });
        error.capture(); return hr;
    }
private:
    t3render::buffers::PrivateBlobState* PrivateState(bool create) noexcept {
        if (this->state_->retain_dormant) return &private_blobs_;
        return marker_cache_.Acquire(*this->state_->markers, this->real_, create,
                                     [](IUnknown* native) { return Identity(native); });
    }
    t3render::buffers::PrivateBlobState private_blobs_;
    PrivateMarkerCache marker_cache_;
};

template<class Interface>
class CommonTexture : public Common<Interface> {
public:
    using Common<Interface>::Common;
    IDirect3DSurface8* NativeSurface() noexcept override { return nullptr; }
    IDirect3DBaseTexture8* NativeTexture() noexcept override { return this->real_; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override { return this->real_->SetPriority(priority); }
    DWORD STDMETHODCALLTYPE GetPriority() override { return this->real_->GetPriority(); }
    void STDMETHODCALLTYPE PreLoad() override { this->real_->PreLoad(); }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return this->real_->GetType(); }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD value) override { return this->real_->SetLOD(value); }
    DWORD STDMETHODCALLTYPE GetLOD() override { return this->real_->GetLOD(); }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return this->real_->GetLevelCount(); }
};


template<>
class Wrapper<IDirect3DSurface8> final : public Common<IDirect3DSurface8> {
public:
    using Common::Common;
    IDirect3DSurface8* NativeSurface() noexcept override { return real_; }
    IDirect3DBaseTexture8* NativeTexture() noexcept override { return nullptr; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID iid, void** output) override {
        LastError error;
        void* native = nullptr;
        error.restore();
        const HRESULT hr = real_->GetContainer(iid, output ? &native : nullptr);
        error.capture();
        if (FAILED(hr)) return hr;
        if (!output || !native) { if (output) *output = nullptr; return E_FAIL; }
        *output = nullptr;
        const HRESULT mapped = MapContainer(state_, iid, reinterpret_cast<IUnknown*>(native), output);
        return FAILED(mapped) ? mapped : hr;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* output) override { return real_->GetDesc(output); }
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* output, const RECT* rect, DWORD flags) override {
        return real_->LockRect(output, rect, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockRect() override { return real_->UnlockRect(); }
};

template<>
class Wrapper<IDirect3DTexture8> final : public CommonTexture<IDirect3DTexture8> {
public:
    using CommonTexture::CommonTexture;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* output) override {
        return real_->GetLevelDesc(level, output);
    }
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface8** output) override {
        LastError error; error.restore();
        const HRESULT hr = real_->GetSurfaceLevel(level, output); error.capture();
        if (SUCCEEDED(hr) && output && *output) {
            *output = Adopt(state_, *output, Kind::Surface);
            if (!*output) return E_FAIL;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* output, const RECT* rect, DWORD flags) override {
        return real_->LockRect(level, output, rect, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override { return real_->UnlockRect(level); }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* rect) override { return real_->AddDirtyRect(rect); }
};

template<>
class Wrapper<IDirect3DCubeTexture8> final : public CommonTexture<IDirect3DCubeTexture8> {
public:
    using CommonTexture::CommonTexture;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* output) override {
        return real_->GetLevelDesc(level, output);
    }
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** output) override {
        LastError error; error.restore();
        const HRESULT hr = real_->GetCubeMapSurface(face, level, output); error.capture();
        if (SUCCEEDED(hr) && output && *output) {
            *output = Adopt(state_, *output, Kind::Surface);
            if (!*output) return E_FAIL;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* output,
                                       const RECT* rect, DWORD flags) override {
        return real_->LockRect(face, level, output, rect, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override {
        return real_->UnlockRect(face, level);
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES face, const RECT* rect) override {
        return real_->AddDirtyRect(face, rect);
    }
};

template<>
class Wrapper<IDirect3DVolume8> final : public Common<IDirect3DVolume8> {
public:
    using Common::Common;
    IDirect3DSurface8* NativeSurface() noexcept override { return nullptr; }
    IDirect3DBaseTexture8* NativeTexture() noexcept override { return nullptr; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID iid, void** output) override {
        LastError error;
        void* native = nullptr;
        error.restore();
        const HRESULT hr = real_->GetContainer(iid, output ? &native : nullptr);
        error.capture();
        if (FAILED(hr)) return hr;
        if (!output || !native) { if (output) *output = nullptr; return E_FAIL; }
        *output = nullptr;
        const HRESULT mapped = MapContainer(state_, iid, reinterpret_cast<IUnknown*>(native), output);
        return FAILED(mapped) ? mapped : hr;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC* output) override { return real_->GetDesc(output); }
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* output, const D3DBOX* box, DWORD flags) override {
        return real_->LockBox(output, box, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockBox() override { return real_->UnlockBox(); }
};

template<>
class Wrapper<IDirect3DVolumeTexture8> final : public CommonTexture<IDirect3DVolumeTexture8> {
public:
    using CommonTexture::CommonTexture;
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DVOLUME_DESC* output) override {
        return real_->GetLevelDesc(level, output);
    }
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level, IDirect3DVolume8** output) override {
        LastError error; error.restore();
        const HRESULT hr = real_->GetVolumeLevel(level, output); error.capture();
        if (SUCCEEDED(hr) && output && *output) {
            *output = Adopt(state_, *output, Kind::Volume);
            if (!*output) return E_FAIL;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockBox(UINT level, D3DLOCKED_BOX* output, const D3DBOX* box, DWORD flags) override {
        return real_->LockBox(level, output, box, flags);
    }
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) override { return real_->UnlockBox(level); }
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX* box) override { return real_->AddDirtyBox(box); }
};

template<>
class Wrapper<IDirect3DSwapChain8> final : public CommonIdentity<IDirect3DSwapChain8> {
public:
    using CommonIdentity::CommonIdentity;
    IDirect3DSurface8* NativeSurface() noexcept override { return nullptr; }
    IDirect3DBaseTexture8* NativeTexture() noexcept override { return nullptr; }
    HRESULT STDMETHODCALLTYPE Present(const RECT* source, const RECT* destination, HWND window,
                                      const RGNDATA* dirty) override {
        return real_->Present(source, destination, window, dirty);
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface8** output) override {
        LastError error; error.restore();
        const HRESULT hr = real_->GetBackBuffer(index, type, output); error.capture();
        if (SUCCEEDED(hr) && output && *output) {
            *output = Adopt(state_, *output, Kind::Surface);
            if (!*output) return E_FAIL;
        }
        return hr;
    }
};

template<class Interface>
Interface* Adopt(const std::shared_ptr<State>& state, Interface* native, Kind kind) noexcept {
    if (!native) return nullptr;
    LastError error;
    if (!state) { native->Release(); return nullptr; }
    void* identity = Identity(native);
    if (!identity || !state->facade->AddRef()) { native->Release(); return nullptr; }
    auto* candidate = new (std::nothrow) Wrapper<Interface>(state, native, kind, identity);
    Entry* result = nullptr;
    bool published = false, revived = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto it = state->identities.find(identity);
        if (!state->closing && it != state->identities.end() && it->second->kind == kind) {
            Entry* item = it->second;
            if (item->TryAddRef() || (revived = item->TryRevive())) result = item;
        } else if (!state->closing && it == state->identities.end() && candidate &&
                   state->identities.size() < state->maximum) {
            try {
                if (state->identities.emplace(identity, candidate).second) {
                    try {
                        if (state->wrappers.emplace(candidate->exposed, candidate).second) {
                            result = candidate; candidate = nullptr; published = true;
                        } else state->identities.erase(identity);
                    } catch (...) { state->identities.erase(identity); }
                }
            } catch (...) {}
        }
    }
    if (candidate) candidate->DiscardUnpublished();
    if (!published && !revived) state->facade->Release();
    if (!published) native->Release();
    return result ? static_cast<Interface*>(result->exposed) : nullptr;
}

template<class Interface>
HRESULT AdoptContainer(const std::shared_ptr<State>& state, IUnknown* native, REFIID requested,
                       REFIID candidate_iid, Kind kind, void** output) noexcept {
    if (!Supported(kind, requested)) return E_NOINTERFACE;
    void* candidate = nullptr;
    const HRESULT hr = native->QueryInterface(candidate_iid, &candidate);
    if (FAILED(hr)) return E_NOINTERFACE;
    if (!candidate) return E_FAIL;
    const void* original_identity = Identity(native);
    const void* candidate_identity = Identity(static_cast<IUnknown*>(candidate));
    if (!original_identity || original_identity != candidate_identity) {
        static_cast<IUnknown*>(candidate)->Release();
        return E_FAIL;
    }
    auto* wrapper = Adopt(state, static_cast<Interface*>(candidate), kind);
    if (!wrapper) return E_FAIL;
    *output = wrapper;
    return S_OK;
}

inline HRESULT MapContainer(const std::shared_ptr<State>& state, REFIID iid, IUnknown* native, void** output) noexcept {
    LastError error;
    void* identity = Identity(native);
    void* owner = Identity(state->owner);
    HRESULT hr = E_NOINTERFACE;
    if (identity && identity == owner &&
        (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirect3DDevice8))) {
        hr = state->facade->QueryInterface(iid, output);
    } else if (identity && state->facade->AddRef()) {
        bool revived = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto it = state->identities.find(identity);
            if (!state->closing && it != state->identities.end() && Supported(it->second->kind, iid) &&
                (it->second->TryAddRef() || (revived = it->second->TryRevive()))) {
                *output = it->second->exposed;
                hr = S_OK;
            }
        }
        if (!revived) state->facade->Release();
    }
    if (identity && identity != owner && hr == E_NOINTERFACE) {
        // A reset may have retired a dormant parent while a child or native
        // state block still retains it. Reconstruct only known D3D interfaces.
        hr = AdoptContainer<IDirect3DTexture8>(state, native, iid, IID_IDirect3DTexture8, Kind::Texture, output);
        if (hr == E_NOINTERFACE)
            hr = AdoptContainer<IDirect3DCubeTexture8>(state, native, iid, IID_IDirect3DCubeTexture8, Kind::Cube, output);
        if (hr == E_NOINTERFACE)
            hr = AdoptContainer<IDirect3DVolumeTexture8>(state, native, iid, IID_IDirect3DVolumeTexture8, Kind::VolumeTexture, output);
        if (hr == E_NOINTERFACE)
            hr = AdoptContainer<IDirect3DSwapChain8>(state, native, iid, IID_IDirect3DSwapChain8, Kind::SwapChain, output);
        if (hr == E_NOINTERFACE)
            hr = AdoptContainer<IDirect3DSurface8>(state, native, iid, IID_IDirect3DSurface8, Kind::Surface, output);
        if (hr == E_NOINTERFACE)
            hr = AdoptContainer<IDirect3DVolume8>(state, native, iid, IID_IDirect3DVolume8, Kind::Volume, output);
    }
    native->Release();
    return hr;
}

class Router final {
public:
    class SurfacePin final {
    public:
        SurfacePin() noexcept = default;
        ~SurfacePin() { Release(); }
        SurfacePin(SurfacePin&& other) noexcept : native_(other.native_), guard_(other.guard_) {
            other.native_ = nullptr; other.guard_ = nullptr;
        }
        SurfacePin& operator=(SurfacePin&& other) noexcept {
            if (this != &other) {
                Release(); native_ = other.native_; guard_ = other.guard_;
                other.native_ = nullptr; other.guard_ = nullptr;
            }
            return *this;
        }
        SurfacePin(const SurfacePin&) = delete;
        SurfacePin& operator=(const SurfacePin&) = delete;
        IDirect3DSurface8* native() const noexcept { return native_; }
        bool wrapped() const noexcept { return guard_ != nullptr; }
    private:
        friend class Router;
        SurfacePin(IDirect3DSurface8* native, IUnknown* guard) noexcept : native_(native), guard_(guard) {}
        void Release() noexcept { if (guard_) { LastError error; guard_->Release(); guard_ = nullptr; } }
        IDirect3DSurface8* native_ = nullptr;
        IUnknown* guard_ = nullptr;
    };
    class TexturePin final {
    public:
        TexturePin() noexcept = default;
        ~TexturePin() { Release(); }
        TexturePin(TexturePin&& other) noexcept : native_(other.native_), guard_(other.guard_) {
            other.native_ = nullptr; other.guard_ = nullptr;
        }
        TexturePin& operator=(TexturePin&& other) noexcept {
            if (this != &other) {
                Release(); native_ = other.native_; guard_ = other.guard_;
                other.native_ = nullptr; other.guard_ = nullptr;
            }
            return *this;
        }
        TexturePin(const TexturePin&) = delete;
        TexturePin& operator=(const TexturePin&) = delete;
        IDirect3DBaseTexture8* native() const noexcept { return native_; }
        bool wrapped() const noexcept { return guard_ != nullptr; }
    private:
        friend class Router;
        TexturePin(IDirect3DBaseTexture8* native, IUnknown* guard) noexcept : native_(native), guard_(guard) {}
        void Release() noexcept { if (guard_) { LastError error; guard_->Release(); guard_ = nullptr; } }
        IDirect3DBaseTexture8* native_ = nullptr;
        IUnknown* guard_ = nullptr;
    };
    static std::shared_ptr<Router> Create(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                                         std::size_t maximum = 4096,
                                         std::shared_ptr<PrivateObjectBridge> bridge = {},
                                         bool retain_dormant = true) noexcept {
        LastError error;
        if (!owner || !facade || !maximum) return {};
        try { return std::shared_ptr<Router>(new Router(std::make_shared<State>(owner, facade, maximum, std::move(bridge),
                                                                                retain_dormant))); }
        catch (...) { return {}; }
    }
    IDirect3DSurface8* AdoptSurface(IDirect3DSurface8* native) noexcept { return Adopt(state_, native, Kind::Surface); }
    IDirect3DTexture8* AdoptTexture(IDirect3DTexture8* native) noexcept { return Adopt(state_, native, Kind::Texture); }
    IDirect3DCubeTexture8* AdoptCube(IDirect3DCubeTexture8* native) noexcept { return Adopt(state_, native, Kind::Cube); }
    IDirect3DVolumeTexture8* AdoptVolumeTexture(IDirect3DVolumeTexture8* native) noexcept {
        return Adopt(state_, native, Kind::VolumeTexture);
    }
    IDirect3DSwapChain8* AdoptSwapChain(IDirect3DSwapChain8* native) noexcept {
        return Adopt(state_, native, Kind::SwapChain);
    }
    std::shared_ptr<PrivateMarkerStore> PrivateMarkers() const noexcept { return state_->markers; }
    bool PinPrivateObject(const void* candidate, PrivateObjectPin& pin) noexcept {
        LastError error;
        return PinPrivateResource(state_, candidate, pin);
    }
    // Consumes the native private-data reference even when mapping fails.
    IUnknown* AdoptPrivateObject(t3render::buffers::PrivateDataKind kind, IUnknown* native) noexcept {
        LastError error;
        if (!native) return nullptr;
        const IID* iid = PrivateResourceIid(kind);
        if (!iid) { native->Release(); return nullptr; }
        void* result = nullptr;
        return FAILED(MapContainer(state_, *iid, native, &result)) ? nullptr : static_cast<IUnknown*>(result);
    }

    // Adopts the native getter's +1. Unknown base texture types fail closed.
    IDirect3DBaseTexture8* AdoptBase(IDirect3DBaseTexture8* native) noexcept {
        if (!native) return nullptr;
        LastError error;
        const auto type = native->GetType();
        void* typed = nullptr;
        HRESULT hr = E_NOINTERFACE;
        if (type == D3DRTYPE_TEXTURE) hr = native->QueryInterface(IID_IDirect3DTexture8, &typed);
        else if (type == D3DRTYPE_CUBETEXTURE) hr = native->QueryInterface(IID_IDirect3DCubeTexture8, &typed);
        else if (type == D3DRTYPE_VOLUMETEXTURE) hr = native->QueryInterface(IID_IDirect3DVolumeTexture8, &typed);
        if (SUCCEEDED(hr) && typed) {
            const void* original_identity = Identity(native);
            const void* typed_identity = Identity(static_cast<IUnknown*>(typed));
            if (!original_identity || original_identity != typed_identity) {
                static_cast<IUnknown*>(typed)->Release(); typed = nullptr; hr = E_FAIL;
            }
        }
        native->Release();
        if (FAILED(hr) || !typed) return nullptr;
        if (type == D3DRTYPE_TEXTURE) return AdoptTexture(static_cast<IDirect3DTexture8*>(typed));
        if (type == D3DRTYPE_CUBETEXTURE) return AdoptCube(static_cast<IDirect3DCubeTexture8*>(typed));
        return AdoptVolumeTexture(static_cast<IDirect3DVolumeTexture8*>(typed));
    }

    // Only known live wrappers are accepted. Never dereference unknown inputs.
    SurfacePin UnwrapSurface(IDirect3DSurface8* value) noexcept {
        LastError error;
        if (!value) return {};
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto it = state_->wrappers.find(value);
        if (state_->closing || it == state_->wrappers.end() || it->second->kind != Kind::Surface ||
            !it->second->TryAddRef()) return {};
        return SurfacePin(it->second->NativeSurface(), it->second->Unknown());
    }
    TexturePin UnwrapTexture(IDirect3DBaseTexture8* value) noexcept {
        LastError error;
        if (!value) return {};
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto it = state_->wrappers.find(value);
        if (state_->closing || it == state_->wrappers.end() ||
            (it->second->kind != Kind::Texture && it->second->kind != Kind::Cube && it->second->kind != Kind::VolumeTexture) ||
            !it->second->TryAddRef()) return {};
        return TexturePin(it->second->NativeTexture(), it->second->Unknown());
    }

    // Release registry-owned native references for dormant wrappers before a
    // reset. Live wrappers remain intact. Getters can re-adopt retained native
    // resources later; unknown identities never escape as raw pointers.
    void RetireDormantForReset() noexcept {
        LastError error;
        Entry* retired = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closing) return;
            for (auto it = state_->wrappers.begin(); it != state_->wrappers.end();) {
                Entry* item = (it++)->second;
                if (item->Dormant()) {
                    state_->identities.erase(item->identity);
                    state_->wrappers.erase(item->exposed);
                    item->next = retired;
                    retired = item;
                }
            }
        }
        while (retired) { Entry* next = retired->next; retired->DestroyDormant(); retired = next; }
    }

    // The facade is still alive and no external wrapper reference may exist.
    // A premature close returns false, prevents reacquisition and may be retried.
    bool Close() noexcept {
        LastError error;
        Entry* retired = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->closing = true;
            for (const auto& item : state_->wrappers) if (!item.second->Dormant()) return false;
            for (auto& item : state_->wrappers) {
                item.second->next = retired;
                retired = item.second;
            }
            state_->wrappers.clear(); state_->identities.clear();
        }
        while (retired) { Entry* next = retired->next; retired->DestroyDormant(); retired = next; }
        return true;
    }
private:
    explicit Router(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

} // namespace t3render::resource_fixture
