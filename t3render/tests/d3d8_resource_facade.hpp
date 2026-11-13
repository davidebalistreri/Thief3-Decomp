#pragma once

// Isolated COM experiment. This is not a complete device/resource boundary
// and must not be installed in TDS. Private data and unknown IIDs fail closed.
#include <windows.h>
#include <d3d8.h>

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

inline void* Identity(IUnknown* value) {
    if (!value) return nullptr;
    void* result = nullptr;
    if (FAILED(value->QueryInterface(IID_IUnknown, &result)) || !result) return nullptr;
    static_cast<IUnknown*>(result)->Release();
    return result;
}

enum class Kind { Surface, Texture, Cube, VolumeTexture, Volume };

inline bool Supported(Kind kind, REFIID iid) {
    if (IsEqualIID(iid, IID_IUnknown)) return true;
    if (kind == Kind::Surface) return IsEqualIID(iid, IID_IDirect3DSurface8);
    if (kind == Kind::Volume) return IsEqualIID(iid, IID_IDirect3DVolume8);
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
    virtual IDirect3DSurface8* NativeSurface() noexcept = 0;
    virtual IDirect3DBaseTexture8* NativeTexture() noexcept = 0;
    virtual void DestroyDormant() noexcept = 0;
protected:
    ~Entry() = default;
};

struct State {
    State(IDirect3DDevice8* owner_, IDirect3DDevice8* facade_, std::size_t maximum_) noexcept
        : owner(owner_), facade(facade_), maximum(maximum_) {}
    std::mutex mutex;
    IDirect3DDevice8* const owner; // facade must retain this native device
    IDirect3DDevice8* const facade;
    const std::size_t maximum;
    bool closing = false;
    std::unordered_map<void*, Entry*> identities;
    std::unordered_map<void*, Entry*> wrappers;
};

template<class Interface> class Wrapper;
template<class Interface>
Interface* Adopt(const std::shared_ptr<State>& state, Interface* native, Kind kind) noexcept;

template<class Interface>
class Common : public Interface, public Entry {
public:
    Common(std::shared_ptr<State> state, Interface* native, Kind kind_, void* identity_) noexcept
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
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            count = refs_.load(std::memory_order_relaxed);
            for (;;) {
                if (!count || count == (std::numeric_limits<ULONG>::max)()) return count;
                if (refs_.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) break;
            }
            if (count == 1) parent = state_->facade;
        }
        // The parent may destroy the registry and this wrapper on final release.
        if (parent) parent->Release();
        return count - 1;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** output) override {
        LastError error;
        IDirect3DDevice8* actual = nullptr;
        error.restore();
        const HRESULT hr = real_->GetDevice(output ? &actual : nullptr);
        error.capture();
        if (FAILED(hr)) return hr;
        if (output) *output = nullptr;
        void* actual_id = nullptr;
        void* owner_id = nullptr;
        if (actual) {
            actual_id = Identity(actual);
            owner_id = Identity(state_->owner);
            actual->Release();
        }
        if (!output || !actual_id || actual_id != owner_id || !state_->facade->AddRef()) return E_FAIL;
        *output = state_->facade;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
    void DiscardUnpublished() noexcept { delete static_cast<Wrapper<Interface>*>(this); }
protected:
    std::shared_ptr<State> state_;
    Interface* const real_;
private:
    std::atomic<ULONG> refs_{1};
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

HRESULT MapContainer(const std::shared_ptr<State>& state, REFIID iid, IUnknown* native, void** output) noexcept;

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
                                         std::size_t maximum = 4096) noexcept {
        LastError error;
        if (!owner || !facade || !maximum) return {};
        try { return std::shared_ptr<Router>(new Router(std::make_shared<State>(owner, facade, maximum))); }
        catch (...) { return {}; }
    }
    IDirect3DSurface8* AdoptSurface(IDirect3DSurface8* native) noexcept { return Adopt(state_, native, Kind::Surface); }
    IDirect3DTexture8* AdoptTexture(IDirect3DTexture8* native) noexcept { return Adopt(state_, native, Kind::Texture); }
    IDirect3DCubeTexture8* AdoptCube(IDirect3DCubeTexture8* native) noexcept { return Adopt(state_, native, Kind::Cube); }
    IDirect3DVolumeTexture8* AdoptVolumeTexture(IDirect3DVolumeTexture8* native) noexcept {
        return Adopt(state_, native, Kind::VolumeTexture);
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
