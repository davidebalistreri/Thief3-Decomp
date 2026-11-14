// Isolated native/facade D3D8 fixture. Run the two modes in separate
// processes, compare their PPM bytes and lines beginning "parity ".
// Resources are native and their GetDevice leaks the native device; no wrapped
// buffers are passed to that escaped device. This is not a TDS test.
#include "d3d8_device_facade.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <vector>

namespace {
constexpr UINT kWidth = 256, kHeight = 256;
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr D3DCOLOR kClear = 0xff111318;
struct Vertex { float x, y, z, rhw; D3DCOLOR color; };

template<class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

bool Call(const char* name, HRESULT result) {
    std::printf("parity %s=0x%08lx\n", name,
                static_cast<unsigned long>(static_cast<std::uint32_t>(result)));
    return SUCCEEDED(result);
}
bool Identity(IUnknown* object, ComPtr<IUnknown>& output) {
    return object && SUCCEEDED(object->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(output.put()))) && output.value;
}
bool EqualIdentity(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> first, second;
    return Identity(a, first) && Identity(b, second) && first.value == second.value;
}
void PrintMatrix(const char* name, const D3DMATRIX& matrix) {
    std::uint32_t bits[16]{};
    static_assert(sizeof(bits) == sizeof(matrix), "unexpected D3DMATRIX size");
    std::memcpy(bits, &matrix, sizeof(bits));
    std::printf("parity %s=", name);
    for (const auto value : bits) std::printf("%08lx", static_cast<unsigned long>(value));
    std::printf("\n");
}

template<class Resource> bool CheckEscape(const char* label, Resource* resource, IDirect3DDevice8* facade,
                                          IUnknown* native_device_identity) {
    ComPtr<IDirect3DDevice8> escaped;
    const HRESULT hr = resource->GetDevice(escaped.put());
    ComPtr<IUnknown> identity;
    const bool ok = SUCCEEDED(hr) && escaped.value && escaped.value != facade && Identity(escaped.value, identity) &&
                    identity.value == native_device_identity && !EqualIdentity(escaped.value, facade);
    std::printf("resource_%s_native_device_alias=%s hr=0x%08lx\n", label, ok ? "PASS" : "FAIL",
                static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return ok;
}

bool CheckNativeEscape(IDirect3DDevice8* facade, IDirect3D8* native_factory, IDirect3DSurface8* surface,
                       IDirect3DTexture8* texture, IDirect3DVertexBuffer8* vb, IDirect3DIndexBuffer8* ib) {
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    (void)native_factory;
    auto parent_is_facade = [facade](auto* resource) {
        ComPtr<IDirect3DDevice8> parent;
        ComPtr<IUnknown> unknown;
        ComPtr<IDirect3DDevice8> roundtrip;
        return SUCCEEDED(resource->GetDevice(parent.put())) && parent.value == facade &&
               Identity(parent.value, unknown) &&
               SUCCEEDED(unknown->QueryInterface(IID_IDirect3DDevice8, reinterpret_cast<void**>(roundtrip.put()))) &&
               roundtrip.value == facade;
    };
    auto container_is = [](IDirect3DSurface8* item, REFIID iid, IUnknown* expected) {
        ComPtr<IUnknown> container;
        return SUCCEEDED(item->GetContainer(iid, reinterpret_cast<void**>(container.put()))) &&
               container.value && EqualIdentity(container.value, expected);
    };
    ComPtr<IDirect3DSurface8> level, level_again, face, face_again;
    ComPtr<IDirect3DCubeTexture8> cube;
    if (!parent_is_facade(surface) || !parent_is_facade(texture) || !parent_is_facade(vb) || !parent_is_facade(ib) ||
        !container_is(surface, IID_IUnknown, facade) || !container_is(surface, IID_IDirect3DDevice8, facade) ||
        FAILED(texture->GetSurfaceLevel(0, level.put())) || !level.value ||
        FAILED(texture->GetSurfaceLevel(0, level_again.put())) || level_again.value != level.value ||
        !parent_is_facade(level.value) || !container_is(level.value, IID_IUnknown, texture) ||
        !container_is(level.value, IID_IDirect3DTexture8, texture) ||
        FAILED(facade->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, cube.put())) || !cube.value ||
        !parent_is_facade(cube.value) ||
        FAILED(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, face.put())) || !face.value ||
        FAILED(cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, face_again.put())) || face_again.value != face.value ||
        !parent_is_facade(face.value) || !container_is(face.value, IID_IUnknown, cube.value) ||
        !container_is(face.value, IID_IDirect3DCubeTexture8, cube.value)) return false;
    ComPtr<IDirect3DBaseTexture8> bound_cube;
    if (FAILED(facade->SetTexture(0, cube.value)) || FAILED(facade->GetTexture(0, bound_cube.put())) ||
        !bound_cube.value || !EqualIdentity(bound_cube.value, cube.value) || FAILED(facade->SetTexture(0, nullptr))) return false;
    // The facade obtained from a resource must unwrap both buffer types.
    ComPtr<IDirect3DDevice8> parent;
    ComPtr<IDirect3DVertexBuffer8> read_vb;
    ComPtr<IDirect3DIndexBuffer8> read_ib;
    UINT stride = 0, base = 0;
    if (FAILED(level->GetDevice(parent.put())) || parent.value != facade ||
        FAILED(parent->SetStreamSource(0, vb, sizeof(Vertex))) || FAILED(parent->SetIndices(ib, 0)) ||
        FAILED(parent->GetStreamSource(0, read_vb.put(), &stride)) || read_vb.value != vb || stride != sizeof(Vertex) ||
        FAILED(parent->GetIndices(read_ib.put(), &base)) || read_ib.value != ib || base != 0) return false;
    std::printf("resource_facade_2d_cube_surface_roundtrips=PASS\n");
    std::printf("resource_facade_cube_binding=PASS\n");
    std::printf("resource_facade_device_unwraps_buffers=PASS\n");
    std::printf("resource_get_device_boundary=facade_for_tested_types; other_types=unverified\n");
    std::printf("buffer_parent_facade_roundtrip=PASS\n");
    return true;
#else
    ComPtr<IDirect3DDevice8> escaped;
    if (FAILED(surface->GetDevice(escaped.put())) || !escaped.value || escaped.value == facade) return false;
    ComPtr<IUnknown> native_identity;
    if (!Identity(escaped.value, native_identity) || EqualIdentity(escaped.value, facade)) return false;
    ComPtr<IDirect3D8> escaped_factory;
    if (FAILED(escaped->GetDirect3D(escaped_factory.put())) || !escaped_factory.value ||
        !EqualIdentity(escaped_factory.value, native_factory)) return false;
    if (!CheckEscape("surface", surface, facade, native_identity.value) ||
        !CheckEscape("texture", texture, facade, native_identity.value)) return false;
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    for (IDirect3DResource8* resource : {static_cast<IDirect3DResource8*>(vb), static_cast<IDirect3DResource8*>(ib)}) {
        ComPtr<IDirect3DDevice8> parent;
        ComPtr<IUnknown> unknown;
        ComPtr<IDirect3DDevice8> roundtrip;
        if (FAILED(resource->GetDevice(parent.put())) || parent.value != facade ||
            !Identity(parent.value, unknown) ||
            FAILED(unknown->QueryInterface(IID_IDirect3DDevice8, reinterpret_cast<void**>(roundtrip.put()))) ||
            roundtrip.value != facade) return false;
    }
    std::printf("buffer_parent_facade_roundtrip=PASS\n");
    std::printf("resource_get_device_boundary=native_nonbuffer; wrapped_buffers=facade\n");
#else
    if (
        !CheckEscape("vertex_buffer", vb, facade, native_identity.value) ||
        !CheckEscape("index_buffer", ib, facade, native_identity.value)) return false;
    std::printf("resource_get_device_boundary=native_device; wrapped_buffers=not_tested\n");
#endif
    return true;
#endif
}

#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
bool ReadFixtureMemory(void*, const void* source, void* destination, std::size_t size) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), source, destination, size, &copied) && copied == size;
}

#ifndef T3RENDER_RESOURCE_FACADE_FIXTURE
bool CheckRetainedBuffer(IDirect3DDevice8* device, IDirect3DDevice8* native,
                         const std::shared_ptr<t3render::buffers::BufferProxy>& buffers) {
    ComPtr<IDirect3DVertexBuffer8> held;
    if (FAILED(device->CreateVertexBuffer(sizeof(Vertex) * 3, D3DUSAGE_WRITEONLY, kFvf,
                                           D3DPOOL_MANAGED, held.put())) || !held.value ||
        FAILED(device->SetStreamSource(0, held.value, sizeof(Vertex)))) return false;
    DWORD token = 0;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &token))) return false;
    {
        auto pin = buffers->UnwrapVertex(native, held.value);
        BYTE byte = 0;
        if (!pin.wrapped() || buffers->ReadVertex(native, pin.native(), 0, &byte, 1) !=
                              t3render::ir::RegistryResult::Unavailable) return false;
    }
    ComPtr<IDirect3DVertexBuffer8> existing;
    UINT stride = 0;
    if (FAILED(device->GetStreamSource(0, existing.put(), &stride)) || existing.value != held.value ||
        stride != sizeof(Vertex)) return false;
    existing->Release(); existing.value = nullptr;
    // Remove the active device binding so only the state block can retain it.
    if (FAILED(device->SetStreamSource(0, nullptr, 0))) return false;
    IDirect3DVertexBuffer8* const original_identity = held.value;
    held->Release(); held.value = nullptr;
    if (FAILED(device->ApplyStateBlock(token))) return false;
    ComPtr<IDirect3DVertexBuffer8> reacquired;
    const HRESULT hr = device->GetStreamSource(0, reacquired.put(), &stride);
    const bool same = SUCCEEDED(hr) && reacquired.value == original_identity && stride == sizeof(Vertex);
    if (same) {
        auto pin = buffers->UnwrapVertex(native, reacquired.value);
        BYTE byte = 0;
        if (!pin.wrapped() || buffers->ReadVertex(native, pin.native(), 0, &byte, 1) !=
                              t3render::ir::RegistryResult::Unavailable) return false;
    }
    const bool cleaned = SUCCEEDED(device->DeleteStateBlock(token)) &&
                         SUCCEEDED(device->SetStreamSource(0, nullptr, 0));
    if (!same || !cleaned) return false;
    std::printf("buffer_stateblock_retained_identity=PASS\n");
    return true;
}
#else
bool CheckRetainedBuffer(IDirect3DDevice8* device, IDirect3DDevice8* native,
                         const std::shared_ptr<t3render::buffers::BufferProxy>& buffers) {
    const Vertex vertices[] = {{12, 12, 0.5f, 1, 0xffe43b3b}, {116, 12, 0.5f, 1, 0xffe43b3b},
                               {12, 116, 0.5f, 1, 0xffe43b3b}};
    const WORD indices[] = {0, 1, 2};
    ComPtr<IDirect3DVertexBuffer8> vertex;
    ComPtr<IDirect3DIndexBuffer8> index;
    if (FAILED(device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, vertex.put())) ||
        !vertex.value || FAILED(device->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
        D3DPOOL_MANAGED, index.put())) || !index.value) return false;
    BYTE* mapped = nullptr;
    if (FAILED(vertex->Lock(0, sizeof(vertices), &mapped, 0)) || !mapped) return false;
    std::memcpy(mapped, vertices, sizeof(vertices));
    if (FAILED(vertex->Unlock())) return false;
    mapped = nullptr;
    if (FAILED(index->Lock(0, sizeof(indices), &mapped, 0)) || !mapped) return false;
    std::memcpy(mapped, indices, sizeof(indices));
    if (FAILED(index->Unlock()) || FAILED(device->SetStreamSource(0, vertex.value, sizeof(Vertex))) ||
        FAILED(device->SetIndices(index.value, 0))) return false;

    DWORD saved = 0, recorded = 0;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || FAILED(device->CaptureStateBlock(saved)) ||
        FAILED(device->BeginStateBlock()) || FAILED(device->SetStreamSource(0, vertex.value, sizeof(Vertex))) ||
        FAILED(device->SetIndices(index.value, 0)) || FAILED(device->EndStateBlock(&recorded)) ||
        FAILED(device->ApplyStateBlock(recorded)) || FAILED(device->DeleteStateBlock(recorded))) return false;

    IDirect3DVertexBuffer8* vertex_identity = vertex.value;
    IDirect3DIndexBuffer8* index_identity = index.value;
    if (FAILED(device->SetStreamSource(0, nullptr, 0)) || FAILED(device->SetIndices(nullptr, 0))) return false;
    vertex->Release(); vertex.value = nullptr;
    index->Release(); index.value = nullptr;
    if (FAILED(device->ApplyStateBlock(saved))) return false;
    UINT stride = 0, base = 0;
    if (FAILED(device->GetStreamSource(0, vertex.put(), &stride)) || vertex.value != vertex_identity || stride != sizeof(Vertex) ||
        FAILED(device->GetIndices(index.put(), &base)) || index.value != index_identity || base != 0) return false;

    if (FAILED(device->SetVertexShader(kFvf)) || FAILED(device->SetPixelShader(0)) ||
        FAILED(device->SetTexture(0, nullptr)) || FAILED(device->SetRenderState(D3DRS_LIGHTING, FALSE)) ||
        FAILED(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE))) return false;
    auto vertex_pin = buffers->UnwrapVertex(native, vertex.value);
    auto index_pin = buffers->UnwrapIndex(native, index.value);
    Vertex vertex_copy[3]{}; WORD index_copy[3]{};
    const auto draw = buffers->BeginDraw(native);
    if (!vertex_pin.wrapped() || !index_pin.wrapped() || !draw ||
        buffers->ReadVertex(native, vertex_pin.native(), 0, vertex_copy, sizeof(vertex_copy)) != t3render::ir::RegistryResult::Ok ||
        buffers->ReadIndex(native, index_pin.native(), 0, index_copy, sizeof(index_copy)) != t3render::ir::RegistryResult::Ok ||
        std::memcmp(vertex_copy, vertices, sizeof(vertices)) || std::memcmp(index_copy, indices, sizeof(indices))) return false;
    const bool drawn = SUCCEEDED(device->BeginScene()) &&
        SUCCEEDED(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 3, 0, 1)) && SUCCEEDED(device->EndScene());
    if (!buffers->EndDraw(native, draw) || !drawn || FAILED(device->DeleteStateBlock(saved)) ||
        FAILED(device->SetStreamSource(0, nullptr, 0)) || FAILED(device->SetIndices(nullptr, 0))) return false;
    std::memset(vertex_copy, 0, sizeof(vertex_copy));
    std::memset(index_copy, 0, sizeof(index_copy));
    if (buffers->ReadVertex(native, vertex_pin.native(), 0, vertex_copy, sizeof(vertex_copy)) != t3render::ir::RegistryResult::Ok ||
        buffers->ReadIndex(native, index_pin.native(), 0, index_copy, sizeof(index_copy)) != t3render::ir::RegistryResult::Ok ||
        std::memcmp(vertex_copy, vertices, sizeof(vertices)) || std::memcmp(index_copy, indices, sizeof(indices))) return false;
    std::printf("buffer_stateblock_retained_identity=PASS\n");
    std::printf("resource_facade_stateblock_buffer_shadows=PASS\n");
    return true;
}

bool CheckFailedStateBlock(IDirect3DDevice8* device, IDirect3DDevice8* native,
                           const std::shared_ptr<t3render::buffers::BufferProxy>& buffers) {
    ComPtr<IDirect3DVertexBuffer8> vertex;
    if (FAILED(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, vertex.put())) ||
        !vertex.value) return false;
    BYTE* mapped = nullptr;
    if (FAILED(vertex->Lock(0, sizeof(Vertex), &mapped, 0)) || !mapped) return false;
    std::memset(mapped, 0x4d, sizeof(Vertex));
    if (FAILED(vertex->Unlock())) return false;
    auto pin = buffers->UnwrapVertex(native, vertex.value);
    BYTE observed = 0;
    if (!pin.wrapped() || buffers->ReadVertex(native, pin.native(), 0, &observed, 1) !=
        t3render::ir::RegistryResult::Ok || observed != 0x4d) return false;
    const HRESULT hr = device->DeleteStateBlock(0xffffffffu);
    if (SUCCEEDED(hr) || buffers->ReadVertex(native, pin.native(), 0, &observed, 1) !=
        t3render::ir::RegistryResult::Unavailable) return false;
    std::printf("resource_facade_failed_stateblock_invalidates=PASS\n");
    return true;
}
#endif
#endif

#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
// Characterize private-data semantics separately from image parity. The
// facade accepts observed opaque bytes and separately bridged buffer cases;
// unknown/external COM provenance is still rejected.
class PrivateUnknown final : public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown)) return E_NOINTERFACE;
        AddRef(); *out = this; return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs;
        if (!remaining) delete this;
        return remaining;
    }
    ULONG refs = 1;
};

class PrivateDeviceHolder final : public IUnknown {
public:
    PrivateDeviceHolder(IDirect3DDevice8* device, std::shared_ptr<std::atomic<bool>> destroyed)
        : device_(device), destroyed_(std::move(destroyed)) {
        device_->AddRef();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown)) return E_NOINTERFACE;
        AddRef(); *out = this; return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs_;
        if (!remaining) {
            IDirect3DDevice8* device = device_;
            destroyed_->store(true);
            delete this;
            device->Release();
        }
        return remaining;
    }
private:
    IDirect3DDevice8* device_;
    std::shared_ptr<std::atomic<bool>> destroyed_;
    ULONG refs_ = 1;
};

template<class Resource>
bool ObservePrivateData(const char* kind, Resource* resource, bool facade_mode) {
    const GUID blob_guid{0x3ee22a91, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    const GUID object_guid{0x3ee22a92, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    const BYTE bytes[]{0x17, 0x63, 0xa2, 0x04};
    BYTE output[sizeof(bytes)]{};
    DWORD missing_size = sizeof(output), query_size = 0, small_size = 1, output_size = sizeof(output);
    const HRESULT missing = resource->GetPrivateData(blob_guid, output, &missing_size);
    const HRESULT set = resource->SetPrivateData(blob_guid, bytes, sizeof(bytes), 0);
    const HRESULT query = resource->GetPrivateData(blob_guid, nullptr, &query_size);
    const HRESULT small = resource->GetPrivateData(blob_guid, output, &small_size);
    const HRESULT get = resource->GetPrivateData(blob_guid, output, &output_size);
    const bool matches = SUCCEEDED(get) && output_size == sizeof(bytes) && !std::memcmp(output, bytes, sizeof(bytes));
    const HRESULT freed = resource->FreePrivateData(blob_guid);
    DWORD after_free_size = sizeof(output);
    const HRESULT after_free = resource->GetPrivateData(blob_guid, output, &after_free_size);

    auto* unknown = new (std::nothrow) PrivateUnknown;
    if (!unknown) return false;
    const HRESULT object_set = resource->SetPrivateData(object_guid, unknown, sizeof(IUnknown*), D3DSPD_IUNKNOWN);
    const ULONG refs_after_set = unknown->refs;
    IUnknown* returned = nullptr;
    DWORD object_size = sizeof(returned);
    const HRESULT object_get = resource->GetPrivateData(object_guid, &returned, &object_size);
    const ULONG refs_after_get = unknown->refs;
    const bool same = SUCCEEDED(object_get) && returned == unknown && object_size == sizeof(returned);
    const HRESULT object_free = resource->FreePrivateData(object_guid);
    const ULONG refs_after_free = unknown->refs;
    // Keep our own reference even if a nonconforming runtime failed to AddRef.
    if (SUCCEEDED(object_get) && returned && (returned != unknown || refs_after_free > 1)) returned->Release();
    const ULONG refs_after_returned_release = unknown->refs;
    unknown->Release();

    const auto code = [](HRESULT hr) { return static_cast<unsigned long>(static_cast<std::uint32_t>(hr)); };
    std::printf("private_data kind=%s missing=0x%08lx set=0x%08lx query=0x%08lx query_size=%lu "
                "small=0x%08lx small_size=%lu get=0x%08lx match=%u free=0x%08lx after_free=0x%08lx "
                "object_set=0x%08lx object_get=0x%08lx object_same=%u object_free=0x%08lx refs=%lu,%lu,%lu,%lu\n",
                kind, code(missing), code(set), code(query), static_cast<unsigned long>(query_size), code(small),
                static_cast<unsigned long>(small_size), code(get), matches ? 1u : 0u, code(freed), code(after_free), code(object_set),
                code(object_get), same ? 1u : 0u, code(object_free), static_cast<unsigned long>(refs_after_set),
                static_cast<unsigned long>(refs_after_get), static_cast<unsigned long>(refs_after_free),
                static_cast<unsigned long>(refs_after_returned_release));
    if (facade_mode) return missing == E_NOTIMPL && SUCCEEDED(set) && SUCCEEDED(query) && FAILED(small) &&
                            query_size == sizeof(bytes) && small_size == sizeof(bytes) && matches && SUCCEEDED(freed) && FAILED(after_free) &&
                            object_set == E_NOTIMPL && object_get == E_NOTIMPL && object_free == E_NOTIMPL &&
                            refs_after_set == 1 && refs_after_get == 1 && refs_after_free == 1 && refs_after_returned_release == 1;
    return SUCCEEDED(set) && matches && SUCCEEDED(freed) && FAILED(after_free) && SUCCEEDED(object_set) && same && SUCCEEDED(object_free) &&
           refs_after_set == 2 && refs_after_get == 3 && refs_after_free == 2 && refs_after_returned_release == 1;
}

bool ObservePrivateDataResources(IDirect3DDevice8* device, IDirect3DVertexBuffer8* vb, IDirect3DIndexBuffer8* ib,
                                 IDirect3DTexture8* texture, IDirect3DSurface8* back, bool facade_mode) {
    ComPtr<IDirect3DCubeTexture8> cube;
    ComPtr<IDirect3DVolumeTexture8> volume_texture;
    ComPtr<IDirect3DVolume8> volume;
    ComPtr<IDirect3DSurface8> level;
    if (FAILED(device->CreateCubeTexture(2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, cube.put())) || !cube.value ||
        FAILED(device->CreateVolumeTexture(2, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, volume_texture.put())) ||
        !volume_texture.value || FAILED(volume_texture->GetVolumeLevel(0, volume.put())) || !volume.value ||
        FAILED(texture->GetSurfaceLevel(0, level.put())) || !level.value) return false;
    if (!ObservePrivateData("vertex", vb, facade_mode) || !ObservePrivateData("index", ib, facade_mode) ||
        !ObservePrivateData("texture", texture, facade_mode) || !ObservePrivateData("backbuffer", back, facade_mode) ||
        !ObservePrivateData("surface_level", level.value, facade_mode) || !ObservePrivateData("cube", cube.value, facade_mode) ||
        !ObservePrivateData("volume_texture", volume_texture.value, facade_mode) ||
        !ObservePrivateData("volume", volume.value, facade_mode)) return false;

    // An application can store a resource interface, not just its own object.
    // Its identity and GetDevice must never become a path to the native device.
    const GUID alias_guid{0x3ee22a93, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    const HRESULT set = texture->SetPrivateData(alias_guid, static_cast<IUnknown*>(vb), sizeof(IUnknown*), D3DSPD_IUNKNOWN);
    ComPtr<IUnknown> value;
    DWORD size = sizeof(value.value);
    const HRESULT get = texture->GetPrivateData(alias_guid, value.put(), &size);
    ComPtr<IDirect3DVertexBuffer8> returned_buffer;
    ComPtr<IDirect3DDevice8> returned_device;
    const bool same = SUCCEEDED(get) && value.value && size == sizeof(value.value) && EqualIdentity(value.value, vb);
    const bool parent = same && SUCCEEDED(value->QueryInterface(IID_IDirect3DVertexBuffer8,
        reinterpret_cast<void**>(returned_buffer.put()))) && returned_buffer.value &&
        SUCCEEDED(returned_buffer->GetDevice(returned_device.put())) && returned_device.value &&
        EqualIdentity(returned_device.value, device);
    // Reuse the same GUID for an index buffer, retaining the first returned
    // vertex interface while the native private reference is replaced.
    const HRESULT index_set = texture->SetPrivateData(alias_guid, static_cast<IUnknown*>(ib), sizeof(IUnknown*), D3DSPD_IUNKNOWN);
    ComPtr<IUnknown> index_value;
    DWORD index_size = sizeof(index_value.value);
    const HRESULT index_get = texture->GetPrivateData(alias_guid, index_value.put(), &index_size);
    ComPtr<IDirect3DIndexBuffer8> returned_index;
    ComPtr<IDirect3DDevice8> index_device;
    const bool index_same = SUCCEEDED(index_get) && index_value.value && index_size == sizeof(index_value.value) &&
                            EqualIdentity(index_value.value, ib);
    const bool index_parent = index_same && SUCCEEDED(index_value->QueryInterface(IID_IDirect3DIndexBuffer8,
        reinterpret_cast<void**>(returned_index.put()))) && returned_index.value &&
        SUCCEEDED(returned_index->GetDevice(index_device.put())) && EqualIdentity(index_device.value, device);
    const HRESULT freed = texture->FreePrivateData(alias_guid);
    const auto code = [](HRESULT hr) { return static_cast<unsigned long>(static_cast<std::uint32_t>(hr)); };
    std::printf("private_data_alias set=0x%08lx get=0x%08lx same=%u parent=%u free=0x%08lx\n",
                code(set), code(get), same ? 1u : 0u, parent ? 1u : 0u, code(freed));
    std::printf("private_data_index_alias set=0x%08lx get=0x%08lx same=%u parent=%u\n",
                code(index_set), code(index_get), index_same ? 1u : 0u, index_parent ? 1u : 0u);
    if (FAILED(set) || FAILED(get) || FAILED(freed) || !same || !parent ||
        FAILED(index_set) || FAILED(index_get) || !index_same || !index_parent) return false;

    const GUID resource_key{0x3ee22a98, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    struct Target { IUnknown* value; const IID* iid; };
    const Target targets[] = {{texture, &IID_IDirect3DTexture8}, {cube.value, &IID_IDirect3DCubeTexture8},
        {volume_texture.value, &IID_IDirect3DVolumeTexture8}, {volume.value, &IID_IDirect3DVolume8},
        {level.value, &IID_IDirect3DSurface8}, {back, &IID_IDirect3DSurface8}};
    for (const auto& target : targets) {
        if (FAILED(texture->SetPrivateData(resource_key, target.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) return false;
        ComPtr<IUnknown> found, typed;
        DWORD found_size = sizeof(found.value);
        if (FAILED(texture->GetPrivateData(resource_key, found.put(), &found_size)) || !found.value ||
            found_size != sizeof(found.value) || !EqualIdentity(found.value, target.value) ||
            FAILED(found->QueryInterface(*target.iid, reinterpret_cast<void**>(typed.put()))) ||
            !EqualIdentity(typed.value, target.value) || FAILED(texture->FreePrivateData(resource_key))) return false;
    }
    // The application-created native texture/cube cycle is deliberately
    // broken while both endpoints are still live, just as for native D3D8.
    if (FAILED(texture->SetPrivateData(resource_key, cube.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN)) ||
        FAILED(cube->SetPrivateData(resource_key, texture, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) return false;
    ComPtr<IUnknown> cube_value, texture_value;
    DWORD cube_size = sizeof(cube_value.value), texture_size = sizeof(texture_value.value);
    if (FAILED(texture->GetPrivateData(resource_key, cube_value.put(), &cube_size)) ||
        FAILED(cube->GetPrivateData(resource_key, texture_value.put(), &texture_size)) ||
        !EqualIdentity(cube_value.value, cube.value) || !EqualIdentity(texture_value.value, texture) ||
        FAILED(texture->FreePrivateData(resource_key)) || FAILED(cube->FreePrivateData(resource_key))) return false;
    std::printf("private_resource_self_and_cross_aliases=PASS\n");

    const Target buffer_targets[] = {{vb, &IID_IDirect3DVertexBuffer8}, {ib, &IID_IDirect3DIndexBuffer8},
        {texture, &IID_IDirect3DTexture8}, {cube.value, &IID_IDirect3DCubeTexture8},
        {volume_texture.value, &IID_IDirect3DVolumeTexture8}, {volume.value, &IID_IDirect3DVolume8},
        {level.value, &IID_IDirect3DSurface8}};
    IDirect3DResource8* sources[] = {vb, ib};
    for (auto* source : sources) for (const auto& target : buffer_targets) {
        if (FAILED(source->SetPrivateData(resource_key, target.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) return false;
        ComPtr<IUnknown> found, typed;
        DWORD found_size = sizeof(found.value);
        if (FAILED(source->GetPrivateData(resource_key, found.put(), &found_size)) || !found.value ||
            found_size != sizeof(found.value) || !EqualIdentity(found.value, target.value) ||
            FAILED(found->QueryInterface(*target.iid, reinterpret_cast<void**>(typed.put()))) ||
            !EqualIdentity(typed.value, target.value) || FAILED(source->FreePrivateData(resource_key))) return false;
    }
    if (FAILED(vb->SetPrivateData(resource_key, texture, sizeof(IUnknown*), D3DSPD_IUNKNOWN)) ||
        FAILED(texture->SetPrivateData(resource_key, vb, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) return false;
    ComPtr<IUnknown> vb_target, texture_target;
    DWORD vb_size = sizeof(vb_target.value), target_size = sizeof(texture_target.value);
    if (FAILED(texture->GetPrivateData(resource_key, vb_target.put(), &vb_size)) ||
        FAILED(vb->GetPrivateData(resource_key, texture_target.put(), &target_size)) ||
        !EqualIdentity(vb_target.value, vb) || !EqualIdentity(texture_target.value, texture) ||
        FAILED(texture->FreePrivateData(resource_key)) || FAILED(vb->FreePrivateData(resource_key))) return false;
    std::printf("private_buffer_sources=PASS\n");
    return true;
}

bool ObserveTranslatedPrivateLifetime(HWND window, bool facade_mode) {
    ComPtr<IDirect3D8> native_factory;
    native_factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!native_factory.value) return false;
    D3DDISPLAYMODE display{};
    if (FAILED(native_factory->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display))) return false;
    ComPtr<IDirect3D8> facade_factory;
    auto buffers = t3render::buffers::BufferProxy::Create(ReadFixtureMemory, nullptr);
    if (!buffers) return false;
    IDirect3D8* selected = native_factory.value;
    if (facade_mode) {
        native_factory->AddRef();
        facade_factory.value = t3render::fixture::CreateBufferDeviceFacadeFactory(native_factory.value, buffers);
        if (!facade_factory.value) { native_factory->Release(); return false; }
        selected = facade_factory.value;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window; pp.Windowed = TRUE;
    ComPtr<IDirect3DDevice8> device;
    if (FAILED(selected->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put())) || !device.value) return false;
    ComPtr<IDirect3DTexture8> texture;
    ComPtr<IDirect3DVertexBuffer8> vertex;
    ComPtr<IDirect3DVertexBuffer8> source_vertex;
    ComPtr<IDirect3DIndexBuffer8> index;
    ComPtr<IDirect3DCubeTexture8> cube;
    if (FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.put())) || !texture.value ||
        FAILED(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, vertex.put())) ||
        FAILED(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, source_vertex.put())) ||
        !vertex.value || FAILED(device->CreateIndexBuffer(3 * sizeof(WORD), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                                          D3DPOOL_MANAGED, index.put())) || !index.value ||
        FAILED(device->CreateCubeTexture(2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, cube.put())) || !cube.value) return false;
    const GUID key{0x3ee22a96, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    const GUID index_key{0x3ee22a97, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    const GUID cube_key{0x3ee22a99, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    const GUID source_key{0x3ee22a9a, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    if (FAILED(texture->SetPrivateData(key, vertex.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN)) ||
        FAILED(texture->SetPrivateData(index_key, index.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN)) ||
        FAILED(texture->SetPrivateData(cube_key, cube.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN)) ||
        !source_vertex.value || FAILED(source_vertex->SetPrivateData(source_key, cube.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) return false;
    DWORD query_size = 0;
    if (FAILED(texture->GetPrivateData(key, nullptr, &query_size)) || query_size != sizeof(IUnknown*)) return false;
    IUnknown* untouched = reinterpret_cast<IUnknown*>(1);
    DWORD small_size = 1;
    if (SUCCEEDED(texture->GetPrivateData(key, &untouched, &small_size)) ||
        untouched != reinterpret_cast<IUnknown*>(1) || small_size != sizeof(IUnknown*)) return false;
    IUnknown* const original = vertex.value; // compared only while its dormant facade is retained
    const ULONG remaining = vertex->Release();
    vertex.value = nullptr;
    IUnknown* const original_index = index.value;
    const ULONG index_remaining = index->Release();
    index.value = nullptr;
    IUnknown* const original_cube = cube.value;
    const ULONG cube_remaining = cube->Release();
    cube.value = nullptr;
    if (facade_mode && (remaining != 0 || index_remaining != 0 || cube_remaining != 0)) return false;
    ComPtr<IUnknown> cube_value;
    ComPtr<IDirect3DCubeTexture8> recovered_cube;
    ComPtr<IDirect3DDevice8> cube_parent;
    DWORD cube_size = sizeof(cube_value.value);
    if (FAILED(texture->GetPrivateData(cube_key, cube_value.put(), &cube_size)) || !cube_value.value ||
        cube_size != sizeof(cube_value.value) || (facade_mode && cube_value.value != original_cube) ||
        FAILED(cube_value->QueryInterface(IID_IDirect3DCubeTexture8, reinterpret_cast<void**>(recovered_cube.put()))) ||
        !recovered_cube.value || FAILED(recovered_cube->GetDevice(cube_parent.put())) ||
        !EqualIdentity(cube_parent.value, device.value)) return false;
    cube_parent->Release(); cube_parent.value = nullptr;
    recovered_cube->Release(); recovered_cube.value = nullptr;
    cube_value->Release(); cube_value.value = nullptr;
    ComPtr<IUnknown> index_value;
    ComPtr<IDirect3DIndexBuffer8> recovered_index;
    ComPtr<IDirect3DDevice8> index_parent;
    DWORD index_size = sizeof(index_value.value);
    if (FAILED(texture->GetPrivateData(index_key, index_value.put(), &index_size)) || !index_value.value ||
        index_size != sizeof(index_value.value) || (facade_mode && index_value.value != original_index) ||
        FAILED(index_value->QueryInterface(IID_IDirect3DIndexBuffer8, reinterpret_cast<void**>(recovered_index.put()))) ||
        !recovered_index.value || FAILED(recovered_index->GetDevice(index_parent.put())) ||
        !EqualIdentity(index_parent.value, device.value)) return false;
    index_parent->Release(); index_parent.value = nullptr;
    recovered_index->Release(); recovered_index.value = nullptr;
    index_value->Release(); index_value.value = nullptr;
    ComPtr<IUnknown> value;
    DWORD size = sizeof(value.value);
    if (FAILED(texture->GetPrivateData(key, value.put(), &size)) || !value.value || size != sizeof(value.value) ||
        (facade_mode && value.value != original)) return false;
    ComPtr<IDirect3DVertexBuffer8> recovered;
    ComPtr<IDirect3DDevice8> parent;
    if (FAILED(value->QueryInterface(IID_IDirect3DVertexBuffer8, reinterpret_cast<void**>(recovered.put()))) ||
        !recovered.value || FAILED(recovered->GetDevice(parent.put())) || !EqualIdentity(parent.value, device.value)) return false;
    parent->Release(); parent.value = nullptr;
    recovered->Release(); recovered.value = nullptr;
    value->Release(); value.value = nullptr;
    // Reset retires the dormant vertex facade; the live managed texture's
    // private data must reacquire it without exposing the native interface.
    if (FAILED(device->Reset(&pp))) return false;
    cube_size = sizeof(cube_value.value);
    if (FAILED(texture->GetPrivateData(cube_key, cube_value.put(), &cube_size)) || !cube_value.value ||
        cube_size != sizeof(cube_value.value) ||
        FAILED(cube_value->QueryInterface(IID_IDirect3DCubeTexture8, reinterpret_cast<void**>(recovered_cube.put()))) ||
        !recovered_cube.value || FAILED(recovered_cube->GetDevice(cube_parent.put())) ||
        !EqualIdentity(cube_parent.value, device.value)) return false;
    ComPtr<IUnknown> buffer_cube;
    DWORD buffer_cube_size = sizeof(buffer_cube.value);
    if (FAILED(source_vertex->GetPrivateData(source_key, buffer_cube.put(), &buffer_cube_size)) ||
        !buffer_cube.value || buffer_cube_size != sizeof(buffer_cube.value) ||
        !EqualIdentity(buffer_cube.value, recovered_cube.value)) return false;
    buffer_cube->Release(); buffer_cube.value = nullptr;
    cube_parent->Release(); cube_parent.value = nullptr;
    recovered_cube->Release(); recovered_cube.value = nullptr;
    cube_value->Release(); cube_value.value = nullptr;
    index_size = sizeof(index_value.value);
    if (FAILED(texture->GetPrivateData(index_key, index_value.put(), &index_size)) || !index_value.value ||
        index_size != sizeof(index_value.value) ||
        FAILED(index_value->QueryInterface(IID_IDirect3DIndexBuffer8, reinterpret_cast<void**>(recovered_index.put()))) ||
        !recovered_index.value || FAILED(recovered_index->GetDevice(index_parent.put())) ||
        !EqualIdentity(index_parent.value, device.value)) return false;
    index_parent->Release(); index_parent.value = nullptr;
    recovered_index->Release(); recovered_index.value = nullptr;
    index_value->Release(); index_value.value = nullptr;
    size = sizeof(value.value);
    if (FAILED(texture->GetPrivateData(key, value.put(), &size)) || !value.value || size != sizeof(value.value) ||
        FAILED(value->QueryInterface(IID_IDirect3DVertexBuffer8, reinterpret_cast<void**>(recovered.put()))) ||
        !recovered.value || FAILED(recovered->GetDevice(parent.put())) || !EqualIdentity(parent.value, device.value)) return false;
    const BYTE blob[]{0x24, 0x68, 0xac};
    BYTE read_blob[sizeof(blob)]{};
    DWORD blob_size = sizeof(read_blob);
    if (FAILED(texture->SetPrivateData(key, blob, sizeof(blob), 0)) ||
        FAILED(texture->GetPrivateData(key, read_blob, &blob_size)) || blob_size != sizeof(blob) ||
        std::memcmp(blob, read_blob, sizeof(blob)) ||
        FAILED(texture->SetPrivateData(key, recovered.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) return false;
    ComPtr<IUnknown> restored;
    size = sizeof(restored.value);
    if (FAILED(texture->GetPrivateData(key, restored.put(), &size)) || !restored.value ||
        !EqualIdentity(restored.value, recovered.value)) return false;
    restored->Release(); restored.value = nullptr;
    parent->Release(); parent.value = nullptr;
    recovered->Release(); recovered.value = nullptr;
    value->Release(); value.value = nullptr;
    source_vertex->Release(); source_vertex.value = nullptr;
    std::printf("private_buffer_sources_reset=PASS\n");
    const unsigned resources_closed = t3render::fixture::resource_facade_close_successes.load();
    const unsigned buffers_closed = t3render::fixture::buffer_facade_close_successes.load();
    const ULONG texture_remaining = texture->Release(); texture.value = nullptr;
    const ULONG device_remaining = device->Release(); device.value = nullptr;
    if (facade_mode && (texture_remaining != 0 || device_remaining != 0 ||
        t3render::fixture::resource_facade_close_successes != resources_closed + 1 ||
        t3render::fixture::buffer_facade_close_successes != buffers_closed + 1)) return false;
    std::printf("private_translated_lifetime=PASS\n");
    return true;
}

bool ObservePrivateResourceLifetime(HWND window, bool facade_mode) {
    ComPtr<IDirect3D8> native_factory;
    native_factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!native_factory.value) return false;
    D3DDISPLAYMODE display{};
    if (FAILED(native_factory->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display))) return false;
    ComPtr<IDirect3D8> facade_factory;
    auto buffers = t3render::buffers::BufferProxy::Create(ReadFixtureMemory, nullptr);
    if (!buffers) return false;
    IDirect3D8* selected = native_factory.value;
    if (facade_mode) {
        native_factory->AddRef();
        facade_factory.value = t3render::fixture::CreateBufferDeviceFacadeFactory(native_factory.value, buffers);
        if (!facade_factory.value) { native_factory->Release(); return false; }
        selected = facade_factory.value;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window; pp.Windowed = TRUE;
    ComPtr<IDirect3DDevice8> device;
    if (FAILED(selected->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put())) || !device.value) return false;
    ComPtr<IDirect3DTexture8> texture;
    ComPtr<IDirect3DVertexBuffer8> vertex;
    if (FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.put())) || !texture.value ||
        FAILED(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, vertex.put())) ||
        !vertex.value) return false;
    const GUID alias_guid{0x3ee22a94, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    if (!facade_mode) {
        const GUID witness_guid{0x3ee22a95, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
        auto* witness = new (std::nothrow) PrivateUnknown;
        if (!witness) return false;
        const bool stored = SUCCEEDED(vertex->SetPrivateData(witness_guid, witness, sizeof(IUnknown*), D3DSPD_IUNKNOWN)) &&
                            witness->refs == 2 &&
                            SUCCEEDED(texture->SetPrivateData(alias_guid, static_cast<IUnknown*>(vertex.value),
                                                               sizeof(IUnknown*), D3DSPD_IUNKNOWN));
        if (!stored) { witness->Release(); return false; }
        vertex->Release(); vertex.value = nullptr;
        texture->Release(); texture.value = nullptr;
        const bool released = witness->refs == 1;
        witness->Release();
        if (!released) return false;
        std::printf("private_lifetime_native_dependent_release=PASS\n");
        return true;
    }

    // Diagnostic only: deliberately simulate naive IUnknown forwarding through
    // the borrowed native texture, never expose this path to an application.
    if (FAILED(device->SetTexture(0, texture.value))) return false;
    auto* native = static_cast<t3render::fixture::DeviceFixture*>(device.value)->NativeForFixture();
    ComPtr<IDirect3DBaseTexture8> native_texture;
    if (!native || FAILED(native->GetTexture(0, native_texture.put())) || !native_texture.value ||
        FAILED(device->SetTexture(0, nullptr)) ||
        FAILED(native_texture->SetPrivateData(alias_guid, static_cast<IUnknown*>(vertex.value),
                                              sizeof(IUnknown*), D3DSPD_IUNKNOWN))) return false;
    const ULONG vertex_remaining = vertex->Release();
    vertex.value = nullptr;
    if (vertex_remaining != 1) return false;
    const ULONG texture_remaining = texture->Release();
    texture.value = nullptr;
    if (texture_remaining != 0) return false;
    // Drop the diagnostic reference. The dormant facade wrapper retains the
    // native texture, and the texture's private data retains the vertex facade.
    auto* borrowed = native_texture.value;
    native_texture->Release(); native_texture.value = nullptr;
    const ULONG device_remaining = device->Release();
    device.value = nullptr;
    if (device_remaining != 1) return false;
    const unsigned resource_closes = t3render::fixture::resource_facade_close_successes.load();
    const unsigned buffer_closes = t3render::fixture::buffer_facade_close_successes.load();
    borrowed->AddRef(); // keep the native texture alive while breaking the cycle
    const HRESULT freed = borrowed->FreePrivateData(alias_guid);
    borrowed->Release();
    if (FAILED(freed) || t3render::fixture::resource_facade_close_successes != resource_closes + 1 ||
        t3render::fixture::buffer_facade_close_successes != buffer_closes + 1) return false;
    std::printf("private_lifetime_naive_cycle_and_cleanup=PASS\n");
    return true;
}

bool ObserveExternalPrivateLifetime(HWND window, bool facade_mode, bool eager = false) {
    ComPtr<IDirect3D8> native_factory;
    native_factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!native_factory.value) return false;
    D3DDISPLAYMODE display{};
    if (FAILED(native_factory->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display))) return false;
    ComPtr<IDirect3D8> facade_factory;
    auto buffers = t3render::buffers::BufferProxy::Create(ReadFixtureMemory, nullptr);
    if (!buffers) return false;
    IDirect3D8* selected = native_factory.value;
    if (facade_mode) {
        native_factory->AddRef();
        facade_factory.value = eager ? t3render::fixture::CreateEagerResourceFacadeFactory(native_factory.value, buffers) :
                                       t3render::fixture::CreateBufferDeviceFacadeFactory(native_factory.value, buffers);
        if (!facade_factory.value) { native_factory->Release(); return false; }
        selected = facade_factory.value;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window; pp.Windowed = TRUE;
    ComPtr<IDirect3DDevice8> device;
    if (FAILED(selected->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put())) || !device.value) return false;
    ComPtr<IDirect3DTexture8> texture;
    if (FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.put())) || !texture.value)
        return false;
    if (eager) {
        const GUID provenance_key{0x3ee22a9c, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
        const GUID object_key{0x3ee22a9d, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
        const BYTE bytes[]{0x12, 0x34};
        ComPtr<IDirect3DCubeTexture8> target;
        if (FAILED(device->CreateCubeTexture(2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, target.put())) || !target.value ||
            FAILED(texture->SetPrivateData(provenance_key, bytes, sizeof(bytes), 0)) ||
            FAILED(texture->SetPrivateData(object_key, target.value, sizeof(IUnknown*), D3DSPD_IUNKNOWN)) ||
            FAILED(device->SetTexture(0, texture.value))) return false;
        target->Release(); target.value = nullptr;
        texture->Release(); texture.value = nullptr;
        ComPtr<IDirect3DBaseTexture8> binding;
        ComPtr<IDirect3DTexture8> reacquired;
        if (FAILED(device->GetTexture(0, binding.put())) || !binding.value ||
            FAILED(binding->QueryInterface(IID_IDirect3DTexture8, reinterpret_cast<void**>(reacquired.put()))) ||
            !reacquired.value || FAILED(device->SetTexture(0, nullptr))) return false;
        binding->Release(); binding.value = nullptr;
        texture.value = reacquired.value; reacquired.value = nullptr;
        BYTE copied[sizeof(bytes)]{}; DWORD copied_size = sizeof(copied);
        const HRESULT read = texture->GetPrivateData(provenance_key, copied, &copied_size);
        if (FAILED(read) || copied_size != sizeof(bytes) || std::memcmp(copied, bytes, sizeof(bytes))) return false;
        std::printf("private_external_eager_reacquire_provenance=preserved\n");
        // Reacquire the source a second time through a state block, then
        // recover the resource-valued entry as a facade, not a native alias.
        DWORD block = 0;
        if (FAILED(device->SetTexture(0, texture.value)) || FAILED(device->CreateStateBlock(D3DSBT_ALL, &block)) ||
            FAILED(device->SetTexture(0, nullptr))) return false;
        texture->Release(); texture.value = nullptr;
        if (FAILED(device->ApplyStateBlock(block)) || FAILED(device->GetTexture(0, binding.put())) || !binding.value ||
            FAILED(binding->QueryInterface(IID_IDirect3DTexture8, reinterpret_cast<void**>(reacquired.put()))) ||
            !reacquired.value || FAILED(device->SetTexture(0, nullptr)) || FAILED(device->DeleteStateBlock(block))) return false;
        binding->Release(); binding.value = nullptr;
        texture.value = reacquired.value; reacquired.value = nullptr;
        ComPtr<IUnknown> target_unknown;
        ComPtr<IDirect3DCubeTexture8> target_recovered;
        ComPtr<IDirect3DDevice8> target_device;
        DWORD target_size = sizeof(target_unknown.value);
        if (FAILED(texture->GetPrivateData(object_key, target_unknown.put(), &target_size)) || !target_unknown.value ||
            target_size != sizeof(target_unknown.value) ||
            FAILED(target_unknown->QueryInterface(IID_IDirect3DCubeTexture8, reinterpret_cast<void**>(target_recovered.put()))) ||
            !target_recovered.value || FAILED(target_recovered->GetDevice(target_device.put())) ||
            !EqualIdentity(target_device.value, device.value)) return false;
        target_device->Release(); target_device.value = nullptr;
        target_recovered->Release(); target_recovered.value = nullptr;
        target_unknown->Release(); target_unknown.value = nullptr;
        std::printf("private_external_eager_stateblock_and_resource=PASS\n");
    }
    auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto* holder = new (std::nothrow) PrivateDeviceHolder(device.value, destroyed);
    if (!holder) return false;
    const GUID key{0x3ee22a9b, 0x15cf, 0x4f15, {0xa1, 1, 2, 3, 4, 5, 6, 7}};
    if (!facade_mode) {
        const HRESULT set = texture->SetPrivateData(key, holder, sizeof(IUnknown*), D3DSPD_IUNKNOWN);
        holder->Release();
        if (FAILED(set)) return false;
        texture->Release(); texture.value = nullptr;
        if (!destroyed->load()) return false;
        device->Release(); device.value = nullptr;
        std::printf("private_external_%snative_dependent_release=PASS\n", eager ? "eager_" : "");
        return true;
    }
    // Direct forwarding is intentionally simulated on a borrowed native
    // resource, after checking that the real facade still rejects it.
    if (texture->SetPrivateData(key, holder, sizeof(IUnknown*), D3DSPD_IUNKNOWN) != E_NOTIMPL ||
        FAILED(device->SetTexture(0, texture.value))) { holder->Release(); return false; }
    auto* native = static_cast<t3render::fixture::DeviceFixture*>(device.value)->NativeForFixture();
    ComPtr<IDirect3DBaseTexture8> native_texture;
    if (!native || FAILED(native->GetTexture(0, native_texture.put())) || !native_texture.value ||
        FAILED(device->SetTexture(0, nullptr)) ||
        FAILED(native_texture->SetPrivateData(key, holder, sizeof(IUnknown*), D3DSPD_IUNKNOWN))) {
        holder->Release(); return false;
    }
    holder->Release();
    if (eager) {
        native_texture->Release(); native_texture.value = nullptr;
        const ULONG texture_remaining = texture->Release(); texture.value = nullptr;
        if (texture_remaining != 0 || !destroyed->load()) return false;
        const unsigned resources_closed = t3render::fixture::resource_facade_close_successes.load();
        const unsigned buffers_closed = t3render::fixture::buffer_facade_close_successes.load();
        const ULONG device_remaining = device->Release(); device.value = nullptr;
        if (device_remaining != 0 || t3render::fixture::resource_facade_close_successes != resources_closed + 1 ||
            t3render::fixture::buffer_facade_close_successes != buffers_closed + 1) return false;
        std::printf("private_external_eager_release_and_cleanup=PASS\n");
        return true;
    }
    const ULONG texture_remaining = texture->Release(); texture.value = nullptr;
    if (texture_remaining != 0) return false;
    auto* borrowed = native_texture.value;
    native_texture->Release(); native_texture.value = nullptr;
    if (destroyed->load()) return false;
    const ULONG device_remaining = device->Release(); device.value = nullptr;
    if (device_remaining != 1) return false;
    const unsigned resources_closed = t3render::fixture::resource_facade_close_successes.load();
    const unsigned buffers_closed = t3render::fixture::buffer_facade_close_successes.load();
    borrowed->AddRef(); // device teardown may release the dormant wrapper during FreePrivateData
    const HRESULT freed = borrowed->FreePrivateData(key);
    borrowed->Release();
    if (FAILED(freed) || !destroyed->load() || t3render::fixture::resource_facade_close_successes != resources_closed + 1 ||
        t3render::fixture::buffer_facade_close_successes != buffers_closed + 1) return false;
    std::printf("private_external_naive_cycle_and_cleanup=PASS\n");
    return true;
}

bool DrawTexturedSamples(IDirect3DDevice8* device) {
    struct Vertex2 { float x, y, z, rhw, u, v; };
    struct Vertex3 { float x, y, z, rhw, xcoord, ycoord, zcoord; };
    const Vertex2 texture_vertices[] = {{204, 198, 0.5f, 1, 0.5f, 0.5f},
                                        {246, 198, 0.5f, 1, 0.5f, 0.5f},
                                        {246, 240, 0.5f, 1, 0.5f, 0.5f}};
    const Vertex3 cube_vertices[] = {{150, 200, 0.5f, 1, 1, 0, 0},
                                     {190, 200, 0.5f, 1, 1, 0, 0},
                                     {150, 240, 0.5f, 1, 1, 0, 0}};
    ComPtr<IDirect3DTexture8> texture, upload;
    ComPtr<IDirect3DCubeTexture8> cube;
    if (FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, upload.put())) || !upload.value ||
        FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, texture.put())) || !texture.value ||
        FAILED(device->CreateCubeTexture(2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, cube.put())) || !cube.value)
        return false;
    auto fill = [](const D3DLOCKED_RECT& locked, DWORD color) {
        if (!locked.pBits || locked.Pitch < 8) return false;
        for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 2; ++x)
            std::memcpy(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch + x * 4, &color, 4);
        return true;
    };
    D3DLOCKED_RECT locked{};
    if (FAILED(upload->LockRect(0, &locked, nullptr, 0))) return false;
    const bool texture_filled = fill(locked, 0xff1234d0u);
    if (FAILED(upload->UnlockRect(0)) || !texture_filled || FAILED(device->UpdateTexture(upload.value, texture.value)))
        return false;
    const D3DCUBEMAP_FACES faces[] = {D3DCUBEMAP_FACE_POSITIVE_X, D3DCUBEMAP_FACE_NEGATIVE_X,
                                     D3DCUBEMAP_FACE_POSITIVE_Y, D3DCUBEMAP_FACE_NEGATIVE_Y,
                                     D3DCUBEMAP_FACE_POSITIVE_Z, D3DCUBEMAP_FACE_NEGATIVE_Z};
    for (const auto face : faces) {
        locked = {};
        if (FAILED(cube->LockRect(face, 0, &locked, nullptr, 0))) return false;
        const bool cube_filled = fill(locked, 0xff24e4ceu);
        if (FAILED(cube->UnlockRect(face, 0)) || !cube_filled) return false;
    }
    if (FAILED(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE)) ||
        FAILED(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE)) ||
        FAILED(device->SetTexture(0, texture.value)) ||
        FAILED(device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1)) ||
        FAILED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, texture_vertices, sizeof(Vertex2))) ||
        FAILED(device->SetTexture(0, cube.value)) ||
        FAILED(device->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0))) ||
        FAILED(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, cube_vertices, sizeof(Vertex3))) ||
        FAILED(device->SetTexture(0, nullptr))) return false;
    return true;
}

bool CheckTextureStateBlock(IDirect3DDevice8* device) {
    ComPtr<IDirect3DTexture8> texture;
    ComPtr<IDirect3DCubeTexture8> cube;
    ComPtr<IDirect3DVolumeTexture8> volume;
    if (FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.put())) || !texture.value ||
        FAILED(device->CreateCubeTexture(2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, cube.put())) || !cube.value ||
        FAILED(device->CreateVolumeTexture(2, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, volume.put())) || !volume.value ||
        FAILED(device->SetTexture(0, texture.value)) || FAILED(device->SetTexture(1, cube.value)) ||
        FAILED(device->SetTexture(2, volume.value))) return false;
    ComPtr<IUnknown> texture_identity, cube_identity, volume_identity;
    if (!Identity(texture.value, texture_identity) || !Identity(cube.value, cube_identity) ||
        !Identity(volume.value, volume_identity)) return false;
    const auto original_texture = reinterpret_cast<std::uintptr_t>(texture_identity.value);
    const auto original_cube = reinterpret_cast<std::uintptr_t>(cube_identity.value);
    const auto original_volume = reinterpret_cast<std::uintptr_t>(volume_identity.value);
    texture_identity->Release(); texture_identity.value = nullptr;
    cube_identity->Release(); cube_identity.value = nullptr;
    volume_identity->Release(); volume_identity.value = nullptr;
    DWORD token = 0;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &token)) ||
        FAILED(device->SetTexture(0, nullptr)) || FAILED(device->SetTexture(1, nullptr)) ||
        FAILED(device->SetTexture(2, nullptr))) return false;
    texture->Release(); texture.value = nullptr;
    cube->Release(); cube.value = nullptr;
    volume->Release(); volume.value = nullptr;
    if (FAILED(device->ApplyStateBlock(token))) return false;
    ComPtr<IDirect3DBaseTexture8> recovered_texture, recovered_cube, recovered_volume;
    ComPtr<IUnknown> recovered_texture_identity, recovered_cube_identity, recovered_volume_identity;
    if (FAILED(device->GetTexture(0, recovered_texture.put())) || !recovered_texture.value ||
        FAILED(device->GetTexture(1, recovered_cube.put())) || !recovered_cube.value ||
        FAILED(device->GetTexture(2, recovered_volume.put())) || !recovered_volume.value ||
        !Identity(recovered_texture.value, recovered_texture_identity) ||
        !Identity(recovered_cube.value, recovered_cube_identity) ||
        !Identity(recovered_volume.value, recovered_volume_identity) ||
        reinterpret_cast<std::uintptr_t>(recovered_texture_identity.value) != original_texture ||
        reinterpret_cast<std::uintptr_t>(recovered_cube_identity.value) != original_cube ||
        reinterpret_cast<std::uintptr_t>(recovered_volume_identity.value) != original_volume ||
        FAILED(device->DeleteStateBlock(token)) || FAILED(device->SetTexture(0, nullptr)) ||
        FAILED(device->SetTexture(1, nullptr)) || FAILED(device->SetTexture(2, nullptr))) return false;
    std::printf("resource_facade_stateblock_texture_identities=PASS\n");
    return true;
}

bool CheckVolumeResource(IDirect3DDevice8* device, IDirect3DVertexBuffer8* vb, IDirect3DIndexBuffer8* ib) {
    ComPtr<IDirect3DVolumeTexture8> texture;
    ComPtr<IDirect3DVolume8> level, again;
    ComPtr<IDirect3DDevice8> parent;
    if (FAILED(device->CreateVolumeTexture(2, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, texture.put())) ||
        !texture.value || FAILED(texture->GetVolumeLevel(0, level.put())) || !level.value ||
        FAILED(texture->GetVolumeLevel(0, again.put())) || again.value != level.value ||
        FAILED(level->GetDevice(parent.put())) || !parent.value || !EqualIdentity(parent.value, device)) return false;
    ComPtr<IUnknown> container, texture_container;
    if (FAILED(level->GetContainer(IID_IUnknown, reinterpret_cast<void**>(container.put()))) ||
        !container.value || !EqualIdentity(container.value, texture.value) ||
        FAILED(level->GetContainer(IID_IDirect3DVolumeTexture8, reinterpret_cast<void**>(texture_container.put()))) ||
        !texture_container.value || !EqualIdentity(texture_container.value, texture.value)) return false;
    const DWORD colors[8] = {0xff112233u, 0xff223344u, 0xff334455u, 0xff445566u,
                             0xff556677u, 0xff667788u, 0xff778899u, 0xff8899aau};
    D3DLOCKED_BOX box{};
    if (FAILED(texture->LockBox(0, &box, nullptr, 0))) return false;
    bool valid = box.pBits && box.RowPitch >= 8 && box.SlicePitch >= 0 && box.RowPitch <= box.SlicePitch / 2;
    if (valid) for (unsigned z = 0; z < 2; ++z) for (unsigned y = 0; y < 2; ++y)
        std::memcpy(static_cast<BYTE*>(box.pBits) + z * box.SlicePitch + y * box.RowPitch, colors + z * 4 + y * 2, 8);
    if (FAILED(texture->UnlockBox(0)) || !valid || FAILED(level->LockBox(&box, nullptr, D3DLOCK_READONLY))) return false;
    valid = box.pBits && box.RowPitch >= 8 && box.SlicePitch >= 0 && box.RowPitch <= box.SlicePitch / 2;
    if (valid) for (unsigned z = 0; z < 2; ++z) for (unsigned y = 0; y < 2; ++y)
        valid = valid && std::memcmp(static_cast<BYTE*>(box.pBits) + z * box.SlicePitch + y * box.RowPitch,
                                     colors + z * 4 + y * 2, 8) == 0;
    if (FAILED(level->UnlockBox()) || !valid ||
        FAILED(parent->SetStreamSource(0, vb, sizeof(Vertex))) || FAILED(parent->SetIndices(ib, 0))) return false;
    ComPtr<IDirect3DBaseTexture8> recovered;
    if (FAILED(device->SetTexture(0, texture.value)) || FAILED(device->GetTexture(0, recovered.put())) ||
        !recovered.value || !EqualIdentity(recovered.value, texture.value) || FAILED(device->SetTexture(0, nullptr))) return false;
    std::printf("resource_facade_volume_roundtrips=PASS\n");
    return true;
}

bool CheckSwapChain(IDirect3DDevice8* device, D3DPRESENT_PARAMETERS parameters,
                    IDirect3DVertexBuffer8* vb, IDirect3DIndexBuffer8* ib) {
    parameters.EnableAutoDepthStencil = FALSE;
    ComPtr<IDirect3DSwapChain8> swap;
    if (FAILED(device->CreateAdditionalSwapChain(&parameters, swap.put())) || !swap.value) return false;
    ComPtr<IUnknown> unknown;
    ComPtr<IDirect3DSwapChain8> roundtrip;
    if (!Identity(swap.value, unknown) ||
        FAILED(unknown->QueryInterface(IID_IDirect3DSwapChain8, reinterpret_cast<void**>(roundtrip.put()))) ||
        !roundtrip.value || !EqualIdentity(roundtrip.value, swap.value)) return false;
    ComPtr<IDirect3DDevice8> queried_device;
    const HRESULT device_hr = swap->QueryInterface(IID_IDirect3DDevice8,
                                                    reinterpret_cast<void**>(queried_device.put()));
    if ((SUCCEEDED(device_hr) && (!queried_device.value || !EqualIdentity(queried_device.value, device))) ||
        (FAILED(device_hr) && queried_device.value)) return false;
    ComPtr<IDirect3DSurface8> back, back_again;
    ComPtr<IDirect3DDevice8> parent;
    if (FAILED(swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put())) || !back.value ||
        FAILED(swap->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back_again.put())) || back_again.value != back.value ||
        FAILED(back->GetDevice(parent.put())) || !parent.value || !EqualIdentity(parent.value, device) ||
        FAILED(parent->SetStreamSource(0, vb, sizeof(Vertex))) || FAILED(parent->SetIndices(ib, 0))) return false;
    ComPtr<IDirect3DVertexBuffer8> read_vb;
    ComPtr<IDirect3DIndexBuffer8> read_ib;
    UINT stride = 0, base = 0;
    if (FAILED(parent->GetStreamSource(0, read_vb.put(), &stride)) || read_vb.value != vb || stride != sizeof(Vertex) ||
        FAILED(parent->GetIndices(read_ib.put(), &base)) || read_ib.value != ib || base != 0) return false;
    ComPtr<IUnknown> container;
    if (FAILED(back->GetContainer(IID_IUnknown, reinterpret_cast<void**>(container.put()))) || !container.value ||
        (!EqualIdentity(container.value, device) && !EqualIdentity(container.value, swap.value))) return false;
    ComPtr<IUnknown> swap_container;
    const HRESULT container_hr = back->GetContainer(IID_IDirect3DSwapChain8,
                                                     reinterpret_cast<void**>(swap_container.put()));
    if ((SUCCEEDED(container_hr) && (!swap_container.value || !EqualIdentity(swap_container.value, swap.value))) ||
        (FAILED(container_hr) && swap_container.value)) return false;
    ComPtr<IDirect3DDevice8> device_container;
    const HRESULT container_device_hr = back->GetContainer(IID_IDirect3DDevice8,
                                                            reinterpret_cast<void**>(device_container.put()));
    if ((SUCCEEDED(container_device_hr) && (!device_container.value || !EqualIdentity(device_container.value, device))) ||
        (FAILED(container_device_hr) && device_container.value)) return false;
    if (FAILED(swap->Present(nullptr, nullptr, nullptr, nullptr))) return false;
    std::printf("resource_facade_swapchain_container_hr=0x%08lx\n",
                static_cast<unsigned long>(static_cast<std::uint32_t>(container_hr)));
    std::printf("resource_facade_swapchain_device_hr=0x%08lx\n",
                static_cast<unsigned long>(static_cast<std::uint32_t>(device_hr)));
    std::printf("resource_facade_swapchain_container_device_hr=0x%08lx\n",
                static_cast<unsigned long>(static_cast<std::uint32_t>(container_device_hr)));
    std::printf("resource_facade_swapchain_roundtrips=PASS\n");
    return true;
}

bool CheckSurfaceTransfers(IDirect3DDevice8* device, bool facade_mode) {
    ComPtr<IDirect3DSurface8> target, back, source, destination;
    if (FAILED(device->GetRenderTarget(target.put())) || !target.value ||
        FAILED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put())) || !back.value ||
        !EqualIdentity(target.value, back.value) || FAILED(device->SetRenderTarget(target.value, nullptr)) ||
        FAILED(device->CreateImageSurface(2, 2, D3DFMT_A8R8G8B8, source.put())) || !source.value ||
        FAILED(device->CreateImageSurface(2, 2, D3DFMT_A8R8G8B8, destination.put())) || !destination.value) return false;
    ComPtr<IDirect3DDevice8> parent;
    if (FAILED(source->GetDevice(parent.put())) || !parent.value || !EqualIdentity(parent.value, device)) return false;
    const DWORD colors[4] = {0xff132435u, 0xff465768u, 0xff798a9bu, 0xffacbdceu};
    D3DLOCKED_RECT locked{};
    if (FAILED(source->LockRect(&locked, nullptr, 0))) return false;
    bool valid = locked.pBits && locked.Pitch >= 8;
    if (valid) for (unsigned y = 0; y < 2; ++y)
        std::memcpy(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch, colors + y * 2, 8);
    if (FAILED(source->UnlockRect()) || !valid ||
        FAILED(device->CopyRects(source.value, nullptr, 0, destination.value, nullptr)) ||
        FAILED(destination->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return false;
    valid = locked.pBits && locked.Pitch >= 8;
    if (valid) for (unsigned y = 0; y < 2; ++y)
        valid = valid && std::memcmp(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch, colors + y * 2, 8) == 0;
    if (FAILED(destination->UnlockRect()) || !valid) return false;
    if (facade_mode) {
        // The isolated facade must reject unknown pointers without dereferencing
        // them or forwarding them into the native runtime.
        SetLastError(1234);
        if (device->SetTexture(0, reinterpret_cast<IDirect3DBaseTexture8*>(1)) != D3DERR_INVALIDCALL ||
            GetLastError() != 1234 ||
            device->CopyRects(nullptr, nullptr, 0, reinterpret_cast<IDirect3DSurface8*>(1), nullptr) != D3DERR_INVALIDCALL ||
            GetLastError() != 1234) return false;
        std::printf("resource_facade_rejects_unknown_inputs=PASS\n");
    }
    std::printf("resource_facade_surface_copy_and_target=PASS\n");
    return true;
}
#endif

bool SetAndReadState(IDirect3DDevice8* device) {
    D3DMATRIX matrix{};
    matrix._11 = matrix._22 = matrix._33 = matrix._44 = 1.0f;
    const D3DTRANSFORMSTATETYPE kinds[] = {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION};
    const char* set_names[] = {"SetTransformWorld", "SetTransformView", "SetTransformProjection"};
    const char* get_names[] = {"GetTransformWorld", "GetTransformView", "GetTransformProjection"};
    const char* value_names[] = {"World", "View", "Projection"};
    for (unsigned i = 0; i < 3; ++i) {
        matrix._41 = static_cast<float>(i + 1);
        if (!Call(set_names[i], device->SetTransform(kinds[i], &matrix))) return false;
        D3DMATRIX observed{};
        if (!Call(get_names[i], device->GetTransform(kinds[i], &observed)) ||
            std::memcmp(&observed, &matrix, sizeof(matrix))) return false;
        PrintMatrix(value_names[i], observed);
    }
    D3DVIEWPORT8 viewport{0, 0, kWidth, kHeight, 0.0f, 1.0f};
    if (!Call("SetViewport", device->SetViewport(&viewport))) return false;
    D3DVIEWPORT8 observed{};
    if (!Call("GetViewport", device->GetViewport(&observed)) || observed.X != viewport.X || observed.Y != viewport.Y ||
        observed.Width != viewport.Width || observed.Height != viewport.Height ||
        observed.MinZ != viewport.MinZ || observed.MaxZ != viewport.MaxZ) return false;
    std::printf("parity Viewport=%lu,%lu,%lu,%lu,%.1f,%.1f\n", static_cast<unsigned long>(observed.X),
                static_cast<unsigned long>(observed.Y), static_cast<unsigned long>(observed.Width),
                static_cast<unsigned long>(observed.Height), observed.MinZ, observed.MaxZ);
    struct RenderState { D3DRENDERSTATETYPE type; DWORD value; const char* name; };
    const RenderState states[] = {{D3DRS_LIGHTING, FALSE, "Lighting"}, {D3DRS_CULLMODE, D3DCULL_NONE, "Cull"},
                                  {D3DRS_ZENABLE, FALSE, "ZEnable"}, {D3DRS_ALPHABLENDENABLE, FALSE, "AlphaBlend"},
                                  {D3DRS_DITHERENABLE, FALSE, "Dither"}, {D3DRS_FOGENABLE, FALSE, "Fog"}};
    for (const auto& state : states) {
        char set_name[64]{}, get_name[64]{};
        std::snprintf(set_name, sizeof(set_name), "SetRender%s", state.name);
        std::snprintf(get_name, sizeof(get_name), "GetRender%s", state.name);
        DWORD value = 0;
        if (!Call(set_name, device->SetRenderState(state.type, state.value)) ||
            !Call(get_name, device->GetRenderState(state.type, &value)) || value != state.value) return false;
        std::printf("parity Render%s=%lu\n", state.name, static_cast<unsigned long>(value));
    }
    struct StageState { D3DTEXTURESTAGESTATETYPE type; DWORD value; const char* name; };
    const StageState stages[] = {{D3DTSS_COLOROP, D3DTOP_SELECTARG1, "ColorOp"},
                                 {D3DTSS_COLORARG1, D3DTA_DIFFUSE, "ColorArg1"},
                                 {D3DTSS_ALPHAOP, D3DTOP_SELECTARG1, "AlphaOp"},
                                 {D3DTSS_ALPHAARG1, D3DTA_DIFFUSE, "AlphaArg1"}};
    for (const auto& state : stages) {
        char set_name[64]{}, get_name[64]{};
        std::snprintf(set_name, sizeof(set_name), "SetStage%s", state.name);
        std::snprintf(get_name, sizeof(get_name), "GetStage%s", state.name);
        DWORD value = 0;
        if (!Call(set_name, device->SetTextureStageState(0, state.type, state.value)) ||
            !Call(get_name, device->GetTextureStageState(0, state.type, &value)) || value != state.value) return false;
        std::printf("parity Stage%s=%lu\n", state.name, static_cast<unsigned long>(value));
    }
    DWORD fvf = 0;
    if (!Call("SetVertexShader", device->SetVertexShader(kFvf)) ||
        !Call("GetVertexShader", device->GetVertexShader(&fvf)) || fvf != kFvf ||
        !Call("SetPixelShader", device->SetPixelShader(0))) return false;
    std::printf("parity FVF=%lu\n", static_cast<unsigned long>(fvf));
    return true;
}

bool ReadBack(IDirect3DSurface8* surface, std::vector<BYTE>& ppm) {
    D3DSURFACE_DESC desc{};
    if (!Call("BackBufferDesc", surface->GetDesc(&desc)) || desc.Width != kWidth || desc.Height != kHeight ||
        (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)) return false;
    D3DLOCKED_RECT lock{};
    if (!Call("BackBufferLock", surface->LockRect(&lock, nullptr, D3DLOCK_READONLY))) return false;
    constexpr std::size_t row_bytes = kWidth * 4u;
    const auto max_offset = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    bool valid = lock.pBits && lock.Pitch >= static_cast<INT>(row_bytes);
    if (valid) {
        const auto pitch = static_cast<std::size_t>(lock.Pitch);
        valid = pitch <= (max_offset - row_bytes) / (kHeight - 1);
        if (valid) {
            const auto extent = pitch * (kHeight - 1) + row_bytes;
            valid = reinterpret_cast<std::uintptr_t>(lock.pBits) <=
                    std::numeric_limits<std::uintptr_t>::max() - extent;
        }
    }
    if (!valid) {
        Call("BackBufferUnlock", surface->UnlockRect());
        std::fprintf(stderr, "Backbuffer returned an invalid pointer or pitch\n");
        return false;
    }
    const char header[] = "P6\n256 256\n255\n";
    ppm.assign(header, header + sizeof(header) - 1);
    ppm.reserve(ppm.size() + kWidth * kHeight * 3);
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    const UINT sample_x[] = {30, 225, 30, 225, 230, 160};
    const UINT sample_y[] = {30, 30, 225, 160, 210, 210};
    const std::uint32_t expected[] = {0xe43b3b, 0x35c665, 0x438af0, 0xe6c948, 0x1234d0, 0x24e4ce};
#else
    const UINT sample_x[] = {30, 225, 30, 225};
    const UINT sample_y[] = {30, 30, 225, 160};
    const std::uint32_t expected[] = {0xe43b3b, 0x35c665, 0x438af0, 0xe6c948};
#endif
    constexpr unsigned sample_count = sizeof(sample_x) / sizeof(sample_x[0]);
    std::uint32_t samples[sample_count]{};
    for (UINT y = 0; y < kHeight; ++y) {
        const auto* row = static_cast<const BYTE*>(lock.pBits) + static_cast<std::ptrdiff_t>(y) * lock.Pitch;
        for (UINT x = 0; x < kWidth; ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, row + x * 4, sizeof(pixel));
            for (unsigned i = 0; i < sample_count; ++i) if (x == sample_x[i] && y == sample_y[i]) samples[i] = pixel & 0xffffffu;
            ppm.push_back(static_cast<BYTE>(pixel >> 16));
            ppm.push_back(static_cast<BYTE>(pixel >> 8));
            ppm.push_back(static_cast<BYTE>(pixel));
        }
    }
    if (!Call("BackBufferUnlock", surface->UnlockRect())) return false;
    for (unsigned i = 0; i < sample_count; ++i) {
        if (samples[i] != expected[i]) {
            std::fprintf(stderr, "triangle %u: expected 0x%06lx, got 0x%06lx\n", i,
                         static_cast<unsigned long>(expected[i]), static_cast<unsigned long>(samples[i]));
            return false;
        }
    }
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    std::printf("resource_facade_textured_pixels=PASS\n");
#endif
    std::printf("parity BackBufferFormat=%lu\n", static_cast<unsigned long>(desc.Format));
    return true;
}

bool WriteNew(const wchar_t* path, const std::vector<BYTE>& bytes) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "CreateFile failed: %lu\n", GetLastError()); return false; }
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size();
    const bool closed = CloseHandle(file) != 0;
    if (!ok || !closed) DeleteFileW(path);
    return ok && closed;
}

#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
bool CheckReset(HWND window, bool facade_mode) {
    ComPtr<IDirect3D8> native_factory, facade_factory;
    native_factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!native_factory.value) return false;
    auto buffers = t3render::buffers::BufferProxy::Create(ReadFixtureMemory, nullptr);
    if (!buffers) return false;
    IDirect3D8* selected = native_factory.value;
    if (facade_mode) {
        native_factory->AddRef();
        facade_factory.value = t3render::fixture::CreateBufferDeviceFacadeFactory(native_factory.value, buffers);
        if (!facade_factory.value) { native_factory->Release(); return false; }
        selected = facade_factory.value;
    }
    D3DDISPLAYMODE display{};
    if (FAILED(selected->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display))) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.Windowed = TRUE; pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ComPtr<IDirect3DDevice8> device;
    if (FAILED(selected->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put())) || !device.value) return false;
    ComPtr<IDirect3DTexture8> managed;
    ComPtr<IDirect3DSurface8> managed_level;
    ComPtr<IDirect3DVertexBuffer8> surviving_vb;
    if (FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, managed.put())) || !managed.value ||
        FAILED(managed->GetSurfaceLevel(0, managed_level.put())) || !managed_level.value ||
        FAILED(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, surviving_vb.put())) ||
        !surviving_vb.value) return false;
    BYTE* surviving_data = nullptr;
    if (FAILED(surviving_vb->Lock(0, sizeof(Vertex), &surviving_data, 0)) || !surviving_data) return false;
    std::memset(surviving_data, 0x45, sizeof(Vertex));
    if (FAILED(surviving_vb->Unlock())) return false;
    managed->Release(); managed.value = nullptr; // the level retains its native parent
    ComPtr<IDirect3DVertexBuffer8> old_vb;
    ComPtr<IDirect3DIndexBuffer8> old_ib;
    ComPtr<IDirect3DTexture8> old_texture;
    ComPtr<IDirect3DCubeTexture8> old_cube;
    ComPtr<IDirect3DVolumeTexture8> old_volume;
    ComPtr<IDirect3DSurface8> old_target;
    ComPtr<IDirect3DSwapChain8> old_swap;
    if (FAILED(device->CreateVertexBuffer(sizeof(Vertex) * 3, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_DEFAULT, old_vb.put())) || !old_vb.value ||
        FAILED(device->CreateIndexBuffer(sizeof(WORD) * 3, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, old_ib.put())) || !old_ib.value ||
        FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, old_texture.put())) || !old_texture.value ||
        FAILED(device->CreateCubeTexture(2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, old_cube.put())) || !old_cube.value ||
        FAILED(device->CreateVolumeTexture(2, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, old_volume.put())) || !old_volume.value ||
        FAILED(device->CreateRenderTarget(16, 16, display.Format, D3DMULTISAMPLE_NONE, TRUE, old_target.put())) || !old_target.value ||
        FAILED(device->CreateAdditionalSwapChain(&pp, old_swap.put())) || !old_swap.value) return false;
    old_vb->Release(); old_vb.value = nullptr;
    old_ib->Release(); old_ib.value = nullptr;
    old_texture->Release(); old_texture.value = nullptr;
    old_cube->Release(); old_cube.value = nullptr;
    old_volume->Release(); old_volume.value = nullptr;
    old_target->Release(); old_target.value = nullptr;
    old_swap->Release(); old_swap.value = nullptr;
    const HRESULT reset_hr = device->Reset(&pp);
    std::printf("resource_facade_reset_hr=0x%08lx\n", static_cast<unsigned long>(static_cast<std::uint32_t>(reset_hr)));
    if (FAILED(reset_hr)) return false;
    ComPtr<IDirect3DTexture8> recovered_parent;
    ComPtr<IDirect3DDevice8> recovered_device;
    if (FAILED(managed_level->GetContainer(IID_IDirect3DTexture8, reinterpret_cast<void**>(recovered_parent.put()))) ||
        !recovered_parent.value || FAILED(recovered_parent->GetDevice(recovered_device.put())) ||
        !recovered_device.value || !EqualIdentity(recovered_device.value, device.value)) return false;
    if (facade_mode) {
        auto* native = static_cast<t3render::fixture::DeviceFixture*>(device.value)->NativeForFixture();
        auto pin = buffers->UnwrapVertex(native, surviving_vb.value);
        BYTE byte = 0;
        if (!pin.wrapped() || buffers->ReadVertex(native, pin.native(), 0, &byte, 1) !=
                              t3render::ir::RegistryResult::UnknownResource) return false;
    }
    surviving_data = nullptr;
    if (FAILED(surviving_vb->Lock(0, sizeof(Vertex), &surviving_data, 0)) || !surviving_data) return false;
    std::memset(surviving_data, 0x67, sizeof(Vertex));
    if (FAILED(surviving_vb->Unlock())) return false;
    const Vertex vertices[] = {{12, 12, 0.5f, 1, 0xffe43b3b}, {116, 12, 0.5f, 1, 0xffe43b3b},
                               {12, 116, 0.5f, 1, 0xffe43b3b}};
    const WORD indices[] = {0, 1, 2};
    ComPtr<IDirect3DVertexBuffer8> vb, read_vb;
    ComPtr<IDirect3DIndexBuffer8> ib, read_ib;
    if (FAILED(device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_DEFAULT, vb.put())) || !vb.value ||
        FAILED(device->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, ib.put())) || !ib.value)
        return false;
    BYTE* mapped = nullptr;
    if (FAILED(vb->Lock(0, sizeof(vertices), &mapped, 0)) || !mapped) return false;
    std::memcpy(mapped, vertices, sizeof(vertices));
    if (FAILED(vb->Unlock())) return false;
    mapped = nullptr;
    if (FAILED(ib->Lock(0, sizeof(indices), &mapped, 0)) || !mapped) return false;
    std::memcpy(mapped, indices, sizeof(indices));
    if (FAILED(ib->Unlock())) return false;
    UINT stride = 0, base = 0;
    if (FAILED(device->SetStreamSource(0, vb.value, sizeof(Vertex))) || FAILED(device->SetIndices(ib.value, 0)) ||
        FAILED(device->GetStreamSource(0, read_vb.put(), &stride)) || read_vb.value != vb.value || stride != sizeof(Vertex) ||
        FAILED(device->GetIndices(read_ib.put(), &base)) || read_ib.value != ib.value || base != 0) return false;
    std::uint64_t draw_ticket = 0;
    if (facade_mode) {
        auto* native = static_cast<t3render::fixture::DeviceFixture*>(device.value)->NativeForFixture();
        auto vertex = buffers->UnwrapVertex(native, vb.value);
        auto index = buffers->UnwrapIndex(native, ib.value);
        Vertex vertex_copy[3]{}; WORD index_copy[3]{};
        draw_ticket = buffers->BeginDraw(native);
        if (!vertex.wrapped() || !index.wrapped() || !draw_ticket ||
            buffers->ReadVertex(native, vertex.native(), 0, vertex_copy, sizeof(vertex_copy)) != t3render::ir::RegistryResult::Ok ||
            buffers->ReadIndex(native, index.native(), 0, index_copy, sizeof(index_copy)) != t3render::ir::RegistryResult::Ok ||
            std::memcmp(vertex_copy, vertices, sizeof(vertices)) || std::memcmp(index_copy, indices, sizeof(indices))) return false;
    }
    if (FAILED(device->SetVertexShader(kFvf)) || FAILED(device->SetPixelShader(0)) ||
        FAILED(device->SetRenderState(D3DRS_LIGHTING, FALSE)) || FAILED(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE)) ||
        FAILED(device->SetRenderState(D3DRS_ZENABLE, FALSE)) || FAILED(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE)) ||
        FAILED(device->SetTexture(0, nullptr)) || FAILED(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1)) ||
        FAILED(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE)) ||
        FAILED(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE)) ||
        FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff111318, 1, 0)) || FAILED(device->BeginScene()) ||
        FAILED(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 3, 0, 1)) || FAILED(device->EndScene())) return false;
    if (facade_mode) {
        auto* native = static_cast<t3render::fixture::DeviceFixture*>(device.value)->NativeForFixture();
        if (!buffers->EndDraw(native, draw_ticket)) return false;
        std::printf("resource_facade_post_reset_buffer_epoch=PASS\n");
    }
    ComPtr<IDirect3DSurface8> back;
    D3DLOCKED_RECT locked{};
    if (FAILED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put())) || !back.value ||
        FAILED(back->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return false;
    std::uint32_t pixel = 0;
    const bool valid = locked.pBits && locked.Pitch >= static_cast<INT>(kWidth * 4);
    if (valid) std::memcpy(&pixel, static_cast<const BYTE*>(locked.pBits) + 30 * locked.Pitch + 30 * 4, sizeof(pixel));
    if (FAILED(back->UnlockRect()) || !valid || (pixel & 0xffffffu) != 0xe43b3b ||
        FAILED(device->Present(nullptr, nullptr, nullptr, nullptr))) return false;
    std::printf("resource_facade_reset_pixel=0x%06lx\n", static_cast<unsigned long>(pixel & 0xffffffu));
    std::printf("resource_facade_reset_roundtrips=PASS\n");
    return true;
}

bool CheckResetWithLiveResources(HWND window, bool facade_mode) {
    ComPtr<IDirect3D8> native_factory, facade_factory;
    native_factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!native_factory.value) return false;
    auto buffers = t3render::buffers::BufferProxy::Create(ReadFixtureMemory, nullptr);
    if (!buffers) return false;
    IDirect3D8* selected = native_factory.value;
    if (facade_mode) {
        native_factory->AddRef();
        facade_factory.value = t3render::fixture::CreateBufferDeviceFacadeFactory(native_factory.value, buffers);
        if (!facade_factory.value) { native_factory->Release(); return false; }
        selected = facade_factory.value;
    }
    D3DDISPLAYMODE display{};
    if (FAILED(selected->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display))) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window; pp.Windowed = TRUE;
    ComPtr<IDirect3DDevice8> device;
    if (FAILED(selected->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put())) || !device.value) return false;
    ComPtr<IDirect3DVertexBuffer8> vb;
    ComPtr<IDirect3DTexture8> texture;
    if (FAILED(device->CreateVertexBuffer(sizeof(Vertex) * 3, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_DEFAULT, vb.put())) || !vb.value ||
        FAILED(device->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, texture.put())) || !texture.value) return false;
    const HRESULT held_hr = device->Reset(&pp);
    ComPtr<IDirect3DDevice8> vb_parent, texture_parent;
    if (FAILED(vb->GetDevice(vb_parent.put())) || !vb_parent.value || !EqualIdentity(vb_parent.value, device.value) ||
        FAILED(texture->GetDevice(texture_parent.put())) || !texture_parent.value || !EqualIdentity(texture_parent.value, device.value))
        return false;
    vb->Release(); vb.value = nullptr;
    texture->Release(); texture.value = nullptr;
    const HRESULT recovery_hr = device->Reset(&pp);
    std::printf("resource_facade_live_reset_hr=0x%08lx\n", static_cast<unsigned long>(static_cast<std::uint32_t>(held_hr)));
    std::printf("resource_facade_recovery_reset_hr=0x%08lx\n", static_cast<unsigned long>(static_cast<std::uint32_t>(recovery_hr)));
    if (FAILED(recovery_hr)) return false;
    ComPtr<IDirect3DVertexBuffer8> fresh;
    if (FAILED(device->CreateVertexBuffer(sizeof(Vertex), D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, fresh.put())) || !fresh.value)
        return false;
    BYTE* data = nullptr;
    if (FAILED(fresh->Lock(0, sizeof(Vertex), &data, 0)) || !data) return false;
    std::memset(data, 0x38, sizeof(Vertex));
    if (FAILED(fresh->Unlock())) return false;
    if (facade_mode) {
        auto* native = static_cast<t3render::fixture::DeviceFixture*>(device.value)->NativeForFixture();
        auto pin = buffers->UnwrapVertex(native, fresh.value);
        BYTE copied[sizeof(Vertex)]{};
        if (!pin.wrapped() || buffers->ReadVertex(native, pin.native(), 0, copied, sizeof(copied)) !=
                              t3render::ir::RegistryResult::Ok) return false;
        for (BYTE value : copied) if (value != 0x38) return false;
        std::printf("resource_facade_recovery_buffer_epoch=PASS\n");
    }
    std::printf("resource_facade_live_reset_roundtrips=PASS\n");
    return true;
}
#endif

bool Render(HWND window, bool facade_mode, const wchar_t* output) {
    ComPtr<IDirect3D8> native_factory;
    native_factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!native_factory.value) return false;
    D3DDISPLAYMODE display{};
    if (!Call("GetAdapterDisplayMode", native_factory->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display))) return false;
    ComPtr<IDirect3D8> facade_factory;
    IDirect3D8* selected = native_factory.value;
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    auto buffers = t3render::buffers::BufferProxy::Create(ReadFixtureMemory, nullptr);
    if (!buffers) return false;
#endif
    if (facade_mode) {
        native_factory->AddRef();
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
        facade_factory.value = t3render::fixture::CreateBufferDeviceFacadeFactory(native_factory.value, buffers);
#else
        facade_factory.value = t3render::fixture::CreateDeviceFacadeFactory(native_factory.value);
#endif
        if (!facade_factory.value) { native_factory->Release(); return false; }
        selected = facade_factory.value;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = display.Format; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window; pp.Windowed = TRUE;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ComPtr<IDirect3DDevice8> device;
    if (!Call("CreateDevice", selected->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
              D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put())) || !device.value) return false;
    IDirect3D8* expected_factory = selected;
    if (facade_mode) { facade_factory->Release(); facade_factory.value = nullptr; }
    ComPtr<IDirect3D8> returned_factory;
    if (!Call("GetDirect3D", device->GetDirect3D(returned_factory.put())) || !returned_factory.value ||
        !EqualIdentity(returned_factory.value, expected_factory) ||
        (facade_mode && returned_factory.value != expected_factory)) return false;
    ComPtr<IUnknown> factory_unknown, device_unknown;
    ComPtr<IDirect3D8> factory_roundtrip;
    ComPtr<IDirect3DDevice8> device_roundtrip;
    if (!Call("FactoryQIUnknown", returned_factory->QueryInterface(IID_IUnknown,
              reinterpret_cast<void**>(factory_unknown.put()))) || !factory_unknown.value ||
        !Call("FactoryQIRoundtrip", factory_unknown->QueryInterface(IID_IDirect3D8,
              reinterpret_cast<void**>(factory_roundtrip.put()))) ||
        !factory_roundtrip.value || !EqualIdentity(factory_roundtrip.value, returned_factory.value) ||
        (facade_mode && factory_roundtrip.value != returned_factory.value) ||
        !Call("DeviceQIUnknown", device->QueryInterface(IID_IUnknown,
              reinterpret_cast<void**>(device_unknown.put()))) || !device_unknown.value ||
        !Call("DeviceQIRoundtrip", device_unknown->QueryInterface(IID_IDirect3DDevice8,
              reinterpret_cast<void**>(device_roundtrip.put()))) ||
        !device_roundtrip.value || !EqualIdentity(device_roundtrip.value, device.value) ||
        (facade_mode && device_roundtrip.value != device.value)) return false;
    std::printf("factory_and_device_QI_roundtrip=PASS factory_initial_release=%s\n", facade_mode ? "PASS" : "not_applicable");

    const Vertex buffered[] = {
        {12, 12, 0.5f, 1, 0xffe43b3b}, {116, 12, 0.5f, 1, 0xffe43b3b}, {12, 116, 0.5f, 1, 0xffe43b3b},
        {140, 12, 0.5f, 1, 0xff35c665}, {244, 12, 0.5f, 1, 0xff35c665}, {244, 116, 0.5f, 1, 0xff35c665},
    };
    const WORD indices[] = {3, 4, 5};
    const Vertex up[] = {{12, 140, 0.5f, 1, 0xff438af0}, {116, 244, 0.5f, 1, 0xff438af0}, {12, 244, 0.5f, 1, 0xff438af0}};
    const Vertex indexed_up[] = {{140, 140, 0.5f, 1, 0xffe6c948}, {244, 140, 0.5f, 1, 0xffe6c948}, {244, 244, 0.5f, 1, 0xffe6c948}};
    const WORD up_indices[] = {0, 1, 2};
    ComPtr<IDirect3DVertexBuffer8> vb;
    ComPtr<IDirect3DIndexBuffer8> ib;
    ComPtr<IDirect3DTexture8> texture;
    ComPtr<IDirect3DSurface8> back;
    if (!Call("CreateVertexBuffer", device->CreateVertexBuffer(sizeof(buffered), D3DUSAGE_WRITEONLY, kFvf,
                                                                  D3DPOOL_MANAGED, vb.put())) || !vb.value ||
        !Call("CreateIndexBuffer", device->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                                               D3DPOOL_MANAGED, ib.put())) || !ib.value ||
        !Call("CreateTexture", device->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                       texture.put())) || !texture.value ||
        !Call("GetBackBuffer", device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put())) || !back.value) return false;
    if (facade_mode && !CheckNativeEscape(device.value, native_factory.value, back.value, texture.value, vb.value, ib.value)) return false;
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    if (!ObservePrivateDataResources(device.value, vb.value, ib.value, texture.value, back.value, facade_mode)) return false;
#endif
    BYTE* data = nullptr;
    if (!Call("VertexLock", vb->Lock(0, sizeof(buffered), &data, 0)) || !data) return false;
    std::memcpy(data, buffered, sizeof(buffered));
    if (!Call("VertexUnlock", vb->Unlock())) return false;
    data = nullptr;
    if (!Call("IndexLock", ib->Lock(0, sizeof(indices), &data, 0)) || !data) return false;
    std::memcpy(data, indices, sizeof(indices));
    if (!Call("IndexUnlock", ib->Unlock()) || !SetAndReadState(device.value)) return false;
    if (!Call("SetTexture", device->SetTexture(0, texture.value))) return false;
    ComPtr<IDirect3DBaseTexture8> read_texture;
    if (!Call("GetTexture", device->GetTexture(0, read_texture.put())) || !read_texture.value ||
        !EqualIdentity(texture.value, read_texture.value) || !Call("ClearTexture", device->SetTexture(0, nullptr))) return false;
    IDirect3DDevice8* draw_device = device.value;
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    ComPtr<IDirect3DDevice8> buffer_device, native_device;
    IDirect3DDevice8* native_owner = nullptr;
    if (facade_mode) {
        if (FAILED(vb->GetDevice(buffer_device.put())) || buffer_device.value != device.value) return false;
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
        native_owner = static_cast<t3render::fixture::DeviceFixture*>(device.value)->NativeForFixture();
#else
        if (FAILED(back->GetDevice(native_device.put())) || !native_device.value) return false;
        native_owner = native_device.value;
#endif
        if (!native_owner) return false;
        draw_device = buffer_device.value;
    }
#endif
    if (!Call("SetStreamSource", draw_device->SetStreamSource(0, vb.value, sizeof(Vertex))) ||
        !Call("SetIndices", draw_device->SetIndices(ib.value, 0))) return false;
    ComPtr<IDirect3DVertexBuffer8> read_vb;
    ComPtr<IDirect3DIndexBuffer8> read_ib;
    UINT stride = 0, base = 0;
    if (!Call("GetStreamSource", draw_device->GetStreamSource(0, read_vb.put(), &stride)) || !read_vb.value ||
        !EqualIdentity(vb.value, read_vb.value) || stride != sizeof(Vertex) ||
        !Call("GetIndices", draw_device->GetIndices(read_ib.put(), &base)) || !read_ib.value ||
        !EqualIdentity(ib.value, read_ib.value) || base != 0) return false;
    std::printf("parity Bindings stride=%u base=%u\n", stride, base);
    if (!Call("Clear", device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1, 0)) ||
        !Call("BeginScene", device->BeginScene())) return false;
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    std::uint64_t window_token = 0;
    if (facade_mode) {
        auto vertex = buffers->UnwrapVertex(native_owner, vb.value);
        auto index = buffers->UnwrapIndex(native_owner, ib.value);
        Vertex vertex_copy[6]{}; WORD index_copy[3]{};
        window_token = buffers->BeginDraw(native_owner);
        if (!vertex.wrapped() || !index.wrapped() || !window_token ||
            buffers->ReadVertex(native_owner, vertex.native(), 0, vertex_copy, sizeof(vertex_copy)) != t3render::ir::RegistryResult::Ok ||
            buffers->ReadIndex(native_owner, index.native(), 0, index_copy, sizeof(index_copy)) != t3render::ir::RegistryResult::Ok ||
            std::memcmp(vertex_copy, buffered, sizeof(buffered)) || std::memcmp(index_copy, indices, sizeof(indices))) return false;
    }
#endif
    const bool drawn = Call("DrawPrimitive", draw_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)) &&
        Call("DrawIndexedPrimitive", draw_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 3, 3, 0, 1)) &&
        Call("DrawPrimitiveUP", draw_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, up, sizeof(Vertex))) &&
        Call("DrawIndexedPrimitiveUP", draw_device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, up_indices,
                                                                       D3DFMT_INDEX16, indexed_up, sizeof(Vertex)));
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    if (facade_mode) {
        if (!buffers->EndDraw(native_owner, window_token)) return false;
        std::printf("buffer_shadow_writeonly_and_draw_window=PASS\n");
    }
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    if (drawn && !DrawTexturedSamples(draw_device)) return false;
#endif
    const bool ended = Call("EndScene", device->EndScene());
    if (!drawn || !ended) return false;
    std::vector<BYTE> pixels;
    if (!ReadBack(back.value, pixels) || !Call("Present", device->Present(nullptr, nullptr, nullptr, nullptr)) ||
        !WriteNew(output, pixels)) return false;
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    if (!CheckVolumeResource(draw_device, vb.value, ib.value)) return false;
    if (!CheckSwapChain(draw_device, pp, vb.value, ib.value)) return false;
#endif
    if (facade_mode && !CheckRetainedBuffer(draw_device, native_owner, buffers)) return false;
#endif
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    if (!CheckTextureStateBlock(draw_device) || !CheckSurfaceTransfers(draw_device, facade_mode)) return false;
    if (facade_mode && !CheckFailedStateBlock(draw_device, native_owner, buffers)) return false;
#endif
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || (std::wcscmp(argv[1], L"--baseline") && std::wcscmp(argv[1], L"--facade"))) {
        std::fprintf(stderr, "usage: fixture (--baseline|--facade) new-output.ppm\n"); return 2;
    }
    const bool facade_mode = std::wcscmp(argv[1], L"--facade") == 0;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderD3D8DeviceFacadeFixture";
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = instance; cls.lpszClassName = name;
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(name, L"T3Render D3D8 device facade", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 300, 300, nullptr, nullptr, instance, nullptr);
    if (!window) { UnregisterClassW(name, instance); return 1; }
#ifndef T3RENDER_BUFFER_FACADE_FIXTURE
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
#endif
    bool ok = Render(window, facade_mode, argv[2]);
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    if (ok) ok = CheckReset(window, facade_mode);
    if (ok) ok = CheckResetWithLiveResources(window, facade_mode);
    if (ok) ok = ObservePrivateResourceLifetime(window, facade_mode);
    if (ok) ok = ObserveTranslatedPrivateLifetime(window, facade_mode);
    if (ok) ok = ObserveExternalPrivateLifetime(window, facade_mode);
    if (ok) ok = ObserveExternalPrivateLifetime(window, facade_mode, true);
#endif
#ifdef T3RENDER_BUFFER_FACADE_FIXTURE
    if (facade_mode) {
        ok = ok && t3render::fixture::buffer_facade_close_successes ==
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
             7 &&
#else
             1 &&
#endif
             t3render::fixture::buffer_facade_close_failures == 0;
        if (ok) std::printf("buffer_facade_final_close=PASS\n");
    }
#ifdef T3RENDER_RESOURCE_FACADE_FIXTURE
    if (facade_mode) {
        ok = ok && t3render::fixture::resource_facade_close_successes == 7 &&
             t3render::fixture::resource_facade_close_failures == 0;
        if (ok) std::printf("resource_facade_final_close=PASS\n");
    }
#endif
#endif
    DestroyWindow(window);
    UnregisterClassW(name, instance);
    std::printf("device_facade_fixture=%s mode=%s\n", ok ? "PASS" : "FAIL", facade_mode ? "facade" : "baseline");
    return ok ? 0 : 1;
}
