#include "../src/buffer_proxy.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <thread>

using namespace t3render::buffers;
using t3render::ir::RegistryResult;
namespace {
constexpr IID kAlias{99};
struct Reader {
    bool fail = false, partial = false;
    unsigned calls = 0;
    static bool Copy(void* context, const void* source, void* out, std::size_t count) {
        auto& reader = *static_cast<Reader*>(context);
        ++reader.calls;
        SetLastError(999);
        if (reader.fail) return false;
        std::memcpy(out, source, reader.partial ? count - 1 : count);
        return !reader.partial;
    }
};

template<class Interface, class Desc, D3DRESOURCETYPE Type>
struct FakeBuffer final : Interface {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> destroyed{false};
    HRESULT lock_result = S_OK, unlock_result = S_OK, query_result = S_OK;
    bool alias_supported = false, null_map = false;
    std::array<BYTE, 8> bytes{};
    unsigned locks = 0, unlocks = 0, queries = 0;
    std::atomic<unsigned> releases{0};
    std::function<void()> on_query, on_release, on_lock, on_unlock;
    HRESULT QueryInterface(REFIID iid, void** out) override {
        ++queries;
        if (on_query) on_query();
        if (!out) return E_POINTER;
        *out = nullptr;
        if (FAILED(query_result)) return query_result;
        const bool type = Type == D3DRTYPE_VERTEXBUFFER ? IsEqualIID(iid, IID_IDirect3DVertexBuffer8)
                                                       : IsEqualIID(iid, IID_IDirect3DIndexBuffer8);
        if (!type && !IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3DResource8) &&
            !(alias_supported && IsEqualIID(iid, kAlias))) return E_NOINTERFACE;
        AddRef(); *out = static_cast<Interface*>(this); SetLastError(810); return S_OK;
    }
    ULONG AddRef() override { assert(refs.load() && !destroyed.load()); return ++refs; }
    ULONG Release() override {
        ++releases; const auto prior = refs.fetch_sub(1); assert(prior); const auto left = prior - 1;
        if (on_release) on_release();
        if (!left) destroyed = true;
        SetLastError(820); return left;
    }
    HRESULT GetDevice(IDirect3DDevice8** out) override { if (out) *out = nullptr; SetLastError(830); return S_OK; }
    HRESULT SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return S_OK; }
    HRESULT GetPrivateData(REFGUID, void*, DWORD*) override { return S_OK; }
    HRESULT FreePrivateData(REFGUID) override { return S_OK; }
    DWORD SetPriority(DWORD priority) override { return priority; }
    DWORD GetPriority() override { return 0; }
    void PreLoad() override {}
    D3DRESOURCETYPE GetType() override { return Type; }
    HRESULT Lock(UINT offset, UINT length, BYTE** out, DWORD) override {
        ++locks; if (on_lock) on_lock(); SetLastError(840);
        if (FAILED(lock_result)) return lock_result;
        if (!out || offset > bytes.size() || (length && length > bytes.size() - offset)) return D3DERR_INVALIDCALL;
        *out = null_map ? nullptr : bytes.data() + offset; return S_OK;
    }
    HRESULT Unlock() override { ++unlocks; if (on_unlock) on_unlock(); SetLastError(850); return unlock_result; }
    HRESULT GetDesc(Desc* out) override { if (!out) return E_POINTER; *out = {}; out->Size = bytes.size(); return S_OK; }
};
using Vertex = FakeBuffer<IDirect3DVertexBuffer8, D3DVERTEXBUFFER_DESC, D3DRTYPE_VERTEXBUFFER>;
using Index = FakeBuffer<IDirect3DIndexBuffer8, D3DINDEXBUFFER_DESC, D3DRTYPE_INDEXBUFFER>;
struct VertexAlias final : IDirect3DVertexBuffer8 {
    explicit VertexAlias(Vertex& target) : target(target) {}
    Vertex& target;
    HRESULT QueryInterface(REFIID iid, void** out) override { return target.QueryInterface(iid, out); }
    ULONG AddRef() override { return target.AddRef(); }
    ULONG Release() override { return target.Release(); }
    HRESULT GetDevice(IDirect3DDevice8** out) override { return target.GetDevice(out); }
    HRESULT SetPrivateData(REFGUID g, const void* d, DWORD n, DWORD f) override { return target.SetPrivateData(g, d, n, f); }
    HRESULT GetPrivateData(REFGUID g, void* d, DWORD* n) override { return target.GetPrivateData(g, d, n); }
    HRESULT FreePrivateData(REFGUID g) override { return target.FreePrivateData(g); }
    DWORD SetPriority(DWORD n) override { return target.SetPriority(n); }
    DWORD GetPriority() override { return target.GetPriority(); }
    void PreLoad() override { target.PreLoad(); }
    D3DRESOURCETYPE GetType() override { return target.GetType(); }
    HRESULT Lock(UINT o, UINT n, BYTE** p, DWORD f) override { return target.Lock(o, n, p, f); }
    HRESULT Unlock() override { return target.Unlock(); }
    HRESULT GetDesc(D3DVERTEXBUFFER_DESC* d) override { return target.GetDesc(d); }
};
IDirect3DDevice8* Owner(unsigned n = 1) { return reinterpret_cast<IDirect3DDevice8*>(static_cast<std::uintptr_t>(n)); }

template<class Buffer>
void Fill(Buffer* buffer, UINT offset, const BYTE* source, UINT length, DWORD flags = 0) {
    BYTE* mapped = nullptr;
    SetLastError(100);
    assert(buffer->Lock(offset, length, &mapped, flags) == S_OK && mapped && GetLastError() == 840);
    std::memcpy(mapped, source, length);
    SetLastError(101);
    assert(buffer->Unlock() == S_OK && GetLastError() == 850);
}
void LifetimeAndIdentity() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    assert(proxy);
    Vertex vb;
    const auto vtable = *reinterpret_cast<void**>(&vb);
    // A canonical QI/Release may re-enter the manager. No COM call is under its mutex.
    vb.on_query = [&] { BYTE out{}; assert(proxy->ReadVertex(Owner(), &vb, 0, &out, 1) == RegistryResult::Unavailable); };
    SetLastError(11);
    auto* wrapper = proxy->AdoptVertex(Owner(), &vb, vb.bytes.size(), true);
    assert(wrapper && wrapper != &vb && GetLastError() == 11 && vb.refs == 1);
    assert(*reinterpret_cast<void**>(&vb) == vtable);
    vb.on_query = {};
    void* first = nullptr; void* second = nullptr;
    assert(wrapper->QueryInterface(IID_IUnknown, &first) == S_OK && first == wrapper);
    assert(wrapper->QueryInterface(IID_IDirect3DResource8, &second) == S_OK && second == first);
    static_cast<IUnknown*>(first)->Release(); static_cast<IUnknown*>(second)->Release();
    const BYTE value[]{4, 5, 6, 7}; BYTE observed[4]{};
    vb.on_lock = [&] { assert(proxy->ReadVertex(Owner(), &vb, 2, observed, 1) == RegistryResult::Busy); };
    vb.on_unlock = vb.on_lock;
    Fill(wrapper, 2, value, sizeof(value));
    vb.on_lock = {}; vb.on_unlock = {};
    assert(proxy->ReadVertex(Owner(), &vb, 2, observed, sizeof(observed)) == RegistryResult::Ok);
    assert(std::memcmp(observed, value, sizeof(value)) == 0);
    assert(proxy->ReadVertex(Owner(), &vb, 1, observed, sizeof(observed)) == RegistryResult::UnknownData);

    vb.AddRef(); // A native GetStreamSource +1 while the wrapper is alive.
    SetLastError(12);
    auto* same = proxy->AdoptVertex(Owner(), &vb, 0, false);
    assert(same == wrapper && vb.refs == 1 && GetLastError() == 12);
    assert(same->Release() == 1);
    SetLastError(13);
    {
        auto pin = proxy->UnwrapVertex(Owner(), wrapper);
        assert(pin.native() == &vb && GetLastError() == 13);
        assert(wrapper->Release() == 1 && !vb.destroyed);
        auto moved = std::move(pin);
        assert(!pin.native() && moved.native() == &vb);
    }
    assert(vb.destroyed && vb.refs == 0 && GetLastError() == 13);
    assert(proxy->ReadVertex(Owner(), &vb, 2, observed, 1) == RegistryResult::UnknownResource);
}
void SharedBudgetAndGetAfterRelease() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 18, 4); // two 8-byte shadows use 18 bytes
    Vertex vb; Index ib;
    auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
    auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
    BYTE values[]{1, 2, 3, 4, 5, 6, 7, 8}, result[8]{};
    Fill(v, 0, values, 8);
    assert(proxy->ReadVertex(Owner(), &vb, 0, result, 8) == RegistryResult::UnknownData);
    assert(i->Release() == 0 && ib.destroyed);
    Fill(v, 0, values, 8);
    assert(proxy->ReadVertex(Owner(), &vb, 0, result, 8) == RegistryResult::Ok);
    assert(std::memcmp(result, values, 8) == 0);
    vb.AddRef(); // Device binds and keeps native resource after wrapper final Release.
    assert(v->Release() == 0 && vb.refs == 1 && !vb.destroyed);
    vb.AddRef(); // device getter gives a native reference, but write history is unknown
    auto* raw = proxy->AdoptVertex(Owner(), &vb, 0, false);
    assert(raw == &vb && vb.refs == 2);
    assert(proxy->ReadVertex(Owner(), &vb, 0, result, 1) == RegistryResult::Unavailable);
    assert(raw->Release() == 1 && vb.Release() == 0 && vb.destroyed);
}
void EscapePoisonAndForwarding() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    Vertex vb; Index ib;
    auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
    auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
    const BYTE bytes[]{8, 7, 6}; BYTE result[3]{};
    Fill(v, 0, bytes, 3); Fill(i, 0, bytes, 3);
    assert(proxy->ReadIndex(Owner(), &ib, 0, result, 3) == RegistryResult::Ok);
    void* alias = nullptr;
    assert(v->QueryInterface(kAlias, &alias) == E_NOINTERFACE && !alias);
    assert(proxy->ReadIndex(Owner(), &ib, 0, result, 3) == RegistryResult::Ok);
    vb.alias_supported = true;
    SetLastError(40);
    assert(v->QueryInterface(kAlias, &alias) == S_OK && alias == &vb && GetLastError() == 810);
    assert(proxy->ReadIndex(Owner(), &ib, 0, result, 3) == RegistryResult::Unavailable);
    // Even after an escape, known getters return the existing wrapper identity.
    ib.AddRef();
    auto* same = proxy->AdoptIndex(Owner(), &ib, 0, false);
    assert(same == i && ib.refs == 1);
    same->Release();
    static_cast<IDirect3DVertexBuffer8*>(alias)->Release();
    assert(v->Release() == 0 && vb.destroyed);
    assert(i->Release() == 0 && ib.destroyed);
}
void DrawHazardsAndUnknownInput() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    Vertex vb; Index ib;
    auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
    auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
    BYTE data[]{1, 2, 3}, out[3]{};
    Fill(v, 0, data, 3); Fill(i, 0, data, 3);
    const auto draw = proxy->BeginDraw(Owner());
    assert(draw && proxy->ReadVertex(Owner(), &vb, 0, out, 3) == RegistryResult::Ok);
    BYTE* mapped = nullptr;
    assert(i->Lock(0, 3, &mapped, 0) == S_OK && mapped);
    assert(i->Unlock() == S_OK);
    assert(!proxy->EndDraw(Owner(), draw));
    assert(proxy->ReadVertex(Owner(), &vb, 0, out, 3) == RegistryResult::Unavailable);
    { auto pin = proxy->UnwrapVertex(Owner(), v); assert(pin.native() == &vb); }
    assert(v->Release() == 0 && i->Release() == 0);

    auto other = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    Vertex tracked, foreign;
    auto* wrapper = other->AdoptVertex(Owner(), &tracked, 8, true);
    Fill(wrapper, 0, data, 3);
    { auto null_pin = other->UnwrapVertex(Owner(), nullptr); assert(!null_pin.native()); }
    assert(other->ReadVertex(Owner(), &tracked, 0, out, 3) == RegistryResult::Ok);
    { auto raw_pin = other->UnwrapVertex(Owner(), &foreign); assert(raw_pin.native() == &foreign); }
    assert(other->ReadVertex(Owner(), &tracked, 0, out, 3) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0 && tracked.destroyed);
    assert(foreign.Release() == 0);
}
void CanonicalAliasAndConcurrency() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    Vertex raw; VertexAlias alias(raw);
    auto* wrapper = proxy->AdoptVertex(Owner(), &raw, 8, true);
    const BYTE data[]{4, 5, 6}; BYTE out[3]{};
    Fill(wrapper, 0, data, 3);
    raw.AddRef(); // GetStreamSource returns another native interface for same IUnknown.
    auto* same = proxy->AdoptVertex(Owner(), &alias, 0, false);
    assert(same == wrapper && raw.refs == 1);
    assert(proxy->ReadVertex(Owner(), &alias, 0, out, 3) == RegistryResult::UnknownResource);
    assert(proxy->ReadVertex(Owner(), &raw, 0, out, 3) == RegistryResult::Ok);
    assert(same->Release() == 1);
    raw.on_release = [&] { (void)proxy->ReadVertex(Owner(), &raw, 0, out, 1); };
    raw.AddRef();
    assert(proxy->AdoptVertex(Owner(), &alias, 0, false) == wrapper); // QI/Release may re-enter.
    assert(wrapper->Release() == 1);
    raw.on_release = {};
    assert(wrapper->Release() == 0 && raw.destroyed);

    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        auto manager = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex item;
        auto* observed = manager->AdoptVertex(Owner(), &item, 8, true);
        item.AddRef(); // native getter owns +1 before racing wrapper release
        std::atomic<bool> go{false};
        std::thread getter([&] {
            while (!go.load()) std::this_thread::yield();
            auto* result = manager->AdoptVertex(Owner(), &item, 0, false);
            assert(result);
            result->Release();
        });
        go = true;
        observed->Release();
        getter.join();
        assert(item.destroyed && item.refs == 0);
    }
}

void BindingMutationWindows() {
    Reader reader;
    const BYTE data[]{4, 5}; BYTE out[2]{};
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex raw;
        auto* buffer = proxy->AdoptVertex(Owner(), &raw, 8, true);
        Fill(buffer, 0, data, 2);
        const auto mutation = proxy->BeginBindingMutation(Owner());
        assert(mutation);
        assert(proxy->BeginDraw(Owner()) == 0); // Set started first; GPU draw overlaps.
        proxy->EndBindingMutation(Owner(), mutation);
        assert(proxy->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Unavailable);
        assert(buffer->Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex raw;
        auto* buffer = proxy->AdoptVertex(Owner(), &raw, 8, true);
        Fill(buffer, 0, data, 2);
        const auto draw = proxy->BeginDraw(Owner());
        assert(draw && proxy->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Ok);
        assert(proxy->BeginBindingMutation(Owner()) == 0); // Set began after the snapshot.
        assert(!proxy->EndDraw(Owner(), draw));
        assert(proxy->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Unavailable);
        assert(buffer->Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex raw;
        auto* buffer = proxy->AdoptVertex(Owner(), &raw, 8, true);
        Fill(buffer, 0, data, 2);
        const auto mutation = proxy->BeginBindingMutation(Owner());
        assert(mutation);
        BYTE* mapped = nullptr;
        assert(buffer->Lock(0, 2, &mapped, 0) == S_OK && mapped);
        assert(buffer->Unlock() == S_OK);
        proxy->EndBindingMutation(Owner(), mutation);
        assert(proxy->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Unavailable);
        assert(buffer->Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex raw;
        auto* buffer = proxy->AdoptVertex(Owner(), &raw, 8, true);
        Fill(buffer, 0, data, 2);
        const auto draw = proxy->BeginDraw(Owner());
        assert(draw && !proxy->EndDraw(Owner(), draw + 1));
        assert(!proxy->EndDraw(Owner(), draw));
        assert(proxy->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Unavailable);
        assert(buffer->Release() == 0);
    }
}

void OwnerIsolationAndFlagModes() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    Vertex first, second;
    auto* a = proxy->AdoptVertex(Owner(1), &first, 8, true);
    auto* b = proxy->AdoptVertex(Owner(2), &second, 8, true);
    BYTE data[]{1, 2, 3, 4}, out[4]{}, *mapped = nullptr;
    Fill(a, 0, data, 4); Fill(b, 0, data, 4);
    assert(a->Lock(1, 2, &mapped, D3DLOCK_READONLY) == S_OK && mapped);
    assert(a->Unlock() == S_OK);
    assert(proxy->ReadVertex(Owner(1), &first, 0, out, 4) == RegistryResult::Ok);
    const BYTE patch[]{9, 8};
    Fill(a, 1, patch, 2, D3DLOCK_NOOVERWRITE);
    assert(proxy->ReadVertex(Owner(1), &first, 0, out, 4) == RegistryResult::Ok);
    const BYTE patched[]{1, 9, 8, 4}; assert(std::memcmp(out, patched, 4) == 0);
    Fill(a, 1, patch, 2, D3DLOCK_DISCARD);
    assert(proxy->ReadVertex(Owner(1), &first, 0, out, 1) == RegistryResult::UnknownData);
    assert(proxy->ReadVertex(Owner(1), &first, 1, out, 2) == RegistryResult::Ok);
    IDirect3DDevice8* escaped = nullptr;
    assert(a->GetDevice(&escaped) == S_OK); // raw device can escape; entire first owner is unsafe
    assert(proxy->ReadVertex(Owner(1), &first, 1, out, 2) == RegistryResult::Unavailable);
    assert(proxy->ReadVertex(Owner(2), &second, 0, out, 4) == RegistryResult::Ok);
    assert(a->Release() == 0 && b->Release() == 0);
}
void UnlockFailureAndCopyFailures() {
    Reader reader;
    const BYTE data[]{1, 2, 3}; BYTE out[3]{}, *mapped = nullptr;
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex raw;
        auto* wrapper = proxy->AdoptVertex(Owner(), &raw, 8, true);
        assert(wrapper->Lock(0, 3, &mapped, 0) == S_OK && mapped);
        std::memcpy(mapped, data, 3);
        raw.unlock_result = E_FAIL;
        assert(wrapper->Unlock() == E_FAIL && GetLastError() == 850);
        assert(proxy->ReadVertex(Owner(), &raw, 0, out, 3) == RegistryResult::Unavailable);
        assert(wrapper->Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex raw;
        auto* wrapper = proxy->AdoptVertex(Owner(), &raw, 8, true);
        assert(wrapper->Lock(0, 3, &mapped, 0) == S_OK && mapped);
        std::memcpy(mapped, data, 3); reader.partial = true;
        assert(wrapper->Unlock() == S_OK); reader.partial = false;
        assert(proxy->ReadVertex(Owner(), &raw, 0, out, 3) == RegistryResult::UnknownData);
        assert(wrapper->Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex raw;
        auto* wrapper = proxy->AdoptVertex(Owner(), &raw, 8, true);
        reader.fail = true;
        assert(wrapper->Lock(0, 3, &mapped, 0) == S_OK); // output pointer copy fails safely
        reader.fail = false;
        assert(wrapper->Unlock() == S_OK);
        assert(proxy->ReadVertex(Owner(), &raw, 0, out, 3) == RegistryResult::Unavailable);
        assert(wrapper->Release() == 0);
    }
}

void CapacityAndUnknownBuffersForward() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 1);
    Vertex first; Index second;
    auto* v = proxy->AdoptVertex(Owner(), &first, 8, true);
    assert(v && v != &first);
    const BYTE data[]{1, 2}; BYTE out[2]{};
    Fill(v, 0, data, 2);
    assert(proxy->ReadVertex(Owner(), &first, 0, out, 2) == RegistryResult::Ok);
    // Capacity failure must not fail native Create or consume its reference.
    auto* i = proxy->AdoptIndex(Owner(), &second, 8, true);
    assert(i == &second && second.refs == 1);
    assert(proxy->ReadVertex(Owner(), &first, 0, out, 2) == RegistryResult::Unavailable);
    { auto pin = proxy->UnwrapVertex(Owner(), v); assert(pin.native() == &first); }
    assert(v->Release() == 0 && first.destroyed);
    assert(i->Release() == 0 && second.destroyed);

    auto no_budget = BufferProxy::Create(Reader::Copy, &reader, 0, 4);
    Vertex raw;
    auto* wrapped = no_budget->AdoptVertex(Owner(), &raw, 8, true);
    assert(wrapped && wrapped != &raw);
    Fill(wrapped, 0, data, 2);
    assert(no_budget->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Unavailable);
    assert(wrapped->Release() == 0 && raw.destroyed);
}

void StopObservationKeepsForwarding() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    Vertex raw;
    auto* wrapper = proxy->AdoptVertex(Owner(), &raw, 8, true);
    BYTE bytes[]{1, 2}, out[2]{};
    Fill(wrapper, 0, bytes, 2);
    assert(proxy->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Ok);
    SetLastError(51);
    proxy->StopObservation();
    proxy->StopObservation(); // idempotent after all shadows are already unknown
    assert(GetLastError() == 51);
    assert(proxy->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Unavailable);
    raw.AddRef();
    auto* same = proxy->AdoptVertex(Owner(), &raw, 0, false);
    assert(same == wrapper && raw.refs == 1);
    assert(same->Release() == 1);
    { auto pin = proxy->UnwrapVertex(Owner(), wrapper); assert(pin.native() == &raw); }
    Fill(wrapper, 0, bytes, 2); // forwards normally despite stopped observation
    assert(proxy->ReadVertex(Owner(), &raw, 0, out, 2) == RegistryResult::Unavailable);
    assert(wrapper->Release() == 0 && raw.destroyed);
}

void ResetProcessAndFailures() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    Vertex vb; Index ib;
    auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
    auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
    BYTE data[]{1, 2}, out[2]{};
    Fill(v, 0, data, 2); Fill(i, 0, data, 2);
    proxy->InvalidateVertex(Owner(), v);
    assert(proxy->ReadVertex(Owner(), &vb, 0, out, 2) == RegistryResult::Unavailable);
    assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Ok);
    proxy->InvalidateDevice(Owner());
    assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Unavailable);
    Vertex fresh;
    assert(proxy->AdoptVertex(Owner(), &fresh, 8, true) == &fresh);
    assert(fresh.Release() == 0);
    assert(v->Release() == 0 && i->Release() == 0);

    auto failure = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    Vertex bad;
    auto* b = failure->AdoptVertex(Owner(), &bad, 8, true);
    BYTE* mapped = nullptr;
    bad.lock_result = E_FAIL;
    assert(b->Lock(0, 2, &mapped, 0) == E_FAIL);
    assert(failure->ReadVertex(Owner(), &bad, 0, out, 2) == RegistryResult::Unavailable);
    assert(b->Release() == 0 && bad.destroyed);
}
}  // namespace
int main() {
    LifetimeAndIdentity();
    SharedBudgetAndGetAfterRelease();
    EscapePoisonAndForwarding();
    DrawHazardsAndUnknownInput();
    CanonicalAliasAndConcurrency();
    BindingMutationWindows();
    OwnerIsolationAndFlagModes();
    UnlockFailureAndCopyFailures();
    CapacityAndUnknownBuffersForward();
    StopObservationKeepsForwarding();
    ResetProcessAndFailures();
}
