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
    HRESULT device_result = S_OK;
    IDirect3DDevice8* device = nullptr;
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
    HRESULT GetDevice(IDirect3DDevice8** out) override {
        if (out && SUCCEEDED(device_result)) { *out = device; if (device) device->AddRef(); }
        SetLastError(830); return device_result;
    }
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
    ClosedFacadeRetirement();
    ClosedFacadeFailuresDoNotExposeNative();
    UnlockFailureAndCopyFailures();
    CapacityAndUnknownBuffersForward();
    StopObservationKeepsForwarding();
    ResetProcessAndFailures();
}
