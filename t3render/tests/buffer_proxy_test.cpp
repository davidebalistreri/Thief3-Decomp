#include "../src/buffer_proxy.hpp"
#include "../src/private_blob.hpp"
#include "../src/private_marker.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
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
    HRESULT device_result = S_OK;
    IDirect3DDevice8* device = nullptr;
    bool alias_supported = false, null_map = false;
    std::array<BYTE, 8> bytes{};
    unsigned locks = 0, unlocks = 0, queries = 0;
    std::atomic<unsigned> releases{0};
    std::function<void()> on_query, on_release, on_lock, on_unlock;
    std::function<HRESULT(REFGUID, const void*, DWORD, DWORD)> on_private_set;
    std::function<HRESULT(REFGUID, void*, DWORD*)> on_private_get;
    std::function<HRESULT(REFGUID)> on_private_free;
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
    HRESULT GetDevice(IDirect3DDevice8** out) override {
        if (out && SUCCEEDED(device_result)) { *out = device; if (device) device->AddRef(); }
        SetLastError(830); return device_result;
    }
    HRESULT SetPrivateData(REFGUID key, const void* data, DWORD size, DWORD flags) override {
        return on_private_set ? on_private_set(key, data, size, flags) : S_OK;
    }
    HRESULT GetPrivateData(REFGUID key, void* data, DWORD* size) override {
        return on_private_get ? on_private_get(key, data, size) : S_OK;
    }
    HRESULT FreePrivateData(REFGUID key) override { return on_private_free ? on_private_free(key) : S_OK; }
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
struct FakeDevice final : IDirect3DDevice8 {
    std::atomic<ULONG> refs{1};
    std::atomic<bool> destroyed{false};
    bool fail_identity = false;
    std::function<void()> on_final_release;
    HRESULT QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (fail_identity || (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IDirect3DDevice8)))
            return E_NOINTERFACE;
        AddRef(); *output = static_cast<IDirect3DDevice8*>(this); SetLastError(831); return S_OK;
    }
    ULONG AddRef() override { assert(refs && !destroyed); SetLastError(832); return ++refs; }
    ULONG Release() override {
        const auto previous = refs.fetch_sub(1); assert(previous);
        if (previous == 1) { destroyed = true; if (on_final_release) on_final_release(); }
        SetLastError(833); return previous - 1;
    }
    HRESULT SetStreamSource(UINT, IDirect3DVertexBuffer8*, UINT) override { return E_NOTIMPL; }
    HRESULT GetStreamSource(UINT, IDirect3DVertexBuffer8**, UINT*) override { return E_NOTIMPL; }
    HRESULT SetIndices(IDirect3DIndexBuffer8*, UINT) override { return E_NOTIMPL; }
    HRESULT GetIndices(IDirect3DIndexBuffer8**, UINT*) override { return E_NOTIMPL; }
    HRESULT ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD) override { return E_NOTIMPL; }
};
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

// A draw or binding mutation must cover the whole native lock window, even
// while Lock/Unlock is still executing. Exercise both buffer types and orderings.
void NativeLockWindows() {
    const BYTE data[]{4, 5};
    for (bool indexed : {false, true}) for (bool mutation : {false, true})
        for (int phase = 0; phase != 3; ++phase) {
            Reader reader;
            auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
            Vertex vb; Index ib;
            auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
            auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
            Fill(v, 0, data, 2); Fill(i, 0, data, 2);
            BYTE out[2]{}, *mapped = nullptr;
            auto rejected = [&] {
                const auto token = mutation ? proxy->BeginBindingMutation(Owner()) : proxy->BeginDraw(Owner());
                assert(token == 0);
                assert(proxy->ReadVertex(Owner(), &vb, 0, out, 2) == RegistryResult::Unavailable);
                assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Unavailable);
            };
            auto exercise = [&](auto* buffer, auto& raw) {
                if (phase == 0) raw.on_lock = rejected;
                assert(buffer->Lock(0, 2, &mapped, 0) == S_OK && mapped && GetLastError() == 840);
                if (phase == 1) rejected();
                if (phase == 2) raw.on_unlock = rejected;
                assert(buffer->Unlock() == S_OK && GetLastError() == 850);
                raw.on_lock = {}; raw.on_unlock = {};
                assert(proxy->BeginDraw(Owner()) == 0);
            };
            if (indexed) exercise(i, ib); else exercise(v, vb);
            assert(v->Release() == 0 && i->Release() == 0);
        }
}

void ConcurrentNativeLockWindows() {
    const BYTE data[]{4, 5};
    for (bool pending_unlock : {false, true}) for (bool mutation : {false, true}) {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb; Index ib;
        auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
        auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
        Fill(i, 0, data, 2);
        std::atomic<bool> entered{false}, proceed{false};
        auto wait = [&] {
            entered.store(true, std::memory_order_release);
            while (!proceed.load(std::memory_order_acquire)) std::this_thread::yield();
        };
        if (pending_unlock) vb.on_unlock = wait; else vb.on_lock = wait;
        std::thread worker([&] {
            BYTE* mapped = nullptr;
            assert(v->Lock(0, 2, &mapped, 0) == S_OK && mapped && GetLastError() == 840);
            assert(v->Unlock() == S_OK && GetLastError() == 850);
        });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto token = mutation ? proxy->BeginBindingMutation(Owner()) : proxy->BeginDraw(Owner());
        assert(token == 0);
        proceed.store(true, std::memory_order_release);
        worker.join();
        BYTE out[2]{};
        assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Unavailable);
        assert(v->Release() == 0 && i->Release() == 0);
    }
}

void UnknownNativeLocks() {
    const BYTE data[]{6, 7}; BYTE out[2]{}, *mapped = nullptr;
    // A successful native lock with an unreadable result pointer is still
    // open, even though the shadow is abandoned. The other buffer must not
    // produce a seemingly stable observation.
    {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb; Index ib;
        auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
        auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
        Fill(v, 0, data, 2); Fill(i, 0, data, 2);
        reader.fail = true;
        assert(v->Lock(0, 2, &mapped, 0) == S_OK && mapped && GetLastError() == 840);
        reader.fail = false;
        assert(proxy->BeginDraw(Owner()) == 0);
        assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Unavailable);
        assert(v->Unlock() == S_OK && GetLastError() == 850);
        assert(v->Release() == 0 && i->Release() == 0);
    }
    // Unsupported flags/ranges still reach the driver, which may accept them.
    for (bool invalid_range : {false, true}) {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb; Index ib;
        auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
        auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
        Fill(i, 0, data, 2);
        assert(v->Lock(invalid_range ? 1 : 0, invalid_range ? 0 : 2, &mapped,
                       invalid_range ? 0 : 0x40000000u) == S_OK && mapped);
        assert(proxy->BeginDraw(Owner()) == 0);
        assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Unavailable);
        assert(v->Unlock() == S_OK);
        assert(v->Release() == 0 && i->Release() == 0);
    }
    // Unlock failure does not establish whether the driver closed the lock.
    {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb; Index ib;
        auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
        auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
        Fill(i, 0, data, 2);
        assert(v->Lock(0, 2, &mapped, 0) == S_OK);
        vb.unlock_result = E_FAIL;
        assert(v->Unlock() == E_FAIL && GetLastError() == 850);
        assert(proxy->BeginDraw(Owner()) == 0);
        assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Unavailable);
        assert(v->Release() == 0 && i->Release() == 0);
    }
    // A binding may retain the native object after the final wrapper Release.
    {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb; Index ib;
        auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
        auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
        Fill(i, 0, data, 2);
        vb.AddRef();
        assert(v->Lock(0, 2, &mapped, 0) == S_OK);
        assert(v->Release() == 0 && !vb.destroyed);
        assert(proxy->BeginDraw(Owner()) == 0);
        assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Unavailable);
        assert(vb.Unlock() == S_OK && vb.Release() == 0);
        assert(i->Release() == 0);
    }
    // GPU-side invalidation must not silently erase an open native lock.
    {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb; Index ib;
        auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
        auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
        Fill(i, 0, data, 2);
        assert(v->Lock(0, 2, &mapped, 0) == S_OK);
        proxy->InvalidateVertex(Owner(), v);
        assert(proxy->BeginDraw(Owner()) == 0);
        assert(proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Unavailable);
        assert(v->Unlock() == S_OK);
        assert(v->Release() == 0 && i->Release() == 0);
    }
}

void LockCountsAndInvalidation() {
    const BYTE data[]{1, 2}; BYTE out[2]{}, *a = nullptr, *b = nullptr;
    // Counts are per-device and include READONLY and NOOVERWRITE locks. One
    // successful Unlock must not hide a second still-open resource.
    for (bool leave_one_open : {false, true}) {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb, other; Index ib;
        auto* v = proxy->AdoptVertex(Owner(1), &vb, 8, true);
        auto* i = proxy->AdoptIndex(Owner(1), &ib, 8, true);
        auto* o = proxy->AdoptVertex(Owner(2), &other, 8, true);
        Fill(v, 0, data, 2); Fill(i, 0, data, 2); Fill(o, 0, data, 2);
        assert(v->Lock(0, 2, &a, D3DLOCK_READONLY) == S_OK);
        assert(i->Lock(0, 2, &b, D3DLOCK_NOOVERWRITE) == S_OK);
        assert(v->Unlock() == S_OK);
        const auto other_draw = proxy->BeginDraw(Owner(2));
        assert(other_draw && proxy->EndDraw(Owner(2), other_draw));
        if (leave_one_open) {
            assert(proxy->BeginDraw(Owner(1)) == 0);
            assert(i->Unlock() == S_OK);
            assert(proxy->ReadVertex(Owner(2), &other, 0, out, 2) == RegistryResult::Ok);
        } else {
            assert(i->Unlock() == S_OK);
            const auto draw = proxy->BeginDraw(Owner(1));
            assert(draw && proxy->EndDraw(Owner(1), draw));
        }
        assert(v->Release() == 0 && i->Release() == 0 && o->Release() == 0);
    }
    // A failed native Lock opens no window; do not poison unrelated buffers.
    {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb; Index ib;
        auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
        auto* i = proxy->AdoptIndex(Owner(), &ib, 8, true);
        Fill(i, 0, data, 2);
        vb.lock_result = E_FAIL;
        assert(v->Lock(0, 2, &a, 0) == E_FAIL && GetLastError() == 840);
        const auto draw = proxy->BeginDraw(Owner());
        assert(draw && proxy->ReadIndex(Owner(), &ib, 0, out, 2) == RegistryResult::Ok);
        assert(proxy->EndDraw(Owner(), draw));
        assert(v->Release() == 0 && i->Release() == 0);
    }
    // Invalidating observation during a pending native call makes its finish
    // stale; it must not underflow the count or prevent normal forwarding.
    for (bool stop : {false, true}) for (bool pending_unlock : {false, true}) {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        Vertex vb;
        auto* v = proxy->AdoptVertex(Owner(), &vb, 8, true);
        auto invalidate = [&] { if (stop) proxy->StopObservation(); else proxy->InvalidateDevice(Owner()); };
        if (!pending_unlock) vb.on_lock = invalidate;
        assert(v->Lock(0, 2, &a, 0) == S_OK && GetLastError() == 840);
        if (pending_unlock) vb.on_unlock = invalidate;
        assert(v->Unlock() == S_OK && GetLastError() == 850);
        assert(proxy->BeginDraw(Owner()) == 0);
        assert(v->Release() == 0);
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

void ClosedFacadeBufferBoundary() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    FakeDevice native_device, facade;
    Vertex vb; Index ib;
    vb.device = &native_device; ib.device = &native_device;
    auto* vertex = proxy->AdoptVertexForFacade(&native_device, &facade, &vb, 8, true);
    auto* index = proxy->AdoptIndexForFacade(&native_device, &facade, &ib, 8, true);
    assert(vertex && vertex != &vb && index && index != &ib && facade.refs == 3);
    const BYTE bytes[]{1, 4, 9}; BYTE observed[3]{};
    Fill(vertex, 0, bytes, 3); Fill(index, 0, bytes, 3);
    assert(proxy->ReadVertex(&native_device, &vb, 0, observed, 3) == RegistryResult::Ok);
    assert(std::memcmp(observed, bytes, 3) == 0);

    // The children keep the facade alive after its caller drops its reference.
    assert(facade.Release() == 2);
    for (IDirect3DResource8* resource : {static_cast<IDirect3DResource8*>(vertex),
                                         static_cast<IDirect3DResource8*>(index)}) {
        IDirect3DDevice8* returned = nullptr;
        SetLastError(17);
        assert(resource->GetDevice(&returned) == S_OK && returned == &facade && GetLastError() == 830);
        assert(native_device.refs == 1 && facade.refs == 3);
        void* identity = nullptr;
        assert(returned->QueryInterface(IID_IUnknown, &identity) == S_OK && identity == &facade);
        static_cast<IUnknown*>(identity)->Release();
        returned->Release();
        void* unknown = reinterpret_cast<void*>(1);
        vb.alias_supported = true;
        assert(resource->QueryInterface(kAlias, &unknown) == E_NOINTERFACE && !unknown);
        DWORD size = 0;
        assert(resource->SetPrivateData(kAlias, &facade, sizeof(facade), D3DSPD_IUNKNOWN) == E_NOTIMPL);
        assert(resource->GetPrivateData(kAlias, nullptr, &size) == E_NOTIMPL);
        assert(resource->FreePrivateData(kAlias) == E_NOTIMPL);
    }
    assert(proxy->ReadIndex(&native_device, &ib, 0, observed, 3) == RegistryResult::Ok);
    IUnknown* pinned_native = nullptr;
    bool vertex_kind = false;
    SetLastError(31);
    IUnknown* guard = proxy->PinPrivateBuffer(&native_device, &facade, vertex, &pinned_native, &vertex_kind);
    assert(guard == vertex && pinned_native == &vb && vertex_kind && GetLastError() == 31);
    assert(guard->Release() == 1);
    guard = proxy->PinPrivateBuffer(&native_device, &facade, index, &pinned_native, &vertex_kind);
    assert(guard == index && pinned_native == &ib && !vertex_kind);
    assert(guard->Release() == 1);
    pinned_native = reinterpret_cast<IUnknown*>(1);
    assert(!proxy->PinPrivateBuffer(&native_device, &facade, &vb, &pinned_native, &vertex_kind) && !pinned_native);
    FakeDevice other;
    assert(!proxy->PinPrivateBuffer(&native_device, &other, vertex, &pinned_native, &vertex_kind) && !pinned_native);
    assert(proxy->ReadVertex(&native_device, &vb, 0, observed, 3) == RegistryResult::Ok);
    vb.AddRef();
    auto* same = proxy->AdoptVertexForFacade(&native_device, &facade, &vb, 0, false);
    assert(same == vertex && vb.refs == 1);
    assert(same->Release() == 1);
    { auto pin = proxy->UnwrapVertex(&native_device, vertex); assert(pin.native() == &vb); }
    { auto pin = proxy->UnwrapIndex(&native_device, index); assert(pin.native() == &ib); }
    assert(vertex->Release() == 0 && !vb.destroyed && facade.refs == 1);
    assert(index->Release() == 0 && !ib.destroyed && facade.destroyed && native_device.refs == 1);
    assert(proxy->CloseFacadeDevice(&native_device, &facade) && vb.destroyed && ib.destroyed);
    assert(native_device.Release() == 0);
}

void ClosedFacadeReacquiresDormantBuffers() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    FakeDevice native_device, facade;
    Vertex raw; raw.device = &native_device;
    auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
    const BYTE bytes[]{3, 1, 4}; BYTE observed[3]{};
    Fill(buffer, 0, bytes, 3);
    raw.AddRef(); // Native device/state block retains the resource.
    assert(buffer->Release() == 0 && facade.refs == 1 && raw.refs == 2);
    raw.AddRef(); // Native getter +1.
    auto* recovered = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 0, false);
    assert(recovered == buffer && facade.refs == 2 && raw.refs == 2);
    assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 3) == RegistryResult::Ok);
    assert(std::memcmp(observed, bytes, 3) == 0);
    IDirect3DDevice8* parent = nullptr;
    assert(recovered->GetDevice(&parent) == S_OK && parent == &facade);
    parent->Release();
    assert(recovered->Release() == 0 && facade.refs == 1);
    assert(raw.Release() == 1); // Native binding released.
    assert(proxy->CloseFacadeDevice(&native_device, &facade) && raw.destroyed);
    assert(facade.Release() == 0 && native_device.Release() == 0);
}

void ClosedFacadeBufferPrivateData() {
    struct Bridge final : PrivateObjectBridge {
        std::shared_ptr<BufferProxy> proxy;
        FakeDevice *owner, *facade, *resource_native, *resource_facade;
        Bridge(std::shared_ptr<BufferProxy> proxy_, FakeDevice* owner_, FakeDevice* facade_,
               FakeDevice* resource_, FakeDevice* wrapper_) : proxy(std::move(proxy_)), owner(owner_),
               facade(facade_), resource_native(resource_), resource_facade(wrapper_) {}
        bool Pin(const void* candidate, PrivateObjectPin& pin) noexcept override {
            if (candidate == resource_facade) {
                resource_facade->AddRef(); pin.guard = resource_facade; pin.native = resource_native;
                pin.kind = PrivateDataKind::Texture; return true;
            }
            bool vertex = false;
            pin.guard = proxy->PinPrivateBuffer(owner, facade, candidate, &pin.native, &vertex);
            pin.kind = vertex ? PrivateDataKind::VertexBuffer : PrivateDataKind::IndexBuffer;
            return pin.guard && pin.native;
        }
        IUnknown* Adopt(PrivateDataKind kind, IUnknown* native) noexcept override {
            if (kind == PrivateDataKind::IndexBuffer)
                return proxy->AdoptIndexForFacade(owner, facade, static_cast<IDirect3DIndexBuffer8*>(native), 8, false);
            assert(kind == PrivateDataKind::Texture && native == resource_native);
            native->Release(); resource_facade->AddRef(); return resource_facade;
        }
    };
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    FakeDevice native, facade, resource_native, resource_facade;
    auto bridge = std::make_shared<Bridge>(proxy, &native, &facade, &resource_native, &resource_facade);
    assert(proxy->InstallFacadePrivateBridge(&native, &facade, bridge));
    assert(!proxy->InstallFacadePrivateBridge(&native, &facade, bridge));
    Vertex vb; Index ib; vb.device = &native; ib.device = &native;
    auto* source = proxy->AdoptVertexForFacade(&native, &facade, &vb, 8, true);
    auto* target = proxy->AdoptIndexForFacade(&native, &facade, &ib, 8, true);
    assert(source && target);
    IUnknown* stored = nullptr;
    vb.on_private_set = [&](REFGUID, const void* data, DWORD size, DWORD flags) {
        assert((data == &ib || data == &resource_native) && size == sizeof(IUnknown*) && flags == D3DSPD_IUNKNOWN);
        DWORD nested_size = 0;
        assert(source->GetPrivateData(kAlias, nullptr, &nested_size) == E_NOTIMPL);
        auto* value = const_cast<IUnknown*>(static_cast<const IUnknown*>(data));
        value->AddRef(); if (stored) stored->Release(); stored = value;
        SetLastError(901); return S_OK;
    };
    vb.on_private_get = [&](REFGUID, void* data, DWORD* size) {
        if (!stored || !size || !data || *size < sizeof(IUnknown*)) return E_FAIL;
        stored->AddRef(); std::memcpy(data, &stored, sizeof(stored)); *size = sizeof(stored);
        SetLastError(902); return S_OK;
    };
    vb.on_private_free = [&](REFGUID) {
        if (!stored) return E_FAIL;
        stored->Release(); stored = nullptr; SetLastError(903); return S_OK;
    };
    assert(source->SetPrivateData(kAlias, target, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == S_OK);
    assert(GetLastError() == 901 && stored == &ib && ib.refs == 2);
    assert(target->Release() == 0 && ib.refs == 2);
    IUnknown* value = nullptr; DWORD size = sizeof(value);
    assert(source->GetPrivateData(kAlias, &value, &size) == S_OK && value == target && GetLastError() == 902);
    value->Release();
    const auto ticket = proxy->BeginFacadeReset(&native, &facade);
    assert(ticket && ib.refs == 1 && !ib.destroyed && proxy->EndFacadeReset(&native, ticket, true));
    size = sizeof(value); value = nullptr;
    assert(source->GetPrivateData(kAlias, &value, &size) == S_OK && value);
    void* typed = nullptr;
    assert(value->QueryInterface(IID_IDirect3DIndexBuffer8, &typed) == S_OK && typed);
    IDirect3DDevice8* parent = nullptr;
    assert(static_cast<IDirect3DIndexBuffer8*>(typed)->GetDevice(&parent) == S_OK && parent == &facade);
    parent->Release(); static_cast<IUnknown*>(typed)->Release(); value->Release();
    assert(source->SetPrivateData(kAlias, &resource_facade, sizeof(IUnknown*), D3DSPD_IUNKNOWN) == S_OK);
    assert(stored == &resource_native && resource_native.refs == 2 && resource_facade.refs == 1);
    size = sizeof(value); value = nullptr;
    assert(source->GetPrivateData(kAlias, &value, &size) == S_OK && value == &resource_facade);
    value->Release();
    assert(source->FreePrivateData(kAlias) == S_OK && resource_native.refs == 1 && GetLastError() == 903);
    assert(source->Release() == 0 && proxy->CloseFacadeDevice(&native, &facade));
    assert(vb.destroyed && ib.destroyed && facade.Release() == 0 && native.Release() == 0 &&
           resource_native.Release() == 0 && resource_facade.Release() == 0);
}

void EagerFacadeBufferRetirement() {
    struct EmptyBridge final : PrivateObjectBridge {
        bool Pin(const void*, PrivateObjectPin&) noexcept override { return false; }
        IUnknown* Adopt(PrivateDataKind, IUnknown* native) noexcept override {
            if (native) native->Release();
            return nullptr;
        }
    };
    for (unsigned n = 0; n < 200; ++n) {
        Reader reader;
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native, facade;
        auto bridge = std::make_shared<EmptyBridge>();
        assert(proxy->InstallFacadePrivateBridge(&native, &facade, bridge, false));
        Vertex raw; raw.device = &native;
        auto* wrapped = proxy->AdoptVertexForFacade(&native, &facade, &raw, 8, true);
        const BYTE bytes[]{4, 2}; BYTE observed[sizeof(bytes)]{};
        Fill(wrapped, 0, bytes, sizeof(bytes));
        raw.AddRef(); // independent native binding
        std::atomic<bool> go{false};
        std::thread getter([&] {
            while (!go.load()) std::this_thread::yield();
            raw.AddRef();
            auto* found = proxy->AdoptVertexForFacade(&native, &facade, &raw, 8, false);
            assert(found);
            found->Release();
        });
        go = true;
        wrapped->Release();
        getter.join();
        assert(raw.refs == 1 && facade.refs == 1);
        assert(proxy->ReadVertex(&native, &raw, 0, observed, sizeof(observed)) == RegistryResult::UnknownResource);
        raw.AddRef();
        auto* recovered = proxy->AdoptVertexForFacade(&native, &facade, &raw, 8, false);
        assert(recovered && proxy->ReadVertex(&native, &raw, 0, observed, sizeof(observed)) == RegistryResult::UnknownResource);
        assert(recovered->Release() == 0 && proxy->CloseFacadeDevice(&native, &facade));
        assert(raw.Release() == 0 && facade.Release() == 0 && native.Release() == 0);
    }
}

void PersistentEagerBufferShadows() {
    struct EmptyBridge final : PrivateObjectBridge {
        bool Pin(const void*, PrivateObjectPin&) noexcept override { return false; }
        IUnknown* Adopt(PrivateDataKind, IUnknown* native) noexcept override {
            if (native) native->Release();
            return nullptr;
        }
    };
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    FakeDevice native, facade;
    auto bridge = std::make_shared<EmptyBridge>();
    auto markers = std::make_shared<t3render::markers::PrivateMarkerStore>(8);
    assert(proxy->InstallFacadePrivateBridge(&native, &facade, bridge, false, markers));
    Vertex vb; Index ib; vb.device = &native; ib.device = &native;
    IUnknown *vertex_marker = nullptr, *index_marker = nullptr;
    bool blob_stored = false;
    const BYTE blob[]{7, 9};
    auto configure = [&](auto& raw, IUnknown*& slot) {
        IUnknown** marker = &slot;
        auto* resource = &raw;
        raw.on_private_get = [&, marker](REFGUID key, void* data, DWORD* size) {
            if (IsEqualIID(key, t3render::markers::kPrivateMarkerGuid))
                return *marker ? S_OK : D3DERR_NOTFOUND;
            if (!blob_stored || !size) return D3DERR_NOTFOUND;
            if (data && *size >= sizeof(blob)) std::memcpy(data, blob, sizeof(blob));
            *size = sizeof(blob); return S_OK;
        };
        raw.on_private_set = [&, marker](REFGUID key, const void* data, DWORD size, DWORD flags) {
            if (IsEqualIID(key, t3render::markers::kPrivateMarkerGuid)) {
                assert(!*marker && size == sizeof(IUnknown*) && flags == D3DSPD_IUNKNOWN);
                *marker = const_cast<IUnknown*>(static_cast<const IUnknown*>(data)); (*marker)->AddRef(); return S_OK;
            }
            assert(data == blob && size == sizeof(blob) && flags == 0);
            blob_stored = true; return S_OK;
        };
        raw.on_release = [resource, marker] {
            if (!resource->refs && *marker) { auto* old = *marker; *marker = nullptr; old->Release(); }
        };
    };
    configure(vb, vertex_marker); configure(ib, index_marker);
    auto* vertex = proxy->AdoptVertexForFacade(&native, &facade, &vb, 8, true);
    auto* index = proxy->AdoptIndexForFacade(&native, &facade, &ib, 8, true);
    assert(vertex && index && vertex_marker && index_marker);
    const BYTE data[]{1, 4, 9}; BYTE observed[sizeof(data)]{};
    Fill(vertex, 0, data, sizeof(data)); Fill(index, 0, data, sizeof(data));
    assert(vertex->SetPrivateData(kAlias, blob, sizeof(blob), 0) == S_OK);
    assert(vertex->SetPrivateData(t3render::markers::kPrivateMarkerGuid, blob, sizeof(blob), 0) == E_NOTIMPL);
    vb.AddRef(); ib.AddRef(); // native state block retains both
    assert(vertex->Release() == 0 && index->Release() == 0 && facade.refs == 1);
    assert(proxy->ReadVertex(&native, reinterpret_cast<IDirect3DVertexBuffer8*>(1), 0, observed, sizeof(observed)) ==
           RegistryResult::UnknownResource); // ordinary reads never dereference unknown inputs
    assert(proxy->ReadVertex(&native, &vb, 0, observed, sizeof(observed)) == RegistryResult::UnknownResource);
    assert(proxy->ReadVertexFromGetter(&native, &vb, 0, observed, sizeof(observed)) == RegistryResult::Ok &&
           !std::memcmp(observed, data, sizeof(data)));
    assert(proxy->ReadIndexFromGetter(&native, &ib, 0, observed, sizeof(observed)) == RegistryResult::Ok &&
           !std::memcmp(observed, data, sizeof(data)));
    VertexAlias alias(vb);
    bool reentered = false;
    vb.on_query = [&] {
        reentered = true;
        assert(proxy->ReadVertexFromGetter(&native, &vb, 0, observed, sizeof(observed)) == RegistryResult::UnknownResource);
    };
    SetLastError(37);
    assert(proxy->ReadVertexFromGetter(&native, &alias, 0, observed, sizeof(observed)) == RegistryResult::Ok &&
           reentered && GetLastError() == 37 && !std::memcmp(observed, data, sizeof(data)));
    vb.on_query = {};
    vb.AddRef(); ib.AddRef();
    vertex = proxy->AdoptVertexForFacade(&native, &facade, &vb, 8, false);
    index = proxy->AdoptIndexForFacade(&native, &facade, &ib, 8, false);
    assert(vertex && index);
    BYTE retrieved[sizeof(blob)]{}; DWORD blob_size = sizeof(retrieved);
    assert(vertex->GetPrivateData(kAlias, retrieved, &blob_size) == S_OK && blob_size == sizeof(blob) &&
           !std::memcmp(retrieved, blob, sizeof(blob)));
    assert(proxy->ReadVertex(&native, &vb, 0, observed, sizeof(observed)) == RegistryResult::Ok);
    const BYTE update[]{6, 5};
    const BYTE updated[]{1, 6, 5};
    Fill(vertex, 1, update, sizeof(update));
    assert(proxy->ReadVertex(&native, &vb, 0, observed, sizeof(observed)) == RegistryResult::Ok &&
           !std::memcmp(observed, updated, sizeof(updated)));
    alignas(Vertex) unsigned char storage[sizeof(Vertex)];
    IUnknown* recycled_marker = nullptr;
    auto* recycled = new (storage) Vertex;
    recycled->device = &native; configure(*recycled, recycled_marker);
    auto* transient = proxy->AdoptVertexForFacade(&native, &facade, recycled, 8, true);
    assert(transient && recycled_marker);
    Fill(transient, 0, data, sizeof(data));
    assert(transient->Release() == 0 && recycled->destroyed && !recycled_marker);
    recycled->~Vertex();
    recycled = new (storage) Vertex;
    recycled->device = &native; configure(*recycled, recycled_marker);
    transient = proxy->AdoptVertexForFacade(&native, &facade, recycled, 8, true);
    assert(transient && recycled_marker &&
           proxy->ReadVertex(&native, recycled, 0, observed, sizeof(observed)) == RegistryResult::UnknownData);
    Fill(transient, 0, data, sizeof(data));
    assert(transient->Release() == 0 && recycled->destroyed && !recycled_marker);
    recycled->~Vertex();
    for (unsigned n = 0; n < 100; ++n) {
        Vertex racing; racing.device = &native;
        IUnknown* racing_marker = nullptr;
        configure(racing, racing_marker);
        auto* current = proxy->AdoptVertexForFacade(&native, &facade, &racing, 8, true);
        assert(current && racing_marker);
        Fill(current, 0, data, sizeof(data));
        racing.AddRef(); // native binding keeps the identity alive
        std::atomic<bool> go{false};
        std::thread getter([&] {
            while (!go.load()) std::this_thread::yield();
            racing.AddRef();
            auto* found = proxy->AdoptVertexForFacade(&native, &facade, &racing, 8, false);
            assert(found);
            found->Release();
        });
        go = true;
        current->Release();
        getter.join();
        assert(proxy->ReadVertexFromGetter(&native, &racing, 0, observed, sizeof(observed)) == RegistryResult::Ok &&
               !std::memcmp(observed, data, sizeof(data)));
        assert(racing.Release() == 0 && racing.destroyed && !racing_marker);
    }
    Vertex collision; collision.device = &native;
    collision.on_private_get = [](REFGUID key, void*, DWORD*) {
        assert(IsEqualIID(key, t3render::markers::kPrivateMarkerGuid)); return S_OK;
    };
    auto* unmarked = proxy->AdoptVertexForFacade(&native, &facade, &collision, 8, true);
    assert(unmarked);
    Fill(unmarked, 0, data, sizeof(data));
    collision.AddRef();
    assert(unmarked->Release() == 0 &&
           proxy->ReadVertex(&native, &collision, 0, observed, sizeof(observed)) == RegistryResult::UnknownResource);
    assert(collision.Release() == 0);
    assert(vertex->Release() == 0 && index->Release() == 0);
    assert(proxy->ReadVertexFromGetter(&native, &vb, 0, observed, sizeof(observed)) == RegistryResult::Ok &&
           !std::memcmp(observed, updated, sizeof(updated)));
    bool reset_reentered = false;
    vb.on_query = [&] {
        reset_reentered = true;
        const auto reset = proxy->BeginFacadeReset(&native, &facade);
        assert(reset && proxy->EndFacadeReset(&native, reset, true));
    };
    assert(proxy->ReadVertexFromGetter(&native, &vb, 0, observed, sizeof(observed)) == RegistryResult::UnknownResource);
    assert(reset_reentered && proxy->ReadIndexFromGetter(&native, &ib, 0, observed, sizeof(observed)) == RegistryResult::UnknownResource);
    vb.on_query = {};
    vb.AddRef();
    vertex = proxy->AdoptVertexForFacade(&native, &facade, &vb, 8, false);
    assert(vertex && proxy->ReadVertex(&native, &vb, 0, observed, sizeof(observed)) == RegistryResult::UnknownResource);
    blob_size = sizeof(retrieved);
    assert(vertex->GetPrivateData(kAlias, retrieved, &blob_size) == S_OK);
    assert(vertex->Release() == 0 && proxy->CloseFacadeDevice(&native, &facade));
    assert(vb.Release() == 0 && ib.Release() == 0 && !vertex_marker && !index_marker);
    assert(native.Release() == 0 && facade.Release() == 0);

    auto hazardous = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    FakeDevice native2, facade2;
    auto markers2 = std::make_shared<t3render::markers::PrivateMarkerStore>(8);
    assert(hazardous->InstallFacadePrivateBridge(&native2, &facade2, bridge, false, markers2));
    Vertex dormant; Index active; dormant.device = &native2; active.device = &native2;
    IUnknown *dormant_marker = nullptr, *active_marker = nullptr;
    configure(dormant, dormant_marker); configure(active, active_marker);
    auto* dormant_wrapper = hazardous->AdoptVertexForFacade(&native2, &facade2, &dormant, 8, true);
    auto* active_wrapper = hazardous->AdoptIndexForFacade(&native2, &facade2, &active, 8, true);
    assert(dormant_wrapper && active_wrapper && dormant_marker && active_marker);
    Fill(dormant_wrapper, 0, data, sizeof(data));
    dormant.AddRef(); dormant_wrapper->Release();
    BYTE* mapped = nullptr;
    assert(active_wrapper->Lock(0, 2, &mapped, 0) == S_OK && mapped);
    assert(hazardous->BeginDraw(&native2) == 0 &&
           hazardous->ReadVertexFromGetter(&native2, &dormant, 0, observed, sizeof(observed)) == RegistryResult::Unavailable);
    assert(active_wrapper->Unlock() == S_OK && active_wrapper->Release() == 0 && dormant.Release() == 0);
    assert(hazardous->CloseFacadeDevice(&native2, &facade2) && !dormant_marker && !active_marker);
    assert(native2.Release() == 0 && facade2.Release() == 0);

    auto cross = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    FakeDevice first_device, first_facade, second_device, second_facade;
    auto shared_markers = std::make_shared<t3render::markers::PrivateMarkerStore>(8);
    assert(cross->InstallFacadePrivateBridge(&first_device, &first_facade, bridge, false, shared_markers));
    assert(cross->InstallFacadePrivateBridge(&second_device, &second_facade, bridge, false, shared_markers));
    auto unrelated = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    assert(!unrelated->InstallFacadePrivateBridge(&second_device, &second_facade, bridge, false, shared_markers));
    Vertex cross_raw; cross_raw.device = &first_device;
    IUnknown* cross_marker = nullptr; configure(cross_raw, cross_marker);
    auto* first_wrapper = cross->AdoptVertexForFacade(&first_device, &first_facade, &cross_raw, 8, true);
    assert(first_wrapper && cross_marker);
    Fill(first_wrapper, 0, data, sizeof(data));
    cross_raw.AddRef(); first_wrapper->Release();
    cross_raw.AddRef();
    auto* foreign = cross->AdoptVertexForFacade(&second_device, &second_facade, &cross_raw, 8, false);
    assert(foreign && cross->ReadVertexFromGetter(&first_device, &cross_raw, 0, observed, sizeof(observed)) == RegistryResult::Unavailable &&
           cross->ReadVertexFromGetter(&second_device, &cross_raw, 0, observed, sizeof(observed)) == RegistryResult::Unavailable);
    foreign->Release(); cross_raw.Release();
    assert(cross->CloseFacadeDevice(&first_device, &first_facade) && cross->CloseFacadeDevice(&second_device, &second_facade));
    assert(!cross_marker && first_device.Release() == 0 && first_facade.Release() == 0 &&
           second_device.Release() == 0 && second_facade.Release() == 0);
}

void ClosedFacadeRetirement() {
    Reader reader;
    for (bool stop : {false, true}) {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        // A premature close must leave live COM objects intact and reject new
        // getter references until the caller can close again.
        if (stop) proxy->StopObservation();
        assert(!proxy->CloseFacadeDevice(&native_device, &facade));
        raw.AddRef();
        assert(!proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 0, false));
        assert(raw.refs == 1 && facade.refs == 2);
        assert(buffer->Release() == 0 && !raw.destroyed);
        bool reentered = false;
        raw.on_release = [&] {
            BYTE value{};
            reentered = true;
            (void)proxy->ReadVertex(&native_device, &raw, 0, &value, 1);
        };
        assert(proxy->CloseFacadeDevice(&native_device, &facade));
        assert(reentered && raw.destroyed && facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw;
        assert(proxy->CloseFacadeDevice(&native_device, &facade));
        assert(!proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true) && raw.destroyed);
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        bool closed = false;
        facade.on_final_release = [&] { closed = proxy->CloseFacadeDevice(&native_device, &facade); };
        assert(facade.Release() == 1);
        // The parent's final Release retires the wrapper while its own final
        // Release is on the stack. No code may touch that wrapper afterwards.
        assert(buffer->Release() == 0 && closed && raw.destroyed && facade.destroyed);
        assert(native_device.Release() == 0);
    }
}

void ClosedFacadeResetRetiresDormantBuffers() {
    Reader reader;
    auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
    FakeDevice native_device, facade;
    Vertex raw; Index live;
    raw.device = &native_device; live.device = &native_device;
    auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
    auto* index = proxy->AdoptIndexForFacade(&native_device, &facade, &live, 8, true);
    const BYTE bytes[]{3, 5}; BYTE observed[2]{};
    Fill(buffer, 0, bytes, 2); Fill(index, 0, bytes, 2);
    raw.AddRef(); // A native state block retains this resource.
    assert(buffer->Release() == 0 && raw.refs == 2);
    bool reentered = false;
    raw.on_release = [&] {
        reentered = true;
        (void)proxy->ReadIndex(&native_device, &live, 0, observed, 1);
    };
    SetLastError(85);
    proxy->RetireFacadeDormantForReset(&native_device, &facade);
    assert(reentered && GetLastError() == 85 && raw.refs == 1 && live.refs == 1);
    raw.on_release = {};
    assert(proxy->ReadIndex(&native_device, &live, 0, observed, 2) == RegistryResult::Unavailable);
    raw.AddRef(); // A getter returns a +1 after reset.
    auto* restored = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 0, false);
    assert(restored && restored != &raw && raw.refs == 2);
    IDirect3DDevice8* parent = nullptr;
    assert(restored->GetDevice(&parent) == S_OK && parent == &facade);
    parent->Release();
    Fill(restored, 0, bytes, 2);
    assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 2) == RegistryResult::Unavailable);
    Vertex after; after.device = &native_device;
    auto* created = proxy->AdoptVertexForFacade(&native_device, &facade, &after, 8, true);
    assert(created && created != &after);
    Fill(created, 0, bytes, 2);
    assert(proxy->ReadVertex(&native_device, &after, 0, observed, 2) == RegistryResult::Unavailable);
    assert(restored->Release() == 0 && created->Release() == 0 && index->Release() == 0);
    assert(proxy->CloseFacadeDevice(&native_device, &facade) && live.destroyed && after.destroyed && raw.refs == 1);
    assert(raw.Release() == 0 && facade.Release() == 0 && native_device.Release() == 0);
}

void ClosedFacadeResetEpochs() {
    Reader reader;
    const BYTE bytes[]{7, 8, 9}; BYTE observed[3]{};
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 512, 8);
        FakeDevice native_device, facade;
        Vertex old, fresh;
        old.device = &native_device; fresh.device = &native_device;
        auto* old_wrapper = proxy->AdoptVertexForFacade(&native_device, &facade, &old, 8, true);
        Fill(old_wrapper, 0, bytes, 3);
        const auto reset = proxy->BeginFacadeReset(&native_device, &facade);
        assert(reset && proxy->ReadVertex(&native_device, &old, 0, observed, 3) == RegistryResult::Unavailable);
        assert(proxy->EndFacadeReset(&native_device, reset, true));
        assert(proxy->ReadVertex(&native_device, &old, 0, observed, 3) == RegistryResult::UnknownResource);
        Fill(old_wrapper, 0, bytes, 3); // known wrapper, but pre-reset bytes remain unknown
        auto* new_wrapper = proxy->AdoptVertexForFacade(&native_device, &facade, &fresh, 8, true);
        Fill(new_wrapper, 0, bytes, 3);
        const auto draw = proxy->BeginDraw(&native_device);
        assert(draw && proxy->ReadVertex(&native_device, &fresh, 0, observed, 3) == RegistryResult::Ok);
        assert(std::memcmp(observed, bytes, 3) == 0 && proxy->EndDraw(&native_device, draw));
        assert(old_wrapper->Release() == 0 && new_wrapper->Release() == 0);
        assert(proxy->CloseFacadeDevice(&native_device, &facade) && old.destroyed && fresh.destroyed);
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw; raw.device = &native_device;
        const auto failed = proxy->BeginFacadeReset(&native_device, &facade);
        assert(failed && proxy->EndFacadeReset(&native_device, failed, false));
        assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 1) == RegistryResult::Unavailable);
        const auto recovery = proxy->BeginFacadeReset(&native_device, &facade);
        assert(recovery && recovery != failed && proxy->EndFacadeReset(&native_device, recovery, true));
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        Fill(buffer, 0, bytes, 3);
        assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 3) == RegistryResult::Ok);
        assert(buffer->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade) && raw.destroyed);
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
    for (bool overlap_reset : {false, true}) {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 512, 8);
        FakeDevice native_device, facade;
        Vertex old, fresh; old.device = &native_device; fresh.device = &native_device;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &old, 8, true);
        Fill(buffer, 0, bytes, 3);
        old.AddRef(); // retained outside the dormant wrapper by the native device
        assert(buffer->Release() == 0);
        const auto reset = proxy->BeginFacadeReset(&native_device, &facade);
        assert(reset && proxy->EndFacadeReset(&native_device, reset, true));
        old.AddRef(); // native getter returns a new reference
        auto* recovered = proxy->AdoptVertexForFacade(&native_device, &facade, &old, 0, false);
        assert(recovered && recovered != &old);
        assert(proxy->ReadVertex(&native_device, &old, 0, observed, 1) == RegistryResult::UnknownResource);
        auto* created = proxy->AdoptVertexForFacade(&native_device, &facade, &fresh, 8, true);
        Fill(created, 0, bytes, 3);
        BYTE* mapped = nullptr;
        assert(recovered->Lock(0, 1, &mapped, 0) == S_OK && mapped);
        if (overlap_reset) assert(!proxy->BeginFacadeReset(&native_device, &facade));
        assert(recovered->Unlock() == S_OK);
        if (overlap_reset) {
            assert(proxy->ReadVertex(&native_device, &fresh, 0, observed, 3) == RegistryResult::Unavailable);
        } else {
            const auto draw = proxy->BeginDraw(&native_device);
            assert(draw && proxy->ReadVertex(&native_device, &fresh, 0, observed, 3) == RegistryResult::Ok);
            assert(proxy->ReadVertex(&native_device, &old, 0, observed, 1) == RegistryResult::UnknownResource);
            assert(proxy->EndDraw(&native_device, draw));
        }
        assert(recovered->Release() == 0 && created->Release() == 0);
        assert(old.Release() == 1 && proxy->CloseFacadeDevice(&native_device, &facade));
        assert(old.destroyed && fresh.destroyed && facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw;
        // Represents a native Create/Get call still in flight, before adoption.
        const auto operation = proxy->BeginBindingMutation(&native_device);
        assert(operation && !proxy->BeginFacadeReset(&native_device, &facade));
        proxy->EndBindingMutation(&native_device, operation);
        auto* created = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        Fill(created, 0, bytes, 3);
        assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 3) == RegistryResult::Unavailable);
        assert(created->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade) && raw.destroyed);
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
    for (int overlap = 0; overlap != 6; ++overlap) {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 512, 8);
        FakeDevice native_device, facade;
        Vertex old, fresh, extra; old.device = &native_device; fresh.device = &native_device; extra.device = &native_device;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &old, 8, true);
        Fill(buffer, 0, bytes, 3);
        const auto reset = proxy->BeginFacadeReset(&native_device, &facade);
        assert(reset);
        if (overlap == 0) assert(!proxy->BeginDraw(&native_device));
        if (overlap == 1) {
            BYTE* mapped = nullptr;
            assert(buffer->Lock(0, 1, &mapped, 0) == S_OK && buffer->Unlock() == S_OK);
        }
        if (overlap == 2) assert(!proxy->BeginBindingMutation(&native_device));
        if (overlap == 3) assert(!proxy->BeginFacadeReset(&native_device, &facade));
        if (overlap == 4) { assert(buffer->Release() == 0); buffer = nullptr; }
        if (overlap == 5) {
            auto* concurrent = proxy->AdoptVertexForFacade(&native_device, &facade, &extra, 8, true);
            assert(concurrent && concurrent != &extra && concurrent->Release() == 0);
        }
        assert(!proxy->EndFacadeReset(&native_device, reset, true));
        auto* created = proxy->AdoptVertexForFacade(&native_device, &facade, &fresh, 8, true);
        Fill(created, 0, bytes, 3);
        assert(proxy->ReadVertex(&native_device, &fresh, 0, observed, 3) == RegistryResult::Unavailable);
        if (buffer) assert(buffer->Release() == 0);
        assert(created->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade));
        if (overlap == 5) assert(extra.destroyed);
        else assert(extra.Release() == 0);
        assert(old.destroyed && fresh.destroyed && facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw; raw.device = &native_device;
        const auto failed = proxy->BeginFacadeReset(&native_device, &facade);
        assert(failed && proxy->EndFacadeReset(&native_device, failed, false));
        assert(!proxy->BeginDraw(&native_device)); // illegal work during recovery poisons the device
        assert(!proxy->BeginFacadeReset(&native_device, &facade));
        auto* created = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        Fill(created, 0, bytes, 3);
        assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 3) == RegistryResult::Unavailable);
        assert(created->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade) && raw.destroyed);
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade, wrong_facade;
        Vertex raw, fresh;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        assert(!proxy->BeginFacadeReset(&native_device, &wrong_facade));
        auto* created = proxy->AdoptVertexForFacade(&native_device, &facade, &fresh, 8, true);
        Fill(created, 0, bytes, 3);
        assert(proxy->ReadVertex(&native_device, &fresh, 0, observed, 3) == RegistryResult::Unavailable);
        assert(buffer->Release() == 0 && created->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade));
        assert(raw.destroyed && fresh.destroyed && wrong_facade.Release() == 0 && facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw;
        proxy->StopObservation();
        assert(!proxy->BeginFacadeReset(&native_device, &facade));
        auto* created = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        Fill(created, 0, bytes, 3);
        assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 3) == RegistryResult::Unavailable);
        assert(created->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade) && raw.destroyed);
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw; raw.device = &native_device;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        raw.AddRef(); // keep the native object alive when the dormant wrapper is retired
        assert(buffer->Release() == 0);
        raw.on_release = [&] { assert(!proxy->BeginDraw(&native_device)); };
        const auto reset = proxy->BeginFacadeReset(&native_device, &facade);
        assert(reset && !proxy->EndFacadeReset(&native_device, reset, true));
        raw.on_release = {};
        assert(raw.Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade));
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
}

void ClosedFacadeFailuresDoNotExposeNative() {
    Reader reader;
    const BYTE bytes[]{5, 6}; BYTE observed[2]{};
    for (int scenario = 0; scenario != 3; ++scenario) {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, wrong_device, facade;
        Vertex raw;
        raw.device = scenario == 0 ? &wrong_device : scenario == 1 ? nullptr : &native_device;
        if (scenario == 2) native_device.fail_identity = true;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        Fill(buffer, 0, bytes, 2);
        IDirect3DDevice8* result = reinterpret_cast<IDirect3DDevice8*>(1);
        SetLastError(19);
        assert(buffer->GetDevice(&result) == E_FAIL && !result && GetLastError() == 830);
        assert(native_device.refs == 1 && wrong_device.refs == 1 && facade.refs == 2);
        assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 2) == RegistryResult::Unavailable);
        assert(buffer->Release() == 0 && !raw.destroyed);
        assert(proxy->CloseFacadeDevice(&native_device, &facade) && raw.destroyed);
        assert(facade.Release() == 0 && native_device.Release() == 0 && wrong_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 1);
        FakeDevice native_device, facade;
        Vertex first; Index overflow;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &first, 8, true);
        SetLastError(20);
        assert(!proxy->AdoptIndexForFacade(&native_device, &facade, &overflow, 8, true));
        assert(overflow.destroyed && GetLastError() == 20 && facade.refs == 2);
        assert(buffer->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade));
        assert(first.destroyed && facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade, other_facade;
        Vertex raw;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        raw.AddRef();
        assert(!proxy->AdoptVertexForFacade(&native_device, &other_facade, &raw, 0, false));
        assert(raw.refs == 1 && proxy->ReadVertex(&native_device, &raw, 0, observed, 2) == RegistryResult::Unavailable);
        assert(buffer->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade));
        assert(raw.destroyed && facade.Release() == 0 && other_facade.Release() == 0 && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device;
        Vertex raw;
        assert(!proxy->AdoptVertexForFacade(&native_device, nullptr, &raw, 8, true));
        assert(raw.destroyed && native_device.Release() == 0);
    }
    {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw; raw.device = &native_device; raw.device_result = E_FAIL;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        Fill(buffer, 0, bytes, 2);
        IDirect3DDevice8* result = reinterpret_cast<IDirect3DDevice8*>(1);
        assert(buffer->GetDevice(&result) == E_FAIL && result == reinterpret_cast<IDirect3DDevice8*>(1));
        assert(proxy->ReadVertex(&native_device, &raw, 0, observed, 2) == RegistryResult::Ok);
        assert(buffer->Release() == 0 && proxy->CloseFacadeDevice(&native_device, &facade));
        assert(raw.destroyed && facade.Release() == 0 && native_device.Release() == 0);
    }
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        raw.AddRef(); // A device getter owns this native reference during the race.
        std::atomic<bool> go{false};
        std::thread getter([&] {
            while (!go.load()) std::this_thread::yield();
            if (auto* result = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 0, false))
                result->Release();
        });
        go = true;
        buffer->Release();
        getter.join();
        assert(!raw.destroyed && raw.refs == 1 && facade.refs == 1);
        assert(proxy->CloseFacadeDevice(&native_device, &facade) && raw.destroyed);
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        auto proxy = BufferProxy::Create(Reader::Copy, &reader, 256, 8);
        FakeDevice native_device, facade;
        Vertex raw;
        auto* buffer = proxy->AdoptVertexForFacade(&native_device, &facade, &raw, 8, true);
        std::atomic<bool> go{false};
        bool closed = false;
        std::thread closer([&] {
            while (!go.load()) std::this_thread::yield();
            closed = proxy->CloseFacadeDevice(&native_device, &facade);
        });
        go = true;
        buffer->Release();
        closer.join();
        if (!closed) assert(proxy->CloseFacadeDevice(&native_device, &facade));
        assert(raw.destroyed && facade.refs == 1);
        assert(facade.Release() == 0 && native_device.Release() == 0);
    }
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
    NativeLockWindows();
    ConcurrentNativeLockWindows();
    UnknownNativeLocks();
    LockCountsAndInvalidation();
    OwnerIsolationAndFlagModes();
    ClosedFacadeBufferBoundary();
    ClosedFacadeReacquiresDormantBuffers();
    ClosedFacadeBufferPrivateData();
    EagerFacadeBufferRetirement();
    PersistentEagerBufferShadows();
    ClosedFacadeRetirement();
    ClosedFacadeResetRetiresDormantBuffers();
    ClosedFacadeResetEpochs();
    ClosedFacadeFailuresDoNotExposeNative();
    UnlockFailureAndCopyFailures();
    CapacityAndUnknownBuffersForward();
    StopObservationKeepsForwarding();
    ResetProcessAndFailures();
}
