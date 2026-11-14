#include "d3d8_resource_facade.hpp"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

using namespace t3render::resource_fixture;

namespace {
constexpr IID kExtra{99};

struct Device final : IDirect3DDevice8 {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> destroyed{false};
    std::function<void()> on_final;
    HRESULT QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3DDevice8)) return E_NOINTERFACE;
        AddRef(); *out = static_cast<IDirect3DDevice8*>(this); SetLastError(51); return S_OK;
    }
    ULONG AddRef() override { assert(refs && !destroyed); SetLastError(52); return ++refs; }
    ULONG Release() override {
        const ULONG old = refs.fetch_sub(1); assert(old);
        if (old == 1) { destroyed = true; if (on_final) on_final(); }
        SetLastError(53); return old - 1;
    }
    HRESULT SetStreamSource(UINT, IDirect3DVertexBuffer8*, UINT) override { return E_NOTIMPL; }
    HRESULT GetStreamSource(UINT, IDirect3DVertexBuffer8**, UINT*) override { return E_NOTIMPL; }
    HRESULT SetIndices(IDirect3DIndexBuffer8*, UINT) override { return E_NOTIMPL; }
    HRESULT GetIndices(IDirect3DIndexBuffer8**, UINT*) override { return E_NOTIMPL; }
    HRESULT ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD) override { return E_NOTIMPL; }
};

struct Surface final : IDirect3DSurface8 {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> destroyed{false};
    Device* device = nullptr;
    IUnknown* container = nullptr;
    std::function<void()> on_release;
    HRESULT QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3DSurface8) && !IsEqualIID(iid, kExtra))
            return E_NOINTERFACE;
        AddRef(); *out = static_cast<IDirect3DSurface8*>(this); SetLastError(61); return S_OK;
    }
    ULONG AddRef() override { assert(refs && !destroyed); return ++refs; }
    ULONG Release() override {
        const ULONG old = refs.fetch_sub(1); assert(old);
        if (old == 1) destroyed = true;
        if (on_release) on_release();
        SetLastError(62); return old - 1;
    }
    HRESULT GetDevice(IDirect3DDevice8** out) override {
        if (!out) return E_POINTER;
        *out = device; if (device) device->AddRef(); SetLastError(63); return S_OK;
    }
    HRESULT SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return S_OK; }
    HRESULT GetPrivateData(REFGUID, void*, DWORD*) override { return S_OK; }
    HRESULT FreePrivateData(REFGUID) override { return S_OK; }
    HRESULT GetContainer(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (!container) { *out = nullptr; return E_NOINTERFACE; }
        const HRESULT hr = container->QueryInterface(iid, out); SetLastError(64); return hr;
    }
    HRESULT GetDesc(D3DSURFACE_DESC* out) override {
        if (!out) return E_POINTER; *out = {4, 4}; return S_OK;
    }
    HRESULT LockRect(D3DLOCKED_RECT* out, const RECT*, DWORD) override {
        if (!out) return E_POINTER; out->Pitch = 16; out->pBits = this; return S_OK;
    }
    HRESULT UnlockRect() override { return S_OK; }
};

template<class Interface, Kind Type>
struct TextureBase : Interface {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> destroyed{false};
    Device* device = nullptr;
    Surface* surface = nullptr; // initial surface reference is held by this resource
    D3DRESOURCETYPE reported_type = static_cast<D3DRESOURCETYPE>(Type == Kind::Texture ? 3 : 5);
    bool fail_typed_query = false;
    IUnknown* wrong_typed_identity = nullptr;
    std::function<void()> on_release;
    std::function<HRESULT(REFGUID, const void*, DWORD, DWORD)> on_private_set;
    std::function<HRESULT(REFGUID, void*, DWORD*)> on_private_get;
    std::function<HRESULT(REFGUID)> on_private_free;
    HRESULT QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (fail_typed_query && (IsEqualIID(iid, IID_IDirect3DTexture8) || IsEqualIID(iid, IID_IDirect3DCubeTexture8)))
            return E_NOINTERFACE;
        if (wrong_typed_identity && (IsEqualIID(iid, IID_IDirect3DTexture8) || IsEqualIID(iid, IID_IDirect3DCubeTexture8)))
            return wrong_typed_identity->QueryInterface(iid, out);
        if (!Supported(Type, iid) && !IsEqualIID(iid, kExtra)) return E_NOINTERFACE;
        AddRef(); *out = static_cast<Interface*>(this); SetLastError(71); return S_OK;
    }
    ULONG AddRef() override { assert(refs && !destroyed); return ++refs; }
    ULONG Release() override {
        const ULONG old = refs.fetch_sub(1); assert(old);
        if (old == 1) { destroyed = true; if (surface) surface->Release(); }
        if (on_release) on_release();
        SetLastError(72); return old - 1;
    }
    HRESULT GetDevice(IDirect3DDevice8** out) override {
        if (!out) return E_POINTER;
        *out = device; if (device) device->AddRef(); SetLastError(73); return S_OK;
    }
    HRESULT SetPrivateData(REFGUID key, const void* data, DWORD size, DWORD flags) override {
        return on_private_set ? on_private_set(key, data, size, flags) : S_OK;
    }
    HRESULT GetPrivateData(REFGUID key, void* data, DWORD* size) override {
        return on_private_get ? on_private_get(key, data, size) : S_OK;
    }
    HRESULT FreePrivateData(REFGUID key) override { return on_private_free ? on_private_free(key) : S_OK; }
    DWORD SetPriority(DWORD value) override { return value; }
    DWORD GetPriority() override { return 9; }
    void PreLoad() override {}
    D3DRESOURCETYPE GetType() override { return reported_type; }
    DWORD SetLOD(DWORD value) override { return value; }
    DWORD GetLOD() override { return 0; }
    DWORD GetLevelCount() override { return 1; }
    HRESULT GetLevelDesc(UINT, D3DSURFACE_DESC* out) override {
        if (!out) return E_POINTER; *out = {4, 4}; return S_OK;
    }
    HRESULT GiveSurface(IDirect3DSurface8** out) {
        if (!out) return E_POINTER;
        *out = surface;
        if (!surface) return E_FAIL;
        surface->AddRef(); SetLastError(74); return S_OK;
    }
};

struct Texture final : TextureBase<IDirect3DTexture8, Kind::Texture> {
    HRESULT GetSurfaceLevel(UINT, IDirect3DSurface8** out) override { return GiveSurface(out); }
    HRESULT LockRect(UINT, D3DLOCKED_RECT*, const RECT*, DWORD) override { return S_OK; }
    HRESULT UnlockRect(UINT) override { return S_OK; }
    HRESULT AddDirtyRect(const RECT*) override { return S_OK; }
};
struct Cube final : TextureBase<IDirect3DCubeTexture8, Kind::Cube> {
    HRESULT GetCubeMapSurface(D3DCUBEMAP_FACES, UINT, IDirect3DSurface8** out) override { return GiveSurface(out); }
    HRESULT LockRect(D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD) override { return S_OK; }
    HRESULT UnlockRect(D3DCUBEMAP_FACES, UINT) override { return S_OK; }
    HRESULT AddDirtyRect(D3DCUBEMAP_FACES, const RECT*) override { return S_OK; }
};
struct Volume final : IDirect3DVolume8 {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> destroyed{false};
    Device* device = nullptr;
    IUnknown* container = nullptr;
    HRESULT QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3DVolume8)) return E_NOINTERFACE;
        AddRef(); *out = static_cast<IDirect3DVolume8*>(this); SetLastError(61); return S_OK;
    }
    ULONG AddRef() override { assert(refs && !destroyed); return ++refs; }
    ULONG Release() override {
        const ULONG old = refs.fetch_sub(1); assert(old);
        if (old == 1) destroyed = true;
        SetLastError(62); return old - 1;
    }
    HRESULT GetDevice(IDirect3DDevice8** out) override {
        if (!out) return E_POINTER;
        *out = device; if (device) device->AddRef(); SetLastError(63); return S_OK;
    }
    HRESULT SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return S_OK; }
    HRESULT GetPrivateData(REFGUID, void*, DWORD*) override { return S_OK; }
    HRESULT FreePrivateData(REFGUID) override { return S_OK; }
    HRESULT GetContainer(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (!container) { *out = nullptr; return E_NOINTERFACE; }
        const HRESULT hr = container->QueryInterface(iid, out); SetLastError(64); return hr;
    }
    HRESULT GetDesc(D3DVOLUME_DESC* out) override { if (!out) return E_POINTER; *out = {4, 4, 4}; return S_OK; }
    HRESULT LockBox(D3DLOCKED_BOX* out, const D3DBOX*, DWORD) override {
        if (!out) return E_POINTER; *out = {16, 64, this}; return S_OK;
    }
    HRESULT UnlockBox() override { return S_OK; }
};
struct VolumeTexture final : IDirect3DVolumeTexture8 {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> destroyed{false};
    Device* device = nullptr;
    Volume* volume = nullptr;
    HRESULT QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!Supported(Kind::VolumeTexture, iid)) return E_NOINTERFACE;
        AddRef(); *out = static_cast<IDirect3DVolumeTexture8*>(this); SetLastError(71); return S_OK;
    }
    ULONG AddRef() override { assert(refs && !destroyed); return ++refs; }
    ULONG Release() override {
        const ULONG old = refs.fetch_sub(1); assert(old);
        if (old == 1) { destroyed = true; if (volume) volume->Release(); }
        SetLastError(72); return old - 1;
    }
    HRESULT GetDevice(IDirect3DDevice8** out) override {
        if (!out) return E_POINTER;
        *out = device; if (device) device->AddRef(); SetLastError(73); return S_OK;
    }
    HRESULT SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return S_OK; }
    HRESULT GetPrivateData(REFGUID, void*, DWORD*) override { return S_OK; }
    HRESULT FreePrivateData(REFGUID) override { return S_OK; }
    DWORD SetPriority(DWORD value) override { return value; }
    DWORD GetPriority() override { return 9; }
    void PreLoad() override {}
    D3DRESOURCETYPE GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD SetLOD(DWORD value) override { return value; }
    DWORD GetLOD() override { return 0; }
    DWORD GetLevelCount() override { return 1; }
    HRESULT GetLevelDesc(UINT, D3DVOLUME_DESC* out) override { if (!out) return E_POINTER; *out = {4, 4, 4}; return S_OK; }
    HRESULT GetVolumeLevel(UINT, IDirect3DVolume8** out) override {
        if (!out) return E_POINTER;
        *out = volume; if (!volume) return E_FAIL;
        volume->AddRef(); SetLastError(74); return S_OK;
    }
    HRESULT LockBox(UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD) override { return S_OK; }
    HRESULT UnlockBox(UINT) override { return S_OK; }
    HRESULT AddDirtyBox(const D3DBOX*) override { return S_OK; }
};
struct SwapChain final : IDirect3DSwapChain8 {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> destroyed{false};
    Surface* back = nullptr;
    HRESULT QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!Supported(Kind::SwapChain, iid)) return E_NOINTERFACE;
        AddRef(); *out = static_cast<IDirect3DSwapChain8*>(this); SetLastError(91); return S_OK;
    }
    ULONG AddRef() override { assert(refs && !destroyed); return ++refs; }
    ULONG Release() override {
        const ULONG old = refs.fetch_sub(1); assert(old);
        if (old == 1) { destroyed = true; if (back) back->Release(); }
        SetLastError(92); return old - 1;
    }
    HRESULT Present(const RECT*, const RECT*, HWND, const RGNDATA*) override { SetLastError(93); return S_OK; }
    HRESULT GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface8** out) override {
        if (!out) return E_POINTER;
        *out = back; if (!back) return E_FAIL;
        back->AddRef(); SetLastError(94); return S_OK;
    }
};

void CheckIdentity(IUnknown* item, REFIID iid, void* expected) {
    void* output = nullptr;
    assert(item->QueryInterface(iid, &output) == S_OK && output == expected);
    reinterpret_cast<IUnknown*>(output)->Release();
}
template<class Resource>
void CheckParent(Resource* surface, REFIID iid, void* expected) {
    void* output = nullptr;
    SetLastError(80);
    assert(surface->GetContainer(iid, &output) == S_OK && output == expected && GetLastError() == 64);
    reinterpret_cast<IUnknown*>(output)->Release();
}

void ResourceGraph() {
    Device native, facade;
    Texture texture; Surface level;
    texture.device = &native; texture.surface = &level; level.device = &native; level.container = &texture;
    Cube cube; Surface face;
    cube.device = &native; cube.surface = &face; face.device = &native; face.container = &cube;
    VolumeTexture volume_texture; Volume volume;
    volume_texture.device = &native; volume_texture.volume = &volume; volume.device = &native; volume.container = &volume_texture;
    SwapChain swap; Surface swap_back;
    swap.back = &swap_back; swap_back.device = &native; swap_back.container = &swap;
    Surface back; back.device = &native; back.container = &native;
    auto router = Router::Create(&native, &facade);
    auto* wrapped = router->AdoptTexture(&texture);
    auto* wrapped_cube = router->AdoptCube(&cube);
    auto* wrapped_volume = router->AdoptVolumeTexture(&volume_texture);
    auto* wrapped_swap = router->AdoptSwapChain(&swap);
    auto* wrapped_back = router->AdoptSurface(&back);
    assert(wrapped && wrapped_cube && wrapped_volume && wrapped_swap && wrapped_back && facade.refs == 6);
    texture.AddRef();
    auto* from_base = router->AdoptBase(&texture);
    assert(from_base == wrapped && texture.refs == 1);
    assert(from_base->Release() == 1);
    volume_texture.AddRef();
    from_base = router->AdoptBase(&volume_texture);
    assert(from_base == wrapped_volume && volume_texture.refs == 1);
    assert(from_base->Release() == 1);
    cube.AddRef();
    from_base = router->AdoptBase(&cube);
    assert(from_base == wrapped_cube && cube.refs == 1);
    assert(from_base->Release() == 1);
    {
        auto first_pin = router->UnwrapTexture(wrapped);
        assert(first_pin.wrapped() && first_pin.native() == &texture);
        auto moved = std::move(first_pin);
        assert(!first_pin.wrapped() && moved.wrapped() && moved.native() == &texture);
        auto surface_pin = router->UnwrapSurface(wrapped_back);
        assert(surface_pin.wrapped() && surface_pin.native() == &back);
        assert(!router->UnwrapTexture(&texture).wrapped());
        assert(!router->UnwrapSurface(&back).wrapped());
    }
    CheckIdentity(wrapped, IID_IUnknown, wrapped);
    CheckIdentity(wrapped, IID_IDirect3DResource8, wrapped);
    CheckIdentity(wrapped, IID_IDirect3DBaseTexture8, wrapped);
    CheckIdentity(wrapped_cube, IID_IDirect3DCubeTexture8, wrapped_cube);
    CheckIdentity(wrapped_volume, IID_IDirect3DVolumeTexture8, wrapped_volume);
    CheckIdentity(wrapped_swap, IID_IDirect3DSwapChain8, wrapped_swap);
    CheckIdentity(wrapped_back, IID_IDirect3DSurface8, wrapped_back);
    void* extra = reinterpret_cast<void*>(1);
    assert(wrapped->QueryInterface(kExtra, &extra) == E_NOINTERFACE && !extra);
    DWORD size = 0;
    assert(wrapped->SetPrivateData(kExtra, &facade, sizeof(facade), D3DSPD_IUNKNOWN) == E_NOTIMPL);
    assert(wrapped->GetPrivateData(kExtra, nullptr, &size) == E_NOTIMPL);
    assert(wrapped_back->FreePrivateData(kExtra) == E_NOTIMPL);
    IDirect3DDevice8* parent = nullptr;
    SetLastError(81);
    assert(wrapped->GetDevice(&parent) == S_OK && parent == &facade && GetLastError() == 73);
    parent->Release();
    IDirect3DSurface8 *first = nullptr, *again = nullptr, *cube_face = nullptr, *swapped = nullptr, *swapped_again = nullptr;
    IDirect3DVolume8 *volume_level = nullptr, *volume_again = nullptr;
    assert(wrapped->GetSurfaceLevel(0, &first) == S_OK && first && GetLastError() == 74);
    assert(wrapped->GetSurfaceLevel(0, &again) == S_OK && again == first);
    assert(wrapped_cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &cube_face) == S_OK && cube_face);
    assert(wrapped_volume->GetVolumeLevel(0, &volume_level) == S_OK && volume_level);
    assert(wrapped_volume->GetVolumeLevel(0, &volume_again) == S_OK && volume_again == volume_level);
    assert(wrapped_swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &swapped) == S_OK && swapped);
    assert(wrapped_swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &swapped_again) == S_OK && swapped_again == swapped);
    SetLastError(84);
    assert(wrapped_swap->Present(nullptr, nullptr, nullptr, nullptr) == S_OK && GetLastError() == 93);
    CheckParent(first, IID_IDirect3DTexture8, wrapped);
    CheckParent(first, IID_IDirect3DResource8, wrapped);
    CheckParent(first, IID_IDirect3DBaseTexture8, wrapped);
    CheckParent(first, IID_IUnknown, wrapped);
    CheckParent(cube_face, IID_IDirect3DCubeTexture8, wrapped_cube);
    CheckParent(cube_face, IID_IUnknown, wrapped_cube);
    CheckParent(volume_level, IID_IDirect3DVolumeTexture8, wrapped_volume);
    CheckParent(volume_level, IID_IUnknown, wrapped_volume);
    CheckParent(swapped, IID_IDirect3DSwapChain8, wrapped_swap);
    CheckParent(swapped, IID_IUnknown, wrapped_swap);
    IDirect3DDevice8* swap_device = nullptr;
    assert(swapped->GetDevice(&swap_device) == S_OK && swap_device == &facade);
    swap_device->Release();
    void* unsupported = reinterpret_cast<void*>(1);
    assert(wrapped_swap->QueryInterface(IID_IDirect3DDevice8, &unsupported) == E_NOINTERFACE && !unsupported);
    assert(swapped->GetContainer(IID_IDirect3DDevice8, &unsupported) == E_NOINTERFACE && !unsupported);
    assert(!router->UnwrapTexture(reinterpret_cast<IDirect3DBaseTexture8*>(volume_level)).wrapped());
    assert(!router->UnwrapTexture(reinterpret_cast<IDirect3DBaseTexture8*>(wrapped_swap)).wrapped());
    CheckParent(wrapped_back, IID_IDirect3DDevice8, &facade);
    CheckParent(wrapped_back, IID_IUnknown, &facade);
    assert(again->Release() == 1 && first->Release() == 0 && cube_face->Release() == 0 &&
           volume_again->Release() == 1 && wrapped_volume->Release() == 0);
    // A volume level may reacquire its dormant parent identity.
    CheckParent(volume_level, IID_IUnknown, wrapped_volume);
    assert(volume_level->Release() == 0);
    assert(swapped_again->Release() == 1 && wrapped_swap->Release() == 0);
    CheckParent(swapped, IID_IUnknown, wrapped_swap);
    assert(swapped->Release() == 0);
    // Surface and texture wrappers can both be reacquired after reaching zero.
    assert(wrapped->Release() == 0);
    texture.AddRef();
    auto* recovered = router->AdoptTexture(&texture);
    assert(recovered == wrapped);
    assert(recovered->GetSurfaceLevel(0, &first) == S_OK && first == again);
    CheckParent(first, IID_IUnknown, recovered);
    first->Release(); recovered->Release();
    assert(wrapped_cube->Release() == 0 && wrapped_back->Release() == 0 && facade.refs == 1);
    assert(router->Close());
    assert(texture.destroyed && cube.destroyed && volume_texture.destroyed && volume.destroyed && swap.destroyed &&
           level.destroyed && face.destroyed && back.destroyed && swap_back.destroyed);
    assert(native.Release() == 0 && facade.Release() == 0);
}

void FailClosedAndLifetime() {
    {
        Device native, wrong, facade;
        Surface raw; raw.device = &wrong; raw.container = &wrong;
        auto router = Router::Create(&native, &facade);
        auto* wrapped = router->AdoptSurface(&raw);
        IDirect3DDevice8* output = reinterpret_cast<IDirect3DDevice8*>(1);
        SetLastError(82);
        assert(wrapped->GetDevice(&output) == E_FAIL && !output && GetLastError() == 63);
        void* container = reinterpret_cast<void*>(1);
        assert(wrapped->GetContainer(IID_IUnknown, &container) == E_NOINTERFACE && !container);
        assert(wrong.refs == 1 && native.refs == 1 && facade.refs == 2);
        assert(!router->Close()); // cannot destroy an active wrapper
        assert(wrapped->Release() == 0);
        assert(router->Close() && raw.destroyed);
        assert(wrong.Release() == 0 && native.Release() == 0 && facade.Release() == 0);
    }
    {
        Device native, facade;
        Surface raw; raw.device = &native; raw.container = &native;
        auto router = Router::Create(&native, &facade);
        auto* wrapped = router->AdoptSurface(&raw);
        raw.AddRef();
        assert(!router->Close());
        assert(!router->AdoptSurface(&raw) && raw.refs == 1);
        assert(wrapped->Release() == 0);
        bool reentered = false;
        raw.on_release = [&] { reentered = router->Close(); };
        assert(router->Close() && reentered && raw.destroyed);
        assert(native.Release() == 0 && facade.Release() == 0);
    }
    {
        Device native, facade;
        Texture raw; raw.device = &native;
        auto router = Router::Create(&native, &facade, 8, {}, false);
        auto* wrapped = router->AdoptTexture(&raw);
        unsigned writes = 0;
        raw.on_private_get = [&](REFGUID key, void*, DWORD* count) {
            assert(IsEqualIID(key, kPrivateMarkerGuid));
            if (count) *count = 3;
            return S_OK; // foreign private data at the reserved GUID
        };
        raw.on_private_set = [&](REFGUID, const void*, DWORD, DWORD) { ++writes; return S_OK; };
        const BYTE value = 1;
        assert(wrapped->SetPrivateData(kExtra, &value, sizeof(value), 0) == E_NOTIMPL && writes == 0);
        assert(wrapped->Release() == 0 && raw.destroyed && router->Close());
        assert(native.Release() == 0 && facade.Release() == 0);
    }
    {
        Device native, facade;
        Surface raw; raw.device = &native;
        auto router = Router::Create(&native, &facade);
        auto* wrapped = router->AdoptSurface(&raw);
        bool closed = false;
        facade.on_final = [&] { closed = router->Close(); };
        assert(facade.Release() == 1);
        assert(wrapped->Release() == 0 && closed && raw.destroyed && facade.destroyed);
        assert(native.Release() == 0);
    }
    {
        Device native, facade;
        Surface first, overflow;
        auto router = Router::Create(&native, &facade, 1);
        auto* wrapped = router->AdoptSurface(&first);
        assert(!router->AdoptSurface(&overflow) && overflow.destroyed && facade.refs == 2);
        assert(wrapped->Release() == 0 && router->Close() && first.destroyed);
        assert(native.Release() == 0 && facade.Release() == 0);
    }
    for (bool fail_query : {false, true}) {
        Device native, facade;
        Texture raw;
        raw.reported_type = fail_query ? D3DRTYPE_TEXTURE : D3DRTYPE_INDEXBUFFER;
        raw.fail_typed_query = fail_query;
        auto router = Router::Create(&native, &facade);
        SetLastError(83);
        assert(!router->AdoptBase(&raw) && raw.destroyed && GetLastError() == 83 && facade.refs == 1);
        assert(router->Close() && native.Release() == 0 && facade.Release() == 0);
    }
    {
        Device native, facade;
        Texture raw, wrong;
        raw.wrong_typed_identity = &wrong;
        auto router = Router::Create(&native, &facade);
        assert(!router->AdoptBase(&raw) && raw.destroyed && wrong.refs == 1 && !wrong.destroyed);
        assert(wrong.Release() == 0 && router->Close() && native.Release() == 0 && facade.Release() == 0);
    }
}

void ResetRetiresDormantAndReconstructsParent() {
    Device native, facade;
    Texture raw; Surface level;
    raw.device = &native; raw.surface = &level; level.device = &native; level.container = &raw;
    auto router = Router::Create(&native, &facade);
    auto* texture = router->AdoptTexture(&raw);
    IDirect3DSurface8* child = nullptr;
    assert(texture->GetSurfaceLevel(0, &child) == S_OK && child);
    raw.AddRef(); // A native state block or child retains the parent.
    assert(texture->Release() == 0 && raw.refs == 2);
    SetLastError(86);
    router->RetireDormantForReset();
    assert(GetLastError() == 86 && raw.refs == 1 && !raw.destroyed);
    Texture wrong;
    raw.wrong_typed_identity = &wrong;
    void* invalid = reinterpret_cast<void*>(1);
    assert(child->GetContainer(IID_IUnknown, &invalid) == E_FAIL && !invalid && wrong.refs == 1);
    raw.wrong_typed_identity = nullptr;
    void* result = nullptr;
    assert(child->GetContainer(IID_IUnknown, &result) == S_OK && result && result != &raw);
    auto* recovered = reinterpret_cast<IUnknown*>(result);
    void* typed = nullptr;
    assert(recovered->QueryInterface(IID_IDirect3DTexture8, &typed) == S_OK && typed == result);
    auto* typed_texture = static_cast<IDirect3DTexture8*>(typed);
    IDirect3DSurface8* same_child = nullptr;
    assert(typed_texture->GetSurfaceLevel(0, &same_child) == S_OK && same_child == child);
    same_child->Release(); typed_texture->Release(); recovered->Release();
    assert(child->Release() == 0);
    router->RetireDormantForReset();
    assert(raw.refs == 1 && !raw.destroyed && !level.destroyed);
    assert(raw.Release() == 0 && raw.destroyed && level.destroyed && wrong.Release() == 0);
    assert(router->Close() && native.Release() == 0 && facade.Release() == 0);
}

void ConcurrentReacquisition() {
    for (unsigned n = 0; n < 200; ++n) {
        Device native, facade;
        Surface raw; raw.device = &native;
        auto router = Router::Create(&native, &facade);
        auto* wrapped = router->AdoptSurface(&raw);
        raw.AddRef(); // native getter owns a reference during the race
        std::atomic<bool> go{false};
        std::thread getter([&] {
            while (!go.load()) std::this_thread::yield();
            auto* found = router->AdoptSurface(&raw);
            assert(found == wrapped);
            found->Release();
        });
        go = true;
        wrapped->Release();
        getter.join();
        assert(raw.refs == 1 && facade.refs == 1 && router->Close() && raw.destroyed);
        assert(native.Release() == 0 && facade.Release() == 0);
    }
    for (unsigned n = 0; n < 200; ++n) {
        Device native, facade;
        Surface raw; raw.device = &native;
        auto router = Router::Create(&native, &facade);
        auto* wrapped = router->AdoptSurface(&raw);
        std::atomic<bool> go{false};
        bool closed = false;
        std::thread closer([&] { while (!go.load()) std::this_thread::yield(); closed = router->Close(); });
        go = true;
        wrapped->Release();
        closer.join();
        if (!closed) assert(router->Close());
        assert(raw.destroyed && facade.refs == 1);
        assert(native.Release() == 0 && facade.Release() == 0);
    }
}

void PrivateVertexTranslation() {
    struct Bridge final : PrivateObjectBridge {
        Device* target_native;
        Device* target_facade;
        Device* index_native;
        Device* index_facade;
        bool fail_adopt = false;
        Bridge(Device* native, Device* facade, Device* index, Device* index_wrapper)
            : target_native(native), target_facade(facade), index_native(index), index_facade(index_wrapper) {}
        bool Pin(const void* candidate, PrivateObjectPin& pin) noexcept override {
            if (candidate == target_facade) {
                pin.guard = target_facade; pin.native = target_native;
                pin.kind = t3render::buffers::PrivateDataKind::VertexBuffer;
            } else if (candidate == index_facade) {
                pin.guard = index_facade; pin.native = index_native;
                pin.kind = t3render::buffers::PrivateDataKind::IndexBuffer;
            } else return false;
            pin.guard->AddRef();
            return true;
        }
        IUnknown* Adopt(t3render::buffers::PrivateDataKind kind, IUnknown* native) noexcept override {
            const bool vertex = kind == t3render::buffers::PrivateDataKind::VertexBuffer;
            assert(vertex || kind == t3render::buffers::PrivateDataKind::IndexBuffer);
            assert(native == (vertex ? target_native : index_native));
            native->Release();
            if (fail_adopt) return nullptr;
            auto* result = vertex ? target_facade : index_facade;
            result->AddRef(); return result;
        }
    };
    Device native, facade, target_native, target_facade, index_native, index_facade;
    Texture texture; texture.device = &native;
    auto bridge = std::make_shared<Bridge>(&target_native, &target_facade, &index_native, &index_facade);
    auto router = Router::Create(&native, &facade, 8, bridge);
    auto* wrapped = router->AdoptTexture(&texture);
    assert(wrapped);
    IUnknown* stored = nullptr;
    bool expect_index = false;
    texture.on_private_set = [&](REFGUID, const void* data, DWORD size, DWORD flags) {
        assert(data == (expect_index ? &index_native : &target_native) && size == sizeof(IUnknown*) && flags == D3DSPD_IUNKNOWN);
        DWORD nested_size = 0;
        SetLastError(201);
        assert(wrapped->GetPrivateData(kExtra, nullptr, &nested_size) == E_NOTIMPL && GetLastError() == 201);
        auto* value = const_cast<IUnknown*>(static_cast<const IUnknown*>(data));
        value->AddRef(); if (stored) stored->Release(); stored = value;
        SetLastError(202); return S_OK;
    };
    texture.on_private_get = [&](REFGUID, void* data, DWORD* size) {
        if (!size || !stored) return E_FAIL;
        const DWORD available = *size;
        *size = sizeof(IUnknown*);
        if (!data) { SetLastError(203); return S_OK; }
        if (available < sizeof(IUnknown*)) { SetLastError(204); return E_FAIL; }
        stored->AddRef(); std::memcpy(data, &stored, sizeof(stored)); SetLastError(205); return S_OK;
    };
    texture.on_private_free = [&](REFGUID) {
        if (!stored) return E_FAIL;
        stored->Release(); stored = nullptr; SetLastError(206); return S_OK;
    };
    assert(wrapped->SetPrivateData(kExtra, &target_facade, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == S_OK);
    assert(GetLastError() == 202 && target_native.refs == 2 && target_facade.refs == 1);
    assert(wrapped->SetPrivateData(kExtra, &target_native, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == E_NOTIMPL);
    assert(wrapped->SetPrivateData(kExtra, &target_facade, 1, D3DSPD_IUNKNOWN) == E_NOTIMPL);
    DWORD size = 0;
    assert(wrapped->GetPrivateData(kExtra, nullptr, &size) == S_OK && size == sizeof(IUnknown*) && GetLastError() == 203);
    IUnknown* result = reinterpret_cast<IUnknown*>(1);
    size = 1;
    assert(wrapped->GetPrivateData(kExtra, &result, &size) == E_FAIL && result == reinterpret_cast<IUnknown*>(1));
    assert(size == sizeof(IUnknown*) && GetLastError() == 204 && target_native.refs == 2);
    size = sizeof(result);
    assert(wrapped->GetPrivateData(kExtra, &result, &size) == S_OK && result == &target_facade && GetLastError() == 205);
    assert(target_native.refs == 2 && target_facade.refs == 2);
    result->Release();
    bridge->fail_adopt = true;
    result = reinterpret_cast<IUnknown*>(1); size = sizeof(result);
    assert(wrapped->GetPrivateData(kExtra, &result, &size) == E_FAIL && result == reinterpret_cast<IUnknown*>(1));
    assert(target_native.refs == 2 && target_facade.refs == 1 && GetLastError() == 205);
    bridge->fail_adopt = false;
    assert(wrapped->FreePrivateData(kExtra) == S_OK && target_native.refs == 1 && GetLastError() == 206);
    size = sizeof(result);
    assert(wrapped->GetPrivateData(kExtra, &result, &size) == E_FAIL);
    expect_index = true;
    assert(wrapped->SetPrivateData(kExtra, &index_facade, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == S_OK);
    size = sizeof(result);
    assert(wrapped->GetPrivateData(kExtra, &result, &size) == S_OK && result == &index_facade);
    assert(index_native.refs == 2 && index_facade.refs == 2);
    result->Release();
    assert(wrapped->FreePrivateData(kExtra) == S_OK && index_native.refs == 1 && index_facade.refs == 1);
    expect_index = false;

    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, proceed = false;
    const auto original_set = texture.on_private_set;
    texture.on_private_set = [&](REFGUID key, const void* data, DWORD count, DWORD flags) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true; condition.notify_one();
            condition.wait(lock, [&] { return proceed; });
        }
        return original_set(key, data, count, flags);
    };
    std::thread writer([&] {
        assert(wrapped->SetPrivateData(kExtra, &target_facade, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == S_OK);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    size = sizeof(result);
    assert(wrapped->GetPrivateData(kExtra, &result, &size) == E_NOTIMPL);
    assert(wrapped->FreePrivateData(kExtra) == E_NOTIMPL);
    {
        std::lock_guard<std::mutex> lock(mutex); proceed = true;
    }
    condition.notify_one(); writer.join();
    assert(target_facade.refs == 1 && target_native.refs == 2);
    assert(wrapped->FreePrivateData(kExtra) == S_OK && target_native.refs == 1);
    assert(wrapped->Release() == 0 && router->Close() && texture.destroyed);
    assert(native.Release() == 0 && facade.Release() == 0 && target_native.Release() == 0 && target_facade.Release() == 0 &&
           index_native.Release() == 0 && index_facade.Release() == 0);
}

void PrivateResourceTranslation() {
    Device native, facade;
    Texture texture; texture.device = &native;
    Cube cube; cube.device = &native;
    auto router = Router::Create(&native, &facade);
    auto* source = router->AdoptTexture(&texture);
    auto* target = router->AdoptCube(&cube);
    assert(source && target);
    IUnknown* texture_data = nullptr;
    IUnknown* cube_data = nullptr;
    auto enable_data = [&](auto& resource, IUnknown*& slot) {
        IUnknown** entry = &slot;
        resource.on_private_set = [&, entry](REFGUID, const void* value, DWORD size, DWORD flags) {
            assert((value == &texture || value == &cube) && size == sizeof(IUnknown*) && flags == D3DSPD_IUNKNOWN);
            auto* native_value = const_cast<IUnknown*>(static_cast<const IUnknown*>(value));
            native_value->AddRef(); if (*entry) (*entry)->Release(); *entry = native_value;
            SetLastError(210); return S_OK;
        };
        resource.on_private_get = [entry](REFGUID, void* data, DWORD* size) {
            if (!*entry || !size || !data || *size < sizeof(IUnknown*)) return E_FAIL;
            (*entry)->AddRef(); std::memcpy(data, entry, sizeof(*entry)); *size = sizeof(IUnknown*);
            SetLastError(211); return S_OK;
        };
        resource.on_private_free = [entry](REFGUID) {
            if (!*entry) return E_FAIL;
            (*entry)->Release(); *entry = nullptr; SetLastError(212); return S_OK;
        };
    };
    enable_data(texture, texture_data);
    enable_data(cube, cube_data);

    // A native self-reference is released explicitly, as it is in native COM.
    assert(source->SetPrivateData(kExtra, source, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == S_OK);
    assert(texture_data == &texture && texture.refs == 2 && GetLastError() == 210);
    IUnknown* found = nullptr; DWORD size = sizeof(found);
    assert(source->GetPrivateData(kExtra, &found, &size) == S_OK && found == source && GetLastError() == 211);
    found->Release();
    assert(source->FreePrivateData(kExtra) == S_OK && texture.refs == 1 && GetLastError() == 212);

    assert(source->SetPrivateData(kExtra, target, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == S_OK);
    assert(target->SetPrivateData(kExtra, source, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == S_OK);
    assert(texture_data == &cube && cube_data == &texture && texture.refs == 2 && cube.refs == 2);
    size = sizeof(found); found = nullptr;
    assert(source->GetPrivateData(kExtra, &found, &size) == S_OK && found == target);
    found->Release();
    size = sizeof(found); found = nullptr;
    assert(target->GetPrivateData(kExtra, &found, &size) == S_OK && found == source);
    found->Release();
    assert(target->FreePrivateData(kExtra) == S_OK && texture.refs == 1);
    assert(target->Release() == 0);
    router->RetireDormantForReset();
    assert(cube.refs == 1 && !cube.destroyed); // native source still retains it
    size = sizeof(found); found = nullptr;
    assert(source->GetPrivateData(kExtra, &found, &size) == S_OK && found);
    void* reacquired = nullptr;
    assert(found->QueryInterface(IID_IDirect3DCubeTexture8, &reacquired) == S_OK && reacquired);
    static_cast<IUnknown*>(reacquired)->Release(); found->Release();
    assert(source->FreePrivateData(kExtra) == S_OK);
    assert(source->Release() == 0 && router->Close() && texture.destroyed && cube.destroyed);
    assert(native.Release() == 0 && facade.Release() == 0);
}

void EagerResourceRetirement() {
    {
        Device native, facade;
        Texture raw; raw.device = &native;
        auto router = Router::Create(&native, &facade, 8, {}, false);
        auto* wrapped = router->AdoptTexture(&raw);
        const BYTE bytes[]{1, 2}; DWORD size = sizeof(bytes);
        IUnknown* marker = nullptr;
        bool blob_stored = false, nested_rejected = false;
        raw.on_private_get = [&](REFGUID key, void* output, DWORD* count) {
            if (IsEqualIID(key, kPrivateMarkerGuid)) {
                if (!marker) return D3DERR_NOTFOUND;
                if (count) *count = sizeof(IUnknown*);
                return S_OK;
            }
            if (!blob_stored) return D3DERR_NOTFOUND;
            if (!count) return E_POINTER;
            if (output && *count >= sizeof(bytes)) std::memcpy(output, bytes, sizeof(bytes));
            *count = sizeof(bytes); return S_OK;
        };
        raw.on_private_set = [&](REFGUID key, const void* data, DWORD count, DWORD flags) {
            if (IsEqualIID(key, kPrivateMarkerGuid)) {
                assert(!marker && flags == D3DSPD_IUNKNOWN && count == sizeof(IUnknown*));
                nested_rejected = wrapped->SetPrivateData(kExtra, bytes, sizeof(bytes), 0) == E_NOTIMPL;
                marker = const_cast<IUnknown*>(static_cast<const IUnknown*>(data));
                marker->AddRef(); return S_OK;
            }
            assert(data == bytes && count == sizeof(bytes) && flags == 0);
            blob_stored = true; return S_OK;
        };
        raw.on_release = [&] {
            if (raw.destroyed && marker) { marker->Release(); marker = nullptr; }
        };
        assert(wrapped && wrapped->SetPrivateData(kExtra, bytes, sizeof(bytes), 0) == S_OK);
        assert(marker && nested_rejected);
        assert(wrapped->SetPrivateData(kPrivateMarkerGuid, bytes, sizeof(bytes), 0) == E_NOTIMPL);
        raw.AddRef(); // an independent native binding keeps the resource alive
        assert(wrapped->Release() == 0 && raw.refs == 1 && facade.refs == 1);
        raw.AddRef(); // native getter reference
        auto* reacquired = router->AdoptTexture(&raw);
        assert(reacquired && raw.refs == 2);
        BYTE data[sizeof(bytes)]{};
        assert(reacquired->GetPrivateData(kExtra, data, &size) == S_OK && size == sizeof(bytes) &&
               !std::memcmp(data, bytes, sizeof(bytes)));
        assert(reacquired->Release() == 0 && raw.refs == 1);
        assert(router->Close() && raw.Release() == 0 && raw.destroyed && !marker);
        assert(native.Release() == 0 && facade.Release() == 0);
    }
    {
        Device native, facade;
        Surface raw; raw.device = &native;
        auto router = Router::Create(&native, &facade, 8, {}, false);
        auto* wrapped = router->AdoptSurface(&raw);
        bool reentered = false;
        raw.on_release = [&] { reentered = true; assert(router->Close()); };
        assert(wrapped->Release() == 0 && raw.destroyed && reentered && facade.refs == 1);
        assert(native.Release() == 0 && facade.Release() == 0);
    }
    for (unsigned n = 0; n < 200; ++n) {
        Device native, facade;
        Surface raw; raw.device = &native;
        auto router = Router::Create(&native, &facade, 8, {}, false);
        auto* wrapped = router->AdoptSurface(&raw);
        raw.AddRef(); // native binding remains valid while getter races Release
        std::atomic<bool> go{false};
        std::thread getter([&] {
            while (!go.load()) std::this_thread::yield();
            raw.AddRef();
            auto* found = router->AdoptSurface(&raw);
            assert(found);
            found->Release();
        });
        go = true;
        wrapped->Release();
        getter.join();
        assert(raw.refs == 1 && facade.refs == 1 && router->Close());
        assert(raw.Release() == 0 && native.Release() == 0 && facade.Release() == 0);
    }
}
} // namespace

int main() {
    static_assert(!std::is_abstract_v<Wrapper<IDirect3DSurface8>>);
    static_assert(!std::is_abstract_v<Wrapper<IDirect3DTexture8>>);
    static_assert(!std::is_abstract_v<Wrapper<IDirect3DCubeTexture8>>);
    static_assert(!std::is_abstract_v<Wrapper<IDirect3DVolume8>>);
    static_assert(!std::is_abstract_v<Wrapper<IDirect3DVolumeTexture8>>);
    static_assert(!std::is_abstract_v<Wrapper<IDirect3DSwapChain8>>);
    ResourceGraph();
    FailClosedAndLifetime();
    ResetRetiresDormantAndReconstructsParent();
    ConcurrentReacquisition();
    PrivateVertexTranslation();
    PrivateResourceTranslation();
    EagerResourceRetirement();
}
