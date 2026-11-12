#include "d3d8_buffer_wrappers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>

using namespace t3render::harness;
using t3render::ir::RegistryResult;

namespace {
constexpr IID kUnknownAlias{99};
constexpr GUID kPrivateGuid{101};

struct FakeDevice : IDirect3DDevice8 {
    ULONG refs = 1;
    IDirect3DVertexBuffer8* vertex = nullptr;
    IDirect3DIndexBuffer8* index = nullptr;
    UINT stride = 0, base = 0;
    HRESULT stream_get_result = S_OK, index_get_result = S_OK, set_result = S_OK;
    HRESULT process_result = S_OK;
    unsigned stream_gets = 0, index_gets = 0, process_calls = 0;
    IDirect3DVertexBuffer8* process_target = nullptr;
    DWORD process_input_error = 0, stream_input_error = 0, index_input_error = 0;

    HRESULT QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3DDevice8)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG AddRef() override { return ++refs; }
    ULONG Release() override { assert(refs); return --refs; }
    HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer8* value, UINT value_stride) override {
        assert(stream == 0);
        if (FAILED(set_result)) return set_result;
        if (value) value->AddRef();
        if (vertex) vertex->Release();
        vertex = value; stride = value_stride; return S_OK;
    }
    HRESULT GetStreamSource(UINT stream, IDirect3DVertexBuffer8** out, UINT* out_stride) override {
        ++stream_gets; assert(stream == 0); stream_input_error = GetLastError(); SetLastError(820);
        if (FAILED(stream_get_result)) return stream_get_result;
        if (!out || !out_stride) return E_POINTER;
        *out = vertex; *out_stride = stride;
        if (vertex) vertex->AddRef();
        return S_OK;
    }
    HRESULT SetIndices(IDirect3DIndexBuffer8* value, UINT value_base) override {
        if (FAILED(set_result)) return set_result;
        if (value) value->AddRef();
        if (index) index->Release();
        index = value; base = value_base; return S_OK;
    }
    HRESULT GetIndices(IDirect3DIndexBuffer8** out, UINT* out_base) override {
        ++index_gets; index_input_error = GetLastError(); SetLastError(830);
        if (FAILED(index_get_result)) return index_get_result;
        if (!out || !out_base) return E_POINTER;
        *out = index; *out_base = base;
        if (index) index->AddRef();
        return S_OK;
    }
    HRESULT ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer8* target, DWORD) override {
        ++process_calls; process_target = target; process_input_error = GetLastError(); SetLastError(840);
        return process_result;
    }
};

struct FakeAlias {
    ULONG refs = 0;
    IDirect3DResource8* owner = nullptr;
    ULONG Release() { assert(refs && owner); --refs; return owner->Release(); }
};

template <class Interface, class Desc, D3DRESOURCETYPE Type>
struct FakeBuffer : Interface {
    FakeDevice* device;
    ULONG refs = 1;
    bool destroyed = false, map_null = false;
    HRESULT lock_result = S_OK, unlock_result = S_OK, desc_result = S_OK;
    HRESULT private_result = S_OK;
    UINT lock_offset = 0, lock_length = 0;
    DWORD lock_flags = 0, priority = 0;
    unsigned locks = 0, unlocks = 0, preload = 0, get_device = 0;
    DWORD lock_input_error = 0, unlock_input_error = 0;
    std::atomic<bool> block_lock{false}, lock_entered{false}, finish_lock{false};
    std::atomic<bool> block_unlock{false}, unlock_entered{false}, finish_unlock{false};
    std::function<void()> lock_callback, unlock_callback;
    unsigned private_sets = 0, private_gets = 0, private_frees = 0;
    std::array<BYTE, 32> bytes{};
    FakeAlias alias;

    explicit FakeBuffer(FakeDevice* owner) : device(owner) { alias.owner = this; }
    HRESULT QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (IsEqualIID(iid, kUnknownAlias)) { ++alias.refs; AddRef(); *out = &alias; return S_OK; }
        const bool type = Type == D3DRTYPE_VERTEXBUFFER ? IsEqualIID(iid, IID_IDirect3DVertexBuffer8)
                                                       : IsEqualIID(iid, IID_IDirect3DIndexBuffer8);
        if (!type && !IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3DResource8)) return E_NOINTERFACE;
        *out = static_cast<Interface*>(this); AddRef(); return S_OK;
    }
    ULONG AddRef() override { assert(!destroyed); return ++refs; }
    ULONG Release() override { SetLastError(870); assert(refs); --refs; if (!refs) destroyed = true; return refs; }
    HRESULT GetDevice(IDirect3DDevice8** out) override {
        ++get_device;
        if (!out) return E_POINTER;
        *out = device; device->AddRef(); return S_OK;
    }
    HRESULT SetPrivateData(REFGUID guid, const void*, DWORD, DWORD) override {
        assert(guid.value == kPrivateGuid.value); ++private_sets; return private_result;
    }
    HRESULT GetPrivateData(REFGUID guid, void*, DWORD*) override {
        assert(guid.value == kPrivateGuid.value); ++private_gets; return private_result;
    }
    HRESULT FreePrivateData(REFGUID guid) override {
        assert(guid.value == kPrivateGuid.value); ++private_frees; return private_result;
    }
    DWORD SetPriority(DWORD value) override { const DWORD old = priority; priority = value; return old; }
    DWORD GetPriority() override { return priority; }
    void PreLoad() override { ++preload; }
    D3DRESOURCETYPE GetType() override { return Type; }
    HRESULT Lock(UINT offset, UINT length, BYTE** out, DWORD flags) override {
        ++locks; lock_offset = offset; lock_length = length; lock_flags = flags;
        lock_input_error = GetLastError(); SetLastError(850);
        if (lock_callback) lock_callback();
        if (block_lock.exchange(false)) { lock_entered = true; while (!finish_lock.load()) std::this_thread::yield(); }
        if (FAILED(lock_result)) return lock_result;
        if (!out || offset > bytes.size() || (length && length > bytes.size() - offset)) return D3DERR_INVALIDCALL;
        *out = map_null ? nullptr : bytes.data() + offset; return S_OK;
    }
    HRESULT Unlock() override {
        ++unlocks; unlock_input_error = GetLastError(); SetLastError(860);
        if (unlock_callback) unlock_callback();
        if (block_unlock.exchange(false)) { unlock_entered = true; while (!finish_unlock.load()) std::this_thread::yield(); }
        return unlock_result;
    }
    HRESULT GetDesc(Desc* out) override {
        if (FAILED(desc_result)) return desc_result;
        if (!out) return E_POINTER;
        *out = {}; out->Type = Type; out->Size = static_cast<UINT>(bytes.size()); out->Pool = D3DPOOL_MANAGED;
        return S_OK;
    }
};
using FakeVertex = FakeBuffer<IDirect3DVertexBuffer8, D3DVERTEXBUFFER_DESC, D3DRTYPE_VERTEXBUFFER>;
using FakeIndex = FakeBuffer<IDirect3DIndexBuffer8, D3DINDEXBUFFER_DESC, D3DRTYPE_INDEXBUFFER>;

struct ReaderContext { bool fail = false, partial = false; unsigned calls = 0; };
bool Reader(void* context, const void* source, void* destination, std::size_t length) {
    auto& reader = *static_cast<ReaderContext*>(context); ++reader.calls;
    SetLastError(999);
    if (reader.fail) return false;
    std::memcpy(destination, source, reader.partial ? length - 1 : length);
    return !reader.partial;
}

template <class Wrapper>
void Fill(Wrapper* wrapper, UINT offset, const BYTE* data, UINT size) {
    BYTE* mapped = nullptr;
    assert(wrapper->Lock(offset, size, &mapped, 0) == S_OK && mapped);
    std::memcpy(mapped, data, size);
    assert(wrapper->Unlock() == S_OK);
}

void VertexForwarding() {
    FakeDevice device;
    FakeVertex raw(&device);
    const auto original_vtable = *reinterpret_cast<void**>(&raw);
    ReaderContext reader;
    assert(!VertexBufferWrapper::Adopt(nullptr, 32, Reader, &reader));
    assert(!VertexBufferWrapper::Adopt(&raw, 32, nullptr, &reader) && raw.refs == 1);
    auto* wrapper = VertexBufferWrapper::Adopt(&raw, raw.bytes.size(), Reader, &reader);
    assert(wrapper && wrapper->Real() == &raw && raw.refs == 1);
    assert(*reinterpret_cast<void**>(&raw) == original_vtable);
    void* output = reinterpret_cast<void*>(1);
    assert(wrapper->QueryInterface(IID_IUnknown, &output) == S_OK && output == wrapper);
    assert(wrapper->Release() >= 1);
    assert(wrapper->QueryInterface(IID_IDirect3DResource8, &output) == S_OK && output == wrapper);
    assert(wrapper->Release() >= 1);
    assert(wrapper->QueryInterface(IID_IDirect3DVertexBuffer8, &output) == S_OK && output == wrapper);
    assert(wrapper->Release() >= 1);
    assert(wrapper->QueryInterface(IID_IUnknown, nullptr) == E_POINTER);

    assert(wrapper->SetPrivateData(kPrivateGuid, nullptr, 0, 0) == S_OK);
    DWORD size = 0;
    assert(wrapper->GetPrivateData(kPrivateGuid, nullptr, &size) == S_OK);
    assert(wrapper->FreePrivateData(kPrivateGuid) == S_OK);
    raw.private_result = E_FAIL;
    assert(wrapper->SetPrivateData(kPrivateGuid, nullptr, 0, 0) == E_FAIL);
    assert(raw.private_sets == 2 && raw.private_gets == 1 && raw.private_frees == 1);
    assert(wrapper->SetPriority(7) == 0 && wrapper->GetPriority() == 7);
    wrapper->PreLoad(); assert(raw.preload == 1 && wrapper->GetType() == D3DRTYPE_VERTEXBUFFER);
    D3DVERTEXBUFFER_DESC desc{};
    assert(wrapper->GetDesc(&desc) == S_OK && desc.Size == raw.bytes.size());
    raw.desc_result = E_FAIL; assert(wrapper->GetDesc(&desc) == E_FAIL);

    BYTE bytes[]{10, 20, 30, 40};
    BYTE observed[4]{};
    assert(wrapper->ReadObserved(4, observed, sizeof(observed)) == RegistryResult::UnknownData);
    Fill(wrapper, 4, bytes, sizeof(bytes));
    assert(raw.locks == 1 && raw.unlocks == 1 && raw.lock_offset == 4 && raw.lock_length == 4 && raw.lock_flags == 0);
    assert(wrapper->ReadObserved(4, observed, sizeof(observed)) == RegistryResult::Ok &&
           std::memcmp(observed, bytes, sizeof(bytes)) == 0);
    assert(reader.calls == 2 && *reinterpret_cast<void**>(&raw) == original_vtable);

    // The device receives only the real resource. Its getter returns a real
    // AddRef, which the fixture helper must convert to a wrapper reference.
    assert(device.SetStreamSource(0, wrapper->Real(), 20) == S_OK && device.vertex == &raw);
    IDirect3DVertexBuffer8* got = nullptr; UINT stride = 0; bool matched = false;
    SetLastError(720);
    assert(GetWrappedStream(&device, 0, wrapper, &got, &stride, matched) == S_OK);
    assert(matched && got == wrapper && stride == 20);
    assert(device.stream_input_error == 720 && GetLastError() == 820);
    assert(got->Release() >= 1);
    device.stream_get_result = E_FAIL; got = nullptr; stride = 123; matched = true;
    SetLastError(721);
    assert(GetWrappedStream(&device, 0, wrapper, &got, &stride, matched) == E_FAIL);
    assert(device.stream_input_error == 721 && GetLastError() == 820);
    device.stream_get_result = S_OK;
    assert(device.SetStreamSource(0, nullptr, 0) == S_OK);
    IDirect3DDevice8* owner = nullptr;
    assert(wrapper->GetDevice(&owner) == S_OK && owner == &device && device.refs == 2);
    assert(owner->Release() == 1);
    assert(wrapper->ReadObserved(4, observed, sizeof(observed)) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0 && raw.refs == 0 && raw.destroyed);
    assert(device.refs == 1);
}

void IndexForwarding() {
    FakeDevice device;
    FakeIndex raw(&device);
    const auto original_vtable = *reinterpret_cast<void**>(&raw);
    ReaderContext reader;
    auto* wrapper = IndexBufferWrapper::Adopt(&raw, raw.bytes.size(), Reader, &reader);
    assert(wrapper && wrapper->Real() == &raw);
    void* output = nullptr;
    assert(wrapper->QueryInterface(IID_IUnknown, &output) == S_OK && output == wrapper);
    assert(wrapper->Release() >= 1);
    assert(wrapper->QueryInterface(IID_IDirect3DResource8, &output) == S_OK && output == wrapper);
    assert(wrapper->Release() >= 1);
    assert(wrapper->QueryInterface(IID_IDirect3DIndexBuffer8, &output) == S_OK && output == wrapper);
    assert(wrapper->Release() >= 1);
    const BYTE values[]{1, 0, 2, 0, 3, 0};
    Fill(wrapper, 2, values, sizeof(values));
    BYTE observed[sizeof(values)]{};
    assert(wrapper->ReadObserved(2, observed, sizeof(observed)) == RegistryResult::Ok &&
           std::memcmp(observed, values, sizeof(values)) == 0);
    BYTE* mapped = nullptr;
    assert(wrapper->Lock(4, 2, &mapped, D3DLOCK_NOOVERWRITE) == S_OK);
    mapped[0] = 9; mapped[1] = 0;
    assert(wrapper->Unlock() == S_OK && raw.lock_flags == D3DLOCK_NOOVERWRITE);
    const BYTE patched[]{1, 0, 9, 0, 3, 0};
    assert(wrapper->ReadObserved(2, observed, sizeof(observed)) == RegistryResult::Ok &&
           std::memcmp(observed, patched, sizeof(patched)) == 0);
    assert(device.SetIndices(wrapper->Real(), 1) == S_OK && device.index == &raw);
    IDirect3DIndexBuffer8* got = nullptr; UINT base = 0; bool matched = false;
    SetLastError(730);
    assert(GetWrappedIndices(&device, wrapper, &got, &base, matched) == S_OK);
    assert(matched && got == wrapper && base == 1);
    assert(device.index_input_error == 730 && GetLastError() == 830);
    assert(got->Release() >= 1);
    device.index_get_result = E_FAIL; got = nullptr; matched = true;
    SetLastError(731);
    assert(GetWrappedIndices(&device, wrapper, &got, &base, matched) == E_FAIL);
    assert(device.index_input_error == 731 && GetLastError() == 830);
    device.index_get_result = S_OK;
    assert(device.SetIndices(nullptr, 0) == S_OK);
    assert(*reinterpret_cast<void**>(&raw) == original_vtable);
    assert(wrapper->Release() == 0 && raw.refs == 0 && raw.destroyed);
}

void AliasesAndFailures() {
    const BYTE data[]{5, 6, 7};
    BYTE output[3]{};
    FakeDevice device;
    ReaderContext reader;
    FakeVertex first(&device), different(&device);
    auto* wrapper = VertexBufferWrapper::Adopt(&first, first.bytes.size(), Reader, &reader);
    assert(wrapper);
    Fill(wrapper, 0, data, sizeof(data));
    assert(device.SetStreamSource(0, &different, 20) == S_OK);
    IDirect3DVertexBuffer8* raw_out = nullptr; UINT stride = 0; bool matched = true;
    assert(GetWrappedStream(&device, 0, wrapper, &raw_out, &stride, matched) == S_OK);
    assert(!matched && raw_out == &different && stride == 20);
    assert(wrapper->ReadObserved(0, output, sizeof(output)) == RegistryResult::Unavailable);
    raw_out->Release(); assert(device.SetStreamSource(0, nullptr, 0) == S_OK);
    assert(wrapper->Release() == 0 && first.refs == 0);
    assert(different.Release() == 0);

    FakeVertex aliased(&device);
    wrapper = VertexBufferWrapper::Adopt(&aliased, aliased.bytes.size(), Reader, &reader);
    Fill(wrapper, 0, data, sizeof(data));
    void* alias = nullptr;
    assert(wrapper->QueryInterface(kUnknownAlias, &alias) == S_OK && alias == &aliased.alias);
    assert(wrapper->ReadObserved(0, output, sizeof(output)) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0);
    assert(aliased.refs == 1 && !aliased.destroyed);
    assert(aliased.alias.Release() == 0 && aliased.destroyed);

    FakeVertex failed_lock(&device);
    wrapper = VertexBufferWrapper::Adopt(&failed_lock, failed_lock.bytes.size(), Reader, &reader);
    Fill(wrapper, 0, data, sizeof(data)); failed_lock.lock_result = E_FAIL;
    BYTE* mapped = nullptr;
    assert(wrapper->Lock(0, sizeof(data), &mapped, 0) == E_FAIL);
    assert(wrapper->ReadObserved(0, output, sizeof(output)) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0);

    FakeVertex failed_unlock(&device);
    wrapper = VertexBufferWrapper::Adopt(&failed_unlock, failed_unlock.bytes.size(), Reader, &reader);
    assert(wrapper->Lock(0, sizeof(data), &mapped, 0) == S_OK);
    std::memcpy(mapped, data, sizeof(data)); failed_unlock.unlock_result = E_FAIL;
    assert(wrapper->Unlock() == E_FAIL);
    assert(wrapper->ReadObserved(0, output, sizeof(output)) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0);

    FakeVertex partial(&device);
    wrapper = VertexBufferWrapper::Adopt(&partial, partial.bytes.size(), Reader, &reader);
    assert(wrapper->Lock(0, sizeof(data), &mapped, 0) == S_OK);
    std::memcpy(mapped, data, sizeof(data)); reader.partial = true;
    assert(wrapper->Unlock() == S_OK); reader.partial = false;
    assert(wrapper->ReadObserved(0, output, sizeof(output)) == RegistryResult::UnknownData);
    assert(wrapper->Release() == 0);

    FakeVertex bad_pointer(&device);
    wrapper = VertexBufferWrapper::Adopt(&bad_pointer, bad_pointer.bytes.size(), Reader, &reader);
    assert(wrapper);
    Fill(wrapper, 0, data, sizeof(data));
    reader.fail = true;
    assert(wrapper->Lock(0, sizeof(data), &mapped, 0) == S_OK);  // pointer snapshot fails
    reader.fail = false;
    assert(wrapper->Unlock() == S_OK);
    assert(wrapper->ReadObserved(0, output, sizeof(output)) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0);

    FakeIndex first_index(&device), different_index(&device);
    auto* index_wrapper = IndexBufferWrapper::Adopt(&first_index, first_index.bytes.size(), Reader, &reader);
    assert(index_wrapper);
    Fill(index_wrapper, 0, data, sizeof(data));
    assert(device.SetIndices(&different_index, 3) == S_OK);
    IDirect3DIndexBuffer8* raw_index = nullptr; UINT index_base = 0; matched = true;
    assert(GetWrappedIndices(&device, index_wrapper, &raw_index, &index_base, matched) == S_OK);
    assert(!matched && raw_index == &different_index && index_base == 3);
    assert(index_wrapper->ReadObserved(0, output, sizeof(output)) == RegistryResult::Unavailable);
    raw_index->Release(); assert(device.SetIndices(nullptr, 0) == S_OK);
    assert(index_wrapper->Release() == 0 && first_index.refs == 0);
    assert(different_index.Release() == 0);

    // The fixture declares an over-budget logical buffer. Even when its fake
    // driver accepts a small write, shadow allocation must remain unavailable.
    FakeVertex over_budget(&device);
    wrapper = VertexBufferWrapper::Adopt(&over_budget, 512u * 1024u, Reader, &reader);
    assert(wrapper);
    Fill(wrapper, 0, data, sizeof(data));
    assert(wrapper->ReadObserved(0, output, sizeof(output)) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0);
}

void LockModesAndProcessVertices() {
    FakeDevice device;
    FakeVertex raw(&device);
    ReaderContext reader;
    auto* wrapper = VertexBufferWrapper::Adopt(&raw, raw.bytes.size(), Reader, &reader);
    const BYTE original[]{1, 2, 3, 4, 5, 6};
    BYTE result[6]{}, *mapped = nullptr;
    raw.lock_callback = [&] { assert(wrapper->ReadObserved(0, result, 1) == RegistryResult::Busy); };
    raw.unlock_callback = [&] { assert(wrapper->ReadObserved(0, result, 1) == RegistryResult::Busy); };
    SetLastError(700);
    assert(wrapper->Lock(0, sizeof(original), &mapped, 0) == S_OK);
    assert(raw.lock_input_error == 700 && GetLastError() == 850);
    std::memcpy(mapped, original, sizeof(original));
    SetLastError(701);
    assert(wrapper->Unlock() == S_OK);
    assert(raw.unlock_input_error == 701 && GetLastError() == 860);
    raw.lock_callback = {}; raw.unlock_callback = {};
    assert(wrapper->ReadObserved(0, result, sizeof(result)) == RegistryResult::Ok &&
           std::memcmp(result, original, sizeof(result)) == 0);

    const unsigned before_readonly = reader.calls;
    assert(wrapper->Lock(2, 2, &mapped, D3DLOCK_READONLY) == S_OK);
    assert(wrapper->Unlock() == S_OK && reader.calls == before_readonly + 1);  // pointer only
    assert(wrapper->ReadObserved(0, result, sizeof(result)) == RegistryResult::Ok &&
           std::memcmp(result, original, sizeof(result)) == 0);

    assert(wrapper->Lock(2, 2, &mapped, D3DLOCK_NOOVERWRITE) == S_OK);
    mapped[0] = 30; mapped[1] = 40;
    assert(wrapper->Unlock() == S_OK && raw.lock_flags == D3DLOCK_NOOVERWRITE);
    const BYTE patched[]{1, 2, 30, 40, 5, 6};
    assert(wrapper->ReadObserved(0, result, sizeof(result)) == RegistryResult::Ok &&
           std::memcmp(result, patched, sizeof(result)) == 0);

    const BYTE replacement[]{42, 43};
    assert(wrapper->Lock(2, 2, &mapped, D3DLOCK_DISCARD) == S_OK);
    std::memcpy(mapped, replacement, sizeof(replacement));
    assert(wrapper->Unlock() == S_OK);
    assert(wrapper->ReadObserved(0, result, 1) == RegistryResult::UnknownData);
    assert(wrapper->ReadObserved(2, result, 2) == RegistryResult::Ok &&
           std::memcmp(result, replacement, sizeof(replacement)) == 0);

    device.process_result = E_FAIL;
    SetLastError(710);
    assert(ProcessVerticesWrapped(&device, 1, 2, 3, wrapper, 4) == E_FAIL);
    assert(device.process_calls == 1 && device.process_target == &raw && device.process_input_error == 710 &&
           GetLastError() == 840);
    assert(wrapper->ReadObserved(2, result, 2) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0);
}

void ConcurrentLocks() {
    FakeDevice device;
    FakeVertex raw(&device);
    ReaderContext reader;
    auto* wrapper = VertexBufferWrapper::Adopt(&raw, raw.bytes.size(), Reader, &reader);
    const BYTE data[]{1, 2, 3};
    BYTE output[3]{}, *mapped = nullptr;
    Fill(wrapper, 0, data, sizeof(data));
    raw.block_lock = true;
    std::thread first([&] { BYTE* ptr = nullptr; assert(wrapper->Lock(0, 2, &ptr, 0) == S_OK); });
    while (!raw.lock_entered.load()) std::this_thread::yield();
    assert(wrapper->ReadObserved(0, output, 1) == RegistryResult::Busy);
    assert(wrapper->Lock(4, 2, &mapped, 0) == S_OK);
    raw.finish_lock = true;
    first.join();
    assert(wrapper->ReadObserved(0, output, 1) == RegistryResult::Unavailable);
    assert(wrapper->Unlock() == S_OK);
    assert(wrapper->Release() == 0);

    FakeVertex raw_unlock(&device);
    wrapper = VertexBufferWrapper::Adopt(&raw_unlock, raw_unlock.bytes.size(), Reader, &reader);
    Fill(wrapper, 0, data, sizeof(data));
    assert(wrapper->Lock(0, sizeof(data), &mapped, 0) == S_OK);
    std::memcpy(mapped, data, sizeof(data));
    raw_unlock.block_unlock = true;
    std::thread unlock([&] { assert(wrapper->Unlock() == S_OK); });
    while (!raw_unlock.unlock_entered.load()) std::this_thread::yield();
    assert(wrapper->ReadObserved(0, output, 1) == RegistryResult::Busy);
    assert(wrapper->Lock(4, 2, &mapped, 0) == S_OK);
    raw_unlock.finish_unlock = true;
    unlock.join();
    assert(wrapper->ReadObserved(0, output, 1) == RegistryResult::Unavailable);
    assert(wrapper->Unlock() == S_OK);
    assert(wrapper->Release() == 0);
}

void BoundDeviceOwnsReal() {
    FakeDevice device;
    FakeVertex vertex(&device);
    FakeIndex index(&device);
    ReaderContext reader;
    auto* vb = VertexBufferWrapper::Adopt(&vertex, vertex.bytes.size(), Reader, &reader);
    auto* ib = IndexBufferWrapper::Adopt(&index, index.bytes.size(), Reader, &reader);
    assert(vb && ib);
    assert(device.SetStreamSource(0, vb->Real(), 20) == S_OK);
    assert(device.SetIndices(ib->Real(), 1) == S_OK);
    assert(vertex.refs == 2 && index.refs == 2);
    assert(vb->Release() == 0 && ib->Release() == 0);
    assert(vertex.refs == 1 && index.refs == 1 && !vertex.destroyed && !index.destroyed);

    IDirect3DVertexBuffer8* vertex_out = nullptr;
    IDirect3DIndexBuffer8* index_out = nullptr;
    UINT stride = 0, base = 0; bool matched = true;
    assert(GetWrappedStream(&device, 0, nullptr, &vertex_out, &stride, matched) == S_OK);
    assert(!matched && vertex_out == &vertex && stride == 20 && vertex.refs == 2);
    matched = true;
    assert(GetWrappedIndices(&device, nullptr, &index_out, &base, matched) == S_OK);
    assert(!matched && index_out == &index && base == 1 && index.refs == 2);
    assert(vertex_out->Release() == 1 && index_out->Release() == 1);
    assert(device.SetStreamSource(0, nullptr, 0) == S_OK && device.SetIndices(nullptr, 0) == S_OK);
    assert(vertex.refs == 0 && index.refs == 0 && vertex.destroyed && index.destroyed && device.refs == 1);
}
}  // namespace

int main() {
    VertexForwarding();
    IndexForwarding();
    AliasesAndFailures();
    LockModesAndProcessVertices();
    ConcurrentLocks();
    BoundDeviceOwnsReal();
}
