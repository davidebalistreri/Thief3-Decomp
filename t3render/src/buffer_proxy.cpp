#include "buffer_proxy.hpp"
#include "private_blob.hpp"

#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace t3render::buffers {
namespace {
struct LastError final {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
    void restore() const { SetLastError(value); }
    void capture() { value = GetLastError(); }
};

struct Control {
    IDirect3DDevice8* owner = nullptr; // address only; the native resource owns any device ref
    IDirect3DDevice8* facade = nullptr; // owned +1 only in the isolated facade experiment
    void* native = nullptr;
    void* identity = nullptr;
    void* exposed = nullptr;
    bool vertex = false;
    ir::BufferToken token{};
    bool pending_lock = false, locked = false, pending_unlock = false, read_only = false;
    bool counted_lock = false;
    Control* retirement_next = nullptr;
    const void* data = nullptr;
    std::size_t length = 0;
    std::uint64_t operation = 0;
    virtual bool TryAddRef() noexcept = 0;
    virtual bool TryRevive() noexcept = 0;
    virtual bool Dormant() const noexcept = 0;
    virtual void DestroyDormant() noexcept = 0;
    virtual IUnknown* Unknown() noexcept = 0;
protected:
    ~Control() = default;
};

struct DeviceState {
    bool unsafe = false;
    bool closing = false;
    bool reset_recovery = false;
    std::uint64_t draw = 0, mutation = 0, reset = 0;
    std::size_t active_locks = 0;
    IDirect3DDevice8* private_facade = nullptr; // borrowed; live buffer keeps it alive
    std::weak_ptr<PrivateObjectBridge> private_bridge;
};
}  // namespace

struct BufferProxy::State {
    State(SnapshotReader r, void* c, std::size_t budget, std::size_t max) noexcept
        : reader(r), context(c), shadows(budget, max), maximum(max) {}

    // No COM or capture callback while mutex is held. All registry operations
    // are serialized here, including snapshot reads and fault-safe staging.
    std::mutex mutex;
    SnapshotReader reader;
    void* context;
    ir::BufferRegistry shadows;
    const std::size_t maximum;
    std::unordered_map<void*, Control*> wrappers;
    std::unordered_map<void*, Control*> identities;
    std::unordered_map<void*, Control*> native;
    std::unordered_map<IDirect3DDevice8*, DeviceState> devices;
    std::uint64_t next_operation = 1, next_draw = 1, next_reset = 1;
    bool global_unsafe = false;

    void ClearLockLocked(Control* item) noexcept {
        if (item->counted_lock) {
            const auto device = devices.find(item->owner);
            // The owner entry exists while observation is active. Never create
            // an entry here: this also runs during global invalidation.
            if (device != devices.end() && device->second.active_locks) {
                --device->second.active_locks;
                item->counted_lock = false;
            } else {
                // An inconsistent count must never permit a stable draw.
                item->counted_lock = false;
                PoisonAllLocked();
            }
        }
        item->pending_lock = false; item->locked = false; item->pending_unlock = false;
        item->data = nullptr; item->length = 0; item->operation = 0;
    }
    void AbandonLocked(Control* item) noexcept {
        shadows.Abandon(item->token);
        ClearLockLocked(item);
    }
    void PoisonAllLocked() noexcept {
        if (global_unsafe) return;
        global_unsafe = true;
        for (auto& entry : wrappers) AbandonLocked(entry.second);
        for (auto& entry : devices) {
            entry.second.unsafe = true; entry.second.draw = 0;
            entry.second.mutation = 0; entry.second.reset = 0;
            entry.second.reset_recovery = false; entry.second.active_locks = 0;
        }
    }
    DeviceState* DeviceLocked(IDirect3DDevice8* owner) noexcept {
        if (global_unsafe) return nullptr;
        if (!owner) { PoisonAllLocked(); return nullptr; }
        auto found = devices.find(owner);
        if (found != devices.end()) return &found->second;
        if (devices.size() >= maximum) { PoisonAllLocked(); return nullptr; }
        try { return &devices.emplace(owner, DeviceState{}).first->second; }
        catch (...) { PoisonAllLocked(); return nullptr; }
    }
    void PoisonLocked(IDirect3DDevice8* owner) noexcept {
        auto* device = DeviceLocked(owner);
        if (!device || device->unsafe) return;
        device->unsafe = true; device->draw = 0; device->mutation = 0;
        device->reset = 0; device->reset_recovery = false;
        for (auto& entry : wrappers)
            if (entry.second->owner == owner) AbandonLocked(entry.second);
    }
    void DetachLocked(Control* item) noexcept {
        auto it = wrappers.find(item->exposed);
        if (it != wrappers.end() && it->second == item) wrappers.erase(it);
        it = identities.find(item->identity);
        if (it != identities.end() && it->second == item) identities.erase(it);
        it = native.find(item->native);
        if (it != native.end() && it->second == item) native.erase(it);
        shadows.Release(item->token);
    }
};

namespace {
template<class Buffer> struct Traits;
template<> struct Traits<IDirect3DVertexBuffer8> {
    using Desc = D3DVERTEXBUFFER_DESC;
    static constexpr bool kVertex = true;
    static REFIID Iid() { return IID_IDirect3DVertexBuffer8; }
};
template<> struct Traits<IDirect3DIndexBuffer8> {
    using Desc = D3DINDEXBUFFER_DESC;
    static constexpr bool kVertex = false;
    static REFIID Iid() { return IID_IDirect3DIndexBuffer8; }
};

// Canonicalize while the caller-owned native reference keeps the object alive.
// The temporary QI reference is immediately released, without any map mutex.
template<class Buffer>
void* Canonical(Buffer* native) {
    void* identity = nullptr;
    const HRESULT hr = native->QueryInterface(IID_IUnknown, &identity);
    if (FAILED(hr) || !identity) return nullptr;
    reinterpret_cast<IUnknown*>(identity)->Release();
    return identity;
}

template<class Buffer>
class Wrapper final : public Buffer, public Control {
public:
    Wrapper(std::shared_ptr<BufferProxy::State> state, IDirect3DDevice8* owner_, IDirect3DDevice8* facade_,
            Buffer* real, void* canonical, UINT size) noexcept : state_(std::move(state)), real_(real), size_(size) {
        owner = owner_; facade = facade_; native = real; identity = canonical; vertex = Traits<Buffer>::kVertex;
        exposed = static_cast<Buffer*>(this);
    }
    bool TryAddRef() noexcept override {
        ULONG count = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!count) return false;
            // Saturation intentionally leaks instead of allowing wraparound/UAF.
            if (count == std::numeric_limits<ULONG>::max()) return true;
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
    void DestroyDormant() noexcept override {
        Buffer* real = real_;
        delete this;
        real->Release();
    }
    IUnknown* Unknown() noexcept override { return static_cast<Buffer*>(this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirect3DResource8) ||
            IsEqualIID(iid, Traits<Buffer>::Iid())) {
            *output = static_cast<Buffer*>(this); AddRef(); return S_OK;
        }
        if (facade) { *output = nullptr; return E_NOINTERFACE; }
        LastError error; error.restore();
        const HRESULT hr = real_->QueryInterface(iid, output); error.capture();
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->PoisonLocked(owner);
        }
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        ULONG count = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!count) return 0; // no COM resurrection
            if (count == std::numeric_limits<ULONG>::max()) return count;
            if (refs_.compare_exchange_weak(count, count + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) return count + 1;
        }
    }
    ULONG STDMETHODCALLTYPE Release() override {
        if (facade) return ReleaseFacade();
        ULONG count = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!count || count == std::numeric_limits<ULONG>::max()) return count;
            if (refs_.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) break;
        }
        if (count != 1) return count - 1;
        LastError error;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            // A device or state block may still retain the native resource.
            // Final wrapper release cannot establish that an open lock ended.
            if (counted_lock) state_->PoisonLocked(owner);
            state_->DetachLocked(this);
        }
        Buffer* real = real_;
        IDirect3DDevice8* parent = facade;
        delete this;
        error.restore();
        real->Release();
        error.capture();
        if (parent) parent->Release();
        return 0;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** output) override {
        if (facade) return GetFacadeDevice(output);
        LastError error; error.restore();
        const HRESULT hr = real_->GetDevice(output); error.capture();
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->PoisonLocked(owner);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
        if (facade) {
            const GUID key = guid;
            if (flags == D3DSPD_IUNKNOWN && size == sizeof(IUnknown*)) {
                LastError error;
                auto bridge = PrivateBridge();
                PrivateObjectPin pin;
                if (!bridge || !bridge->Pin(data, pin) || !pin.native || !pin.guard ||
                    pin.kind == PrivateDataKind::Blob) return E_NOTIMPL;
                error.restore();
                const HRESULT hr = private_blobs_.SetTyped(key, pin.kind,
                    [&] { return real_->SetPrivateData(key, pin.native, size, flags); });
                error.capture();
                return hr;
            }
            return private_blobs_.Set(key, flags, [&] { return real_->SetPrivateData(key, data, size, flags); });
        }
        LastError error; error.restore();
        const HRESULT hr = real_->SetPrivateData(guid, data, size, flags); error.capture();
        if (SUCCEEDED(hr) && (flags & D3DSPD_IUNKNOWN)) {
            std::lock_guard<std::mutex> lock(state_->mutex); state_->PoisonLocked(owner);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
        if (facade) {
            const GUID key = guid;
            return private_blobs_.GetTyped(key, [&](PrivateDataKind kind) {
                if (kind == PrivateDataKind::Blob) return real_->GetPrivateData(key, data, size);
                auto bridge = PrivateBridge();
                if (!bridge) return E_NOTIMPL;
                IUnknown* native = nullptr;
                const HRESULT hr = real_->GetPrivateData(key, data ? &native : nullptr, size);
                LastError error;
                if (FAILED(hr) || !data) return hr;
                if (!native || !size || *size != sizeof(IUnknown*)) {
                    if (native) native->Release();
                    return E_FAIL;
                }
                IUnknown* mapped = bridge->Adopt(kind, native);
                if (!mapped) return E_FAIL;
                std::memcpy(data, &mapped, sizeof(mapped));
                return hr;
            });
        }
        LastError error; error.restore();
        const HRESULT hr = real_->GetPrivateData(guid, data, size); error.capture();
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(state_->mutex); state_->PoisonLocked(owner);
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override {
        if (facade) {
            const GUID key = guid;
            return private_blobs_.Free(key, [&] { return real_->FreePrivateData(key); });
        }
        return real_->FreePrivateData(guid);
    }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override { return real_->SetPriority(priority); }
    DWORD STDMETHODCALLTYPE GetPriority() override { return real_->GetPriority(); }
    void STDMETHODCALLTYPE PreLoad() override { real_->PreLoad(); }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return real_->GetType(); }
    HRESULT STDMETHODCALLTYPE GetDesc(typename Traits<Buffer>::Desc* desc) override { return real_->GetDesc(desc); }

    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT bytes, BYTE** output, DWORD flags) override {
        LastError error;
        const auto id = BeginLock(offset, bytes, flags);
        error.restore();
        const HRESULT hr = real_->Lock(offset, bytes, output, flags); error.capture();
        BYTE* data = nullptr;
        bool copied = false;
        if (id && SUCCEEDED(hr) && output) {
            try { copied = state_->reader(state_->context, output, &data, sizeof(data)) && data; }
            catch (...) { copied = false; }
        }
        FinishLock(id, hr, copied, data);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {
        LastError error;
        const auto id = BeginUnlock();
        error.restore();
        const HRESULT hr = real_->Unlock(); error.capture();
        FinishUnlock(id, hr);
        return hr;
    }
    // Destroy an unpublished wrapper without consuming its native reference.
    void DiscardUnpublished() noexcept { delete this; }

private:
    std::shared_ptr<PrivateObjectBridge> PrivateBridge() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto found = state_->devices.find(owner);
        if (found == state_->devices.end() || found->second.closing || found->second.private_facade != facade) return {};
        return found->second.private_bridge.lock();
    }
    ULONG ReleaseFacade() {
        LastError error;
        bool final = false;
        ULONG result = 0;
        IDirect3DDevice8* parent = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ULONG count = refs_.load(std::memory_order_relaxed);
            for (;;) {
                if (!count || count == std::numeric_limits<ULONG>::max()) return count;
                if (refs_.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) break;
            }
            result = count - 1;
            final = count == 1;
            const auto device = state_->devices.find(owner);
            if (final && device != state_->devices.end() && device->second.reset) state_->PoisonLocked(owner);
            if (final && counted_lock) state_->PoisonLocked(owner);
            if (final) parent = facade;
        }
        // Parent final Release may call CloseFacadeDevice and delete this.
        if (parent) parent->Release();
        return result;
    }
    HRESULT GetFacadeDevice(IDirect3DDevice8** output) {
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
            try { actual_id = Canonical(actual); owner_id = Canonical(owner); } catch (...) {}
            actual->Release();
        }
        if (!output || !actual_id || actual_id != owner_id || !facade->AddRef()) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->PoisonLocked(owner);
            return E_FAIL;
        }
        *output = facade;
        return hr;
    }
    static constexpr std::size_t kSnapshotLimit = 256u * 1024u;
    struct ReadContext { SnapshotReader reader; void* context; const void* source; };
    static bool Read(void* opaque, void* destination, std::size_t size) {
        auto& value = *static_cast<ReadContext*>(opaque);
        return value.reader(value.context, value.source, destination, size);
    }
    std::uint64_t BeginLock(UINT offset, UINT bytes, DWORD flags) noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto* device = state_->DeviceLocked(owner);
        if (!device || device->unsafe || state_->global_unsafe) return 0;
        if (device->draw || device->mutation || device->reset || device->reset_recovery) {
            state_->PoisonLocked(owner); return 0;
        }
        constexpr DWORD allowed = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD;
        if ((flags & ~allowed) || ((flags & D3DLOCK_READONLY) && (flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) ||
            ((flags & D3DLOCK_DISCARD) && (flags & D3DLOCK_NOOVERWRITE)) ||
            pending_lock || locked || pending_unlock || !state_->next_operation ||
            device->active_locks == std::numeric_limits<std::size_t>::max()) {
            // The native call is still forwarded and may succeed. Its lock
            // window cannot be represented by our shadow state.
            state_->PoisonLocked(owner); return 0;
        }
        const std::size_t length_ = bytes ? bytes : (offset == 0 ? size_ : 0);
        const auto kind = flags & D3DLOCK_READONLY ? ir::BufferLock::ReadOnly :
                          flags & D3DLOCK_DISCARD ? ir::BufferLock::Discard : ir::BufferLock::Write;
        if (token.identity && token.generation &&
            state_->shadows.BeginLock(token, offset, length_, kind) != ir::RegistryResult::Ok) {
            state_->PoisonLocked(owner); return 0;
        }
        operation = state_->next_operation++;
        pending_lock = true; read_only = kind == ir::BufferLock::ReadOnly; length = length_;
        counted_lock = true; ++device->active_locks;
        return operation;
    }
    void FinishLock(std::uint64_t id, HRESULT hr, bool copied, const void* mapped) noexcept {
        if (!id) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (operation != id || !pending_lock) return;
        if (FAILED(hr)) { state_->AbandonLocked(this); return; }
        // A successful native Lock remains open even if its returned pointer
        // is inaccessible to the observer. Do not report later draws stable.
        if (!copied) { state_->PoisonLocked(owner); return; }
        pending_lock = false; locked = true; data = mapped;
    }
    std::uint64_t BeginUnlock() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto* device = state_->DeviceLocked(owner);
        if (!device || device->unsafe || state_->global_unsafe) return 0;
        if (device->draw || device->mutation || device->reset || device->reset_recovery) {
            state_->PoisonLocked(owner); return 0;
        }
        if (pending_lock || !locked || pending_unlock || !state_->next_operation) {
            state_->PoisonLocked(owner); return 0;
        }
        operation = state_->next_operation++;
        pending_unlock = true;
        if (token.identity && token.generation && !read_only && length <= kSnapshotLimit) {
            ReadContext read{state_->reader, state_->context, data};
            state_->shadows.StageUnlockFromReader(token, Read, &read);
        }
        return operation;
    }
    void FinishUnlock(std::uint64_t id, HRESULT hr) noexcept {
        if (!id) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (operation != id || !pending_unlock) return;
        if (FAILED(hr)) { state_->PoisonLocked(owner); return; }
        if (token.identity && token.generation) state_->shadows.EndUnlock(token, true);
        state_->ClearLockLocked(this);
    }

    ~Wrapper() = default;
    std::shared_ptr<BufferProxy::State> state_;
    Buffer* const real_;
    const UINT size_;
    std::atomic<ULONG> refs_{1};
    PrivateBlobState private_blobs_;
};

template<class Buffer>
Buffer* Adopt(const std::shared_ptr<BufferProxy::State>& state, IDirect3DDevice8* owner,
              IDirect3DDevice8* facade, Buffer* native, UINT size, bool fresh, bool closed) noexcept {
    if (!native) return nullptr;
    LastError error;
    const auto fallback = [&]() -> Buffer* {
        if (!closed) return native;
        native->Release();
        return nullptr;
    };
    if (!state || !owner || (closed && !facade)) {
        if (state) { std::lock_guard<std::mutex> lock(state->mutex); state->PoisonAllLocked(); }
        return fallback();
    }
    void* identity = nullptr;
    try { identity = Canonical(native); } catch (...) { identity = nullptr; }
    if (!identity) {
        { std::lock_guard<std::mutex> lock(state->mutex); state->PoisonLocked(owner); }
        return fallback();
    }
    bool facade_ref = false;
    if (facade) {
        facade_ref = facade->AddRef() != 0;
        if (!facade_ref) {
            { std::lock_guard<std::mutex> lock(state->mutex); state->PoisonLocked(owner); }
            return fallback();
        }
    }
    Wrapper<Buffer>* candidate = nullptr;
    if (fresh || closed) {
        candidate = new (std::nothrow) Wrapper<Buffer>(state, owner, facade, native, identity, size);
    }
    Control* found = nullptr;
    bool published = false, revived = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto* device = state->DeviceLocked(owner);
        if (device && (device->reset || device->reset_recovery)) state->PoisonLocked(owner);
        const auto recorded_device = state->devices.find(owner);
        const bool closing = recorded_device != state->devices.end() && recorded_device->second.closing;
        auto existing = state->identities.find(identity);
        if (existing != state->identities.end()) {
            Control* item = existing->second;
            if (item->owner != owner || item->facade != facade ||
                item->vertex != Traits<Buffer>::kVertex || (facade && closing)) {
                state->PoisonLocked(owner);
                if (item->owner != owner) state->PoisonLocked(item->owner);
            } else if (item->TryAddRef() || (facade && (revived = item->TryRevive()))) {
                found = item;
                if (fresh) state->PoisonLocked(owner);  // duplicate creation is ambiguous
            } else state->PoisonLocked(owner);
        } else if (!closing && candidate && state->wrappers.size() < state->maximum &&
                   (closed || (device && !device->unsafe && !state->global_unsafe && fresh))) {
            const bool can_track = device && !device->unsafe && !state->global_unsafe && fresh;
            bool registered = false;
            if (can_track) {
                ir::BufferAvailability availability{};
                registered = state->shadows.Register(reinterpret_cast<std::uintptr_t>(identity), size,
                                                      candidate->token, availability) == ir::RegistryResult::Ok;
            }
            // In a closed facade a getter may return a previously retired
            // resource. Keep its identity and lock windows, but never infer
            // its contents. Allocation or registration failures still poison.
            if (!registered && !(closed && !fresh && device && !device->unsafe && !state->global_unsafe))
                state->PoisonLocked(owner);
            if (registered || closed) {
                try {
                    const bool wrapper_added = state->wrappers.emplace(candidate->exposed, candidate).second;
                    const bool identity_added = state->identities.emplace(identity, candidate).second;
                    const bool native_added = state->native.emplace(native, candidate).second;
                    if (!wrapper_added || !identity_added || !native_added) {
                        state->DetachLocked(candidate);
                        state->PoisonLocked(owner);
                    } else {
                        found = candidate;
                        candidate = nullptr;
                        published = true;
                    }
                } catch (...) {
                    state->DetachLocked(candidate);
                    state->PoisonLocked(owner);
                }
            }
        } else state->PoisonLocked(owner);
    }
    if (candidate) candidate->DiscardUnpublished();
    if (facade_ref && !published && !revived) facade->Release();
    if (!found) return fallback();
    // A newly published wrapper adopts native's returned reference. An
    // existing wrapper already owns one, so release the extra Create/Get ref.
    if (!published) native->Release();
    return static_cast<Buffer*>(found->exposed);
}
}  // namespace

std::shared_ptr<BufferProxy> BufferProxy::Create(SnapshotReader reader, void* context,
                                                 std::size_t budget, std::size_t max_resources) noexcept {
    LastError error;
    if (!reader || !max_resources) return {};
    try {
        auto state = std::make_shared<State>(reader, context, budget, max_resources);
        return std::shared_ptr<BufferProxy>(new BufferProxy(std::move(state)));
    } catch (...) { return {}; }
}

IDirect3DVertexBuffer8* BufferProxy::AdoptVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                                 UINT size, bool fresh) noexcept {
    return Adopt(state_, owner, nullptr, native, size, fresh, false);
}
IDirect3DIndexBuffer8* BufferProxy::AdoptIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                               UINT size, bool fresh) noexcept {
    return Adopt(state_, owner, nullptr, native, size, fresh, false);
}
IDirect3DVertexBuffer8* BufferProxy::AdoptVertexForFacade(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                                                          IDirect3DVertexBuffer8* native, UINT size, bool fresh) noexcept {
    return Adopt(state_, owner, facade, native, size, fresh, true);
}
IDirect3DIndexBuffer8* BufferProxy::AdoptIndexForFacade(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                                                        IDirect3DIndexBuffer8* native, UINT size, bool fresh) noexcept {
    return Adopt(state_, owner, facade, native, size, fresh, true);
}

bool BufferProxy::CloseFacadeDevice(IDirect3DDevice8* owner, IDirect3DDevice8* facade) noexcept {
    LastError error;
    if (!state_ || !owner || !facade) return false;
    Control* retired = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        (void)state_->DeviceLocked(owner); // record a close even before the first buffer exists
        auto device = state_->devices.find(owner);
        if (device != state_->devices.end()) device->second.closing = true;
        state_->PoisonLocked(owner);
        for (const auto& entry : state_->wrappers) {
            const Control* item = entry.second;
            if (item->owner == owner && item->facade == facade && !item->Dormant()) return false;
        }
        for (auto it = state_->wrappers.begin(); it != state_->wrappers.end();) {
            Control* item = (it++)->second;
            if (item->owner == owner && item->facade == facade) {
                state_->DetachLocked(item);
                item->retirement_next = retired;
                retired = item;
            }
        }
    }
    while (retired) {
        Control* next = retired->retirement_next;
        retired->DestroyDormant();
        retired = next;
    }
    return true;
}

void BufferProxy::RetireFacadeDormantForReset(IDirect3DDevice8* owner, IDirect3DDevice8* facade) noexcept {
    LastError error;
    if (!state_ || !owner || !facade) return;
    Control* retired = nullptr;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->PoisonLocked(owner);
        for (auto it = state_->wrappers.begin(); it != state_->wrappers.end();) {
            Control* item = (it++)->second;
            if (item->owner == owner && item->facade == facade && item->Dormant()) {
                state_->DetachLocked(item);
                item->retirement_next = retired;
                retired = item;
            }
        }
    }
    while (retired) {
        Control* next = retired->retirement_next;
        retired->DestroyDormant();
        retired = next;
    }
}

std::uint64_t BufferProxy::BeginFacadeReset(IDirect3DDevice8* owner, IDirect3DDevice8* facade) noexcept {
    LastError error;
    if (!state_ || !owner || !facade) return 0;
    Control* retired = nullptr;
    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto* device = state_->DeviceLocked(owner);
        if (!device || device->unsafe || device->closing || state_->global_unsafe) return 0;
        if (device->draw || device->mutation || device->reset || device->active_locks || !state_->next_reset) {
            state_->PoisonLocked(owner); return 0;
        }
        for (const auto& entry : state_->wrappers) {
            const Control* item = entry.second;
            if (item->owner == owner && item->facade != facade) { state_->PoisonLocked(owner); return 0; }
        }
        ticket = state_->next_reset++;
        device->reset = ticket;
        for (auto it = state_->wrappers.begin(); it != state_->wrappers.end();) {
            Control* item = (it++)->second;
            if (item->owner != owner) continue;
            state_->shadows.Release(item->token);
            item->token = {};
            if (item->Dormant()) {
                state_->DetachLocked(item);
                item->retirement_next = retired;
                retired = item;
            }
        }
    }
    while (retired) {
        Control* next = retired->retirement_next;
        retired->DestroyDormant();
        retired = next;
    }
    return ticket;
}

bool BufferProxy::EndFacadeReset(IDirect3DDevice8* owner, std::uint64_t ticket, bool succeeded) noexcept {
    LastError error;
    if (!state_ || !owner || !ticket) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto device = state_->devices.find(owner);
    if (state_->global_unsafe || device == state_->devices.end() || device->second.unsafe) return false;
    if (device->second.reset != ticket) { state_->PoisonLocked(owner); return false; }
    device->second.reset = 0;
    device->second.reset_recovery = !succeeded;
    return true;
}

namespace {
template<class Buffer>
IUnknown* Unwrap(const std::shared_ptr<BufferProxy::State>& state, IDirect3DDevice8* owner,
                 Buffer* input, Buffer*& output) noexcept {
    output = input;
    if (!state || !input) return nullptr;
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto found = state->wrappers.find(input);
    if (found == state->wrappers.end()) {
        // A native or unknown interface may bypass future lock observation.
        state->PoisonLocked(owner);
        return nullptr;
    }
    Control* item = found->second;
    if (item->vertex != Traits<Buffer>::kVertex || !item->TryAddRef()) {
        // A matching issued wrapper must never be passed to Wine. The zero-ref
        // case means the caller raced final Release, which is invalid COM use;
        // return null to avoid forwarding a dangling wrapper.
        state->PoisonLocked(owner);
        output = nullptr;
        return nullptr;
    }
    if (item->owner != owner) {
        state->PoisonLocked(owner);
        state->PoisonLocked(item->owner);
    }
    output = static_cast<Buffer*>(item->native);
    return item->Unknown();
}

template<class Buffer>
ir::RegistryResult Read(const std::shared_ptr<BufferProxy::State>& state, IDirect3DDevice8* owner,
                        Buffer* native, std::size_t offset, void* destination, std::size_t length) noexcept {
    if (!state || !owner || !native) return ir::RegistryResult::UnknownResource;
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto device = state->devices.find(owner);
    if (state->global_unsafe || device == state->devices.end() || device->second.unsafe ||
        device->second.reset || device->second.reset_recovery)
        return ir::RegistryResult::Unavailable;
    const auto found = state->native.find(native);
    if (found == state->native.end() || found->second->owner != owner ||
        found->second->vertex != Traits<Buffer>::kVertex) return ir::RegistryResult::UnknownResource;
    if (!found->second->token.identity || !found->second->token.generation) return ir::RegistryResult::UnknownResource;
    return state->shadows.Read(found->second->token, offset, destination, length);
}

void ReleaseGuard(IUnknown* guard) noexcept {
    if (!guard) return;
    LastError error;
    guard->Release();
}
}  // namespace

BufferProxy::VertexPin::~VertexPin() { ReleaseGuard(guard_); }
BufferProxy::VertexPin::VertexPin(VertexPin&& other) noexcept : native_(other.native_), guard_(other.guard_) {
    other.native_ = nullptr; other.guard_ = nullptr;
}
BufferProxy::VertexPin& BufferProxy::VertexPin::operator=(VertexPin&& other) noexcept {
    if (this != &other) {
        ReleaseGuard(guard_); native_ = other.native_; guard_ = other.guard_;
        other.native_ = nullptr; other.guard_ = nullptr;
    }
    return *this;
}
BufferProxy::IndexPin::~IndexPin() { ReleaseGuard(guard_); }
BufferProxy::IndexPin::IndexPin(IndexPin&& other) noexcept : native_(other.native_), guard_(other.guard_) {
    other.native_ = nullptr; other.guard_ = nullptr;
}
BufferProxy::IndexPin& BufferProxy::IndexPin::operator=(IndexPin&& other) noexcept {
    if (this != &other) {
        ReleaseGuard(guard_); native_ = other.native_; guard_ = other.guard_;
        other.native_ = nullptr; other.guard_ = nullptr;
    }
    return *this;
}
BufferProxy::VertexPin BufferProxy::UnwrapVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* input) noexcept {
    LastError error;
    IDirect3DVertexBuffer8* native = nullptr;
    IUnknown* guard = Unwrap(state_, owner, input, native);
    return VertexPin(native, guard);
}
BufferProxy::IndexPin BufferProxy::UnwrapIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* input) noexcept {
    LastError error;
    IDirect3DIndexBuffer8* native = nullptr;
    IUnknown* guard = Unwrap(state_, owner, input, native);
    return IndexPin(native, guard);
}
IUnknown* BufferProxy::PinPrivateBuffer(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                                         const void* input, IUnknown** native, bool* vertex) noexcept {
    LastError error;
    if (!native || !vertex) return nullptr;
    *native = nullptr;
    *vertex = false;
    if (!state_ || !owner || !facade || !input) return nullptr;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->wrappers.find(const_cast<void*>(input));
    if (found == state_->wrappers.end()) return nullptr;
    Control* item = found->second;
    if (item->owner != owner || item->facade != facade || !item->TryAddRef()) return nullptr;
    *vertex = item->vertex;
    *native = item->vertex ? static_cast<IUnknown*>(static_cast<IDirect3DVertexBuffer8*>(item->native)) :
                            static_cast<IUnknown*>(static_cast<IDirect3DIndexBuffer8*>(item->native));
    return item->Unknown(); // owned wrapper reference, released outside the lock
}
bool BufferProxy::InstallFacadePrivateBridge(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                                              const std::shared_ptr<PrivateObjectBridge>& bridge) noexcept {
    LastError error;
    if (!state_ || !owner || !facade || !bridge) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto* device = state_->DeviceLocked(owner);
    if (!device || device->closing || device->private_facade) return false;
    device->private_facade = facade;
    device->private_bridge = bridge;
    return true;
}
void BufferProxy::StopObservation() noexcept {
    LastError error;
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->PoisonAllLocked();
}
void BufferProxy::InvalidateDevice(IDirect3DDevice8* owner) noexcept {
    LastError error;
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->PoisonLocked(owner);
}
void BufferProxy::InvalidateVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* input) noexcept {
    LastError error;
    if (!state_ || !input) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    Control* item = nullptr;
    const auto wrapper = state_->wrappers.find(input);
    if (wrapper != state_->wrappers.end()) item = wrapper->second;
    else {
        const auto native = state_->native.find(input);
        if (native != state_->native.end()) item = native->second;
    }
    if (!item || !item->vertex || item->owner != owner) {
        state_->PoisonLocked(owner);
        if (item && item->owner != owner) state_->PoisonLocked(item->owner);
        return;
    }
    const auto device = state_->devices.find(owner);
    if (device != state_->devices.end() && (device->second.draw || device->second.active_locks ||
                                             device->second.reset || device->second.reset_recovery))
        state_->PoisonLocked(owner);
    else state_->AbandonLocked(item);
}
std::uint64_t BufferProxy::BeginDraw(IDirect3DDevice8* owner) noexcept {
    LastError error;
    if (!state_) return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto* device = state_->DeviceLocked(owner);
    if (!device || device->unsafe || state_->global_unsafe) return 0;
    if (device->draw || device->mutation || device->reset || device->reset_recovery ||
        device->active_locks || !state_->next_draw) {
        state_->PoisonLocked(owner); return 0;
    }
    device->draw = state_->next_draw++;
    return device->draw;
}
bool BufferProxy::EndDraw(IDirect3DDevice8* owner, std::uint64_t token) noexcept {
    LastError error;
    if (!state_ || !owner || !token) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto device = state_->devices.find(owner);
    if (state_->global_unsafe || device == state_->devices.end() || device->second.unsafe) return false;
    if (device->second.draw != token) { state_->PoisonLocked(owner); return false; }
    device->second.draw = 0;
    return true;
}
std::uint64_t BufferProxy::BeginBindingMutation(IDirect3DDevice8* owner) noexcept {
    LastError error;
    if (!state_) return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto* device = state_->DeviceLocked(owner);
    if (!device || device->unsafe || state_->global_unsafe) return 0;
    if (device->draw || device->mutation || device->reset || device->reset_recovery ||
        device->active_locks || !state_->next_operation) {
        state_->PoisonLocked(owner); return 0;
    }
    device->mutation = state_->next_operation++;
    return device->mutation;
}
void BufferProxy::EndBindingMutation(IDirect3DDevice8* owner, std::uint64_t token) noexcept {
    LastError error;
    if (!state_ || !owner || !token) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto device = state_->devices.find(owner);
    if (device == state_->devices.end() || device->second.unsafe || state_->global_unsafe) return;
    if (device->second.mutation != token) { state_->PoisonLocked(owner); return; }
    device->second.mutation = 0;
}
ir::RegistryResult BufferProxy::ReadVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                            std::size_t offset, void* destination, std::size_t length) noexcept {
    LastError error;
    return Read(state_, owner, native, offset, destination, length);
}
ir::RegistryResult BufferProxy::ReadIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                           std::size_t offset, void* destination, std::size_t length) noexcept {
    LastError error;
    return Read(state_, owner, native, offset, destination, length);
}

}  // namespace t3render::buffers
