#include "buffer_proxy.hpp"
#include "private_blob.hpp"
#include "private_marker.hpp"

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
#if defined(__GNUC__) || defined(__clang__)
#define T3RENDER_CALL_LINE __builtin_LINE()
#else
#define T3RENDER_CALL_LINE 0u
#endif

const char* CauseName(InvalidationCause cause) noexcept {
    switch (cause) {
    case InvalidationCause::Internal: return "internal";
    case InvalidationCause::Explicit: return "explicit";
    case InvalidationCause::StateBlock: return "state_block";
    case InvalidationCause::ProcessVertices: return "process_vertices";
    case InvalidationCause::ExternalPrivateData: return "external_private_data";
    case InvalidationCause::UnlockMismatch: return "unlock_mismatch";
    }
    return "unknown";
}
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
    bool persistent = false;
    bool marker_enabled = false;
    t3render::markers::PrivateMarker* marker = nullptr; // owned by the wrapper
    ir::BufferToken token{};
    bool pending_lock = false, locked = false, pending_unlock = false, read_only = false;
    bool counted_lock = false;
    bool matched_unlock_completed = false, repeated_unlock_pending = false;
    bool last_lock_known = false;
    std::int32_t last_lock_hr = 0;
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

struct PersistentShadow {
    ir::BufferToken token{};
    bool vertex = false;
};
struct DeviceState {
    bool unsafe = false;
    unsigned poison_line = 0;
    InvalidationCause poison_cause = InvalidationCause::Internal;
    UnlockFailure unlock_failure;
    void* unlock_failure_native = nullptr; // identity comparison only; never dereferenced
    bool closing = false;
    bool reset_recovery = false;
    std::uint64_t draw = 0, mutation = 0, reset = 0;
    std::size_t active_locks = 0;
    std::uint64_t repeated_unlocks = 0;
    IDirect3DDevice8* private_facade = nullptr; // borrowed; live buffer keeps it alive
    std::weak_ptr<PrivateObjectBridge> private_bridge;
    bool retain_buffers = true;
    std::weak_ptr<t3render::markers::PrivateMarkerStore> markers;
    std::unordered_map<void*, PersistentShadow> persistent;
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
    bool allow_repeated_unlock = false;
    unsigned global_poison_line = 0;

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
        item->matched_unlock_completed = false; item->repeated_unlock_pending = false;
        item->data = nullptr; item->length = 0; item->operation = 0;
    }
    void AbandonLocked(Control* item) noexcept {
        shadows.Abandon(item->token);
        ClearLockLocked(item);
    }
    void PoisonAllLocked(unsigned line = T3RENDER_CALL_LINE) noexcept {
        if (global_unsafe) return;
        global_unsafe = true;
        global_poison_line = line;
        for (auto& entry : wrappers) AbandonLocked(entry.second);
        for (auto& entry : devices) {
            entry.second.unsafe = true; entry.second.draw = 0;
            entry.second.mutation = 0; entry.second.reset = 0;
            entry.second.reset_recovery = false; entry.second.active_locks = 0;
            for (const auto& shadow : entry.second.persistent) shadows.Abandon(shadow.second.token);
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
    void PoisonLocked(IDirect3DDevice8* owner, unsigned line = T3RENDER_CALL_LINE,
                      InvalidationCause cause = InvalidationCause::Internal) noexcept {
        auto* device = DeviceLocked(owner);
        if (!device || device->unsafe) return;
        device->unsafe = true; device->poison_line = line; device->poison_cause = cause;
        device->draw = 0; device->mutation = 0;
        device->reset = 0; device->reset_recovery = false;
        for (const auto& shadow : device->persistent) shadows.Abandon(shadow.second.token);
        for (auto& entry : wrappers)
            if (entry.second->owner == owner) AbandonLocked(entry.second);
    }
    void DetachLocked(Control* item, bool preserve = false) noexcept {
        auto it = wrappers.find(item->exposed);
        if (it != wrappers.end() && it->second == item) wrappers.erase(it);
        it = identities.find(item->identity);
        if (it != identities.end() && it->second == item) identities.erase(it);
        it = native.find(item->native);
        if (it != native.end() && it->second == item) native.erase(it);
        if (!preserve) {
            if (item->persistent) {
                const auto device = devices.find(item->owner);
                if (device != devices.end()) {
                    const auto found = device->second.persistent.find(item->identity);
                    if (found != device->second.persistent.end() && found->second.token.generation == item->token.generation)
                        device->second.persistent.erase(found);
                }
            }
            shadows.Release(item->token);
        }
    }
    void ClearPersistentLocked(DeviceState& device) noexcept {
        for (const auto& entry : device.persistent) shadows.Release(entry.second.token);
        device.persistent.clear();
    }
};

namespace {
struct BufferLease final {
    std::weak_ptr<BufferProxy::State> state;
    IDirect3DDevice8* owner = nullptr;
    ir::BufferToken token{};
    bool vertex = false;
    ~BufferLease() {
        auto shared = state.lock();
        if (!shared || !token.identity || !token.generation) return;
        std::lock_guard<std::mutex> lock(shared->mutex);
        const auto device = shared->devices.find(owner);
        if (device == shared->devices.end()) return;
        const auto found = device->second.persistent.find(reinterpret_cast<void*>(token.identity));
        if (found != device->second.persistent.end() && found->second.token.identity == token.identity &&
            found->second.token.generation == token.generation) {
            shared->shadows.Release(token);
            device->second.persistent.erase(found);
        }
    }
};

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
            if (marker_enabled && IsEqualIID(key, t3render::markers::kPrivateMarkerGuid)) return E_NOTIMPL;
            if (flags == D3DSPD_IUNKNOWN && size == sizeof(IUnknown*)) {
                LastError error;
                auto bridge = PrivateBridge();
                PrivateObjectPin pin;
                if (!bridge) return E_NOTIMPL;
                if (!bridge->Pin(data, pin)) {
                    if (!marker_enabled || !marker || !data) return E_NOTIMPL;
                    {
                        std::lock_guard<std::mutex> lock(state_->mutex);
                        state_->PoisonLocked(owner, __LINE__, InvalidationCause::ExternalPrivateData);
                    }
                    // The facade may also be observing shader or other state;
                    // notify it outside the buffer mutex before forwarding an
                    // external object capable of holding hidden aliases.
                    bridge->ExternalObserved();
                    error.restore();
                    const HRESULT hr = PrivateState().SetTyped(key, PrivateDataKind::ExternalUnknown,
                        [&] { return real_->SetPrivateData(key, data, size, flags); });
                    error.capture();
                    return hr;
                }
                if (!pin.native || !pin.guard || pin.kind == PrivateDataKind::Blob) return E_NOTIMPL;
                error.restore();
                const HRESULT hr = PrivateState().SetTyped(key, pin.kind,
                    [&] { return real_->SetPrivateData(key, pin.native, size, flags); });
                error.capture();
                return hr;
            }
            return PrivateState().Set(key, flags, [&] { return real_->SetPrivateData(key, data, size, flags); });
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
            if (marker_enabled && IsEqualIID(key, t3render::markers::kPrivateMarkerGuid)) return E_NOTIMPL;
            return PrivateState().GetTyped(key, [&](PrivateDataKind kind) {
                if (kind == PrivateDataKind::Blob) return real_->GetPrivateData(key, data, size);
                if (kind == PrivateDataKind::ExternalUnknown) return real_->GetPrivateData(key, data, size);
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
            if (marker_enabled && IsEqualIID(key, t3render::markers::kPrivateMarkerGuid)) return E_NOTIMPL;
            return PrivateState().Free(key, [&] { return real_->FreePrivateData(key); });
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
    PrivateBlobState& PrivateState() noexcept { return marker ? marker->state : private_blobs_; }
    std::shared_ptr<PrivateObjectBridge> PrivateBridge() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto found = state_->devices.find(owner);
        if (found == state_->devices.end() || found->second.closing || found->second.private_facade != facade) return {};
        return found->second.private_bridge.lock();
    }
    ULONG ReleaseFacade() {
        LastError error;
        bool final = false;
        bool retire = false;
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
            if (final) {
                parent = facade;
                if (device != state_->devices.end() && !device->second.retain_buffers) {
                    state_->DetachLocked(this, persistent);
                    retire = true;
                }
            }
        }
        if (retire) {
            Buffer* real = real_;
            delete this;
            real->Release();
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
        if (token.identity && token.generation) {
            const auto result = state_->shadows.BeginLock(token, offset, length_, kind);
            ir::BufferAvailability availability{};
            const bool discarded = state_->allow_repeated_unlock && result == ir::RegistryResult::Unavailable &&
                state_->shadows.Availability(token, availability) == ir::RegistryResult::Ok &&
                availability == ir::BufferAvailability::Abandoned;
            if (result != ir::RegistryResult::Ok && !discarded) {
                state_->PoisonLocked(owner); return 0;
            }
        }
        operation = state_->next_operation++;
        matched_unlock_completed = false;
        pending_lock = true; read_only = kind == ir::BufferLock::ReadOnly; length = length_;
        counted_lock = true; ++device->active_locks;
        return operation;
    }
    void FinishLock(std::uint64_t id, HRESULT hr, bool copied, const void* mapped) noexcept {
        if (!id) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (operation != id || !pending_lock) return;
        last_lock_known = true;
        last_lock_hr = static_cast<std::int32_t>(hr);
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
            if (state_->allow_repeated_unlock && facade && matched_unlock_completed && last_lock_known &&
                last_lock_hr >= 0 && !pending_lock && !locked && !pending_unlock && state_->next_operation &&
                !device->active_locks && device->repeated_unlocks != std::numeric_limits<std::uint64_t>::max()) {
                // The repeated call can touch the source resource, so discard
                // its bytes before forwarding. The mutation ticket detects
                // observed overlaps; uncaptured draws are not bracketed.
                state_->AbandonLocked(this);
                operation = state_->next_operation++;
                pending_unlock = true;
                repeated_unlock_pending = true;
                matched_unlock_completed = false;
                device->mutation = operation;
                return operation;
            }
            UnlockFailure diagnostic;
            diagnostic.present = true;
            diagnostic.vertex = vertex;
            diagnostic.pending_lock = pending_lock;
            diagnostic.locked = locked;
            diagnostic.pending_unlock = pending_unlock;
            diagnostic.operation_exhausted = !state_->next_operation;
            diagnostic.last_lock_known = last_lock_known;
            diagnostic.last_lock_hr = last_lock_hr;
            state_->PoisonLocked(owner, __LINE__, InvalidationCause::UnlockMismatch);
            if (device->unsafe && device->poison_cause == InvalidationCause::UnlockMismatch &&
                !device->unlock_failure.present) {
                device->unlock_failure = diagnostic;
                device->unlock_failure_native = native;
            }
            return 0;
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
        if (!id) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            const auto found = state_->devices.find(owner);
            if (found != state_->devices.end() && found->second.poison_cause == InvalidationCause::UnlockMismatch &&
                found->second.unlock_failure_native == native && found->second.unlock_failure.present &&
                !found->second.unlock_failure.unlock_result_known) {
                found->second.unlock_failure.unlock_result_known = true;
                found->second.unlock_failure.unlock_hr_candidate = static_cast<std::int32_t>(hr);
            }
            return;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (operation != id || !pending_unlock) return;
        if (repeated_unlock_pending) {
            const auto device = state_->devices.find(owner);
            if (device == state_->devices.end() || device->second.unsafe || state_->global_unsafe ||
                device->second.mutation != id || FAILED(hr)) {
                state_->PoisonLocked(owner);
                return;
            }
            device->second.mutation = 0;
            ++device->second.repeated_unlocks;
            pending_unlock = false;
            repeated_unlock_pending = false;
            operation = 0;
            return;
        }
        if (FAILED(hr)) { state_->PoisonLocked(owner); return; }
        if (token.identity && token.generation) state_->shadows.EndUnlock(token, true);
        state_->ClearLockLocked(this);
        matched_unlock_completed = true;
    }

    ~Wrapper() { if (marker) marker->Release(); }
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
    std::shared_ptr<t3render::markers::PrivateMarkerStore> marker_store;
    if (closed) {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto device = state->devices.find(owner);
        if (device != state->devices.end() && device->second.private_facade == facade &&
            !device->second.retain_buffers && !device->second.closing)
            marker_store = device->second.markers.lock();
    }
    auto* marker = marker_store ? marker_store->Acquire(identity, native, fresh) : nullptr;
    Wrapper<Buffer>* candidate = nullptr;
    if (fresh || closed) {
        candidate = new (std::nothrow) Wrapper<Buffer>(state, owner, facade, native, identity, size);
    }
    std::shared_ptr<BufferLease> lease;
    if (candidate) {
        candidate->marker = marker;
        candidate->marker_enabled = static_cast<bool>(marker_store);
        marker = nullptr;
        if (fresh && candidate->marker) {
            try {
                lease = std::make_shared<BufferLease>();
                lease->state = state; lease->owner = owner; lease->vertex = Traits<Buffer>::kVertex;
            } catch (...) {}
        }
    }
    if (marker) marker->Release();
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
                if (registered && candidate->marker && lease) {
                    try {
                        if (device->persistent.emplace(identity, PersistentShadow{candidate->token,
                                                                                  Traits<Buffer>::kVertex}).second) {
                            lease->token = candidate->token;
                            if (candidate->marker->InstallBufferPayload(lease)) candidate->persistent = true;
                            else { device->persistent.erase(identity); lease->token = {}; }
                        }
                    } catch (...) {}
                }
            } else if (!fresh && device && !device->unsafe && !state->global_unsafe && candidate->marker) {
                auto payload = candidate->marker->BufferPayload();
                auto previous = std::static_pointer_cast<BufferLease>(payload);
                if (previous) {
                    const auto expected = device->persistent.find(identity);
                    const auto previous_state = previous->state.lock();
                    ir::BufferAvailability availability{};
                    if (previous_state == state && previous->owner != owner) {
                        state->PoisonLocked(owner);
                        state->PoisonLocked(previous->owner);
                    }
                    if (previous_state == state && previous->owner == owner && previous->vertex == Traits<Buffer>::kVertex &&
                        expected != device->persistent.end() && expected->second.token.identity == previous->token.identity &&
                        expected->second.token.generation == previous->token.generation &&
                        !device->unsafe && state->shadows.Availability(previous->token, availability) == ir::RegistryResult::Ok) {
                        candidate->token = previous->token;
                        candidate->persistent = true;
                        registered = true;
                    }
                }
            }
            // In a closed facade a getter may return a previously retired
            // resource. Without a matching persistent marker its contents
            // remain unknown. Allocation or registration failures still poison.
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

bool BufferProxy::EnableRepeatedUnlockExperiment() noexcept {
    LastError error;
    if (!state_) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->global_unsafe || !state_->devices.empty() || !state_->wrappers.empty() || state_->allow_repeated_unlock)
        return false;
    state_->allow_repeated_unlock = true;
    return true;
}

BufferProxy::RepeatedUnlockStats BufferProxy::GetRepeatedUnlockStats(IDirect3DDevice8* owner) noexcept {
    LastError error;
    if (!state_) return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto device = state_->devices.find(owner);
    return {state_->allow_repeated_unlock, device == state_->devices.end() ? 0 : device->second.repeated_unlocks};
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
        if (device != state_->devices.end()) state_->ClearPersistentLocked(device->second);
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
        const auto device = state_->devices.find(owner);
        if (device != state_->devices.end()) state_->ClearPersistentLocked(device->second);
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
        state_->ClearPersistentLocked(*device);
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

thread_local bool persistent_identity_query = false;

template<class Buffer>
ir::BufferToken PrepareGetterToken(const std::shared_ptr<BufferProxy::State>& state, IDirect3DDevice8* owner,
                                   Buffer* native, std::uint64_t draw) noexcept {
    if (!state || !owner || !native || !draw) return {};
    std::unique_lock<std::mutex> lock(state->mutex);
    auto device = state->devices.find(owner);
    if (state->global_unsafe || device == state->devices.end() || device->second.unsafe ||
        device->second.reset || device->second.reset_recovery || device->second.draw != draw) return {};
    const auto found = state->native.find(native);
    if (found != state->native.end()) {
        if (found->second->owner != owner || found->second->vertex != Traits<Buffer>::kVertex) return {};
        return found->second->token;
    }
    if (device->second.persistent.empty() || persistent_identity_query) return {};
    lock.unlock();
    struct QueryScope { QueryScope() { persistent_identity_query = true; } ~QueryScope() { persistent_identity_query = false; } } scope;
    void* identity = nullptr;
    try { identity = Canonical(native); } catch (...) {}
    if (!identity) return {};
    lock.lock();
    device = state->devices.find(owner);
    if (state->global_unsafe || device == state->devices.end() || device->second.unsafe ||
        device->second.reset || device->second.reset_recovery || device->second.draw != draw) return {};
    const auto shadow = device->second.persistent.find(identity);
    if (shadow == device->second.persistent.end() || shadow->second.vertex != Traits<Buffer>::kVertex) return {};
    return shadow->second.token;
}

template<class Buffer>
ir::RegistryResult ReadPrepared(const std::shared_ptr<BufferProxy::State>& state, IDirect3DDevice8* expected_owner,
                                Buffer* expected_native, ir::BufferToken token, std::uint64_t draw,
                                IDirect3DDevice8* owner, Buffer* native, std::size_t offset,
                                void* destination, std::size_t length) noexcept {
    if (!state || !owner || owner != expected_owner || !native || native != expected_native ||
        !token.identity || !token.generation || !draw) return ir::RegistryResult::UnknownResource;
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto device = state->devices.find(owner);
    if (state->global_unsafe || device == state->devices.end() || device->second.unsafe ||
        device->second.reset || device->second.reset_recovery || device->second.draw != draw)
        return ir::RegistryResult::Unavailable;
    return state->shadows.Read(token, offset, destination, length);
}

template<class Buffer>
ir::RegistryResult Read(const std::shared_ptr<BufferProxy::State>& state, IDirect3DDevice8* owner,
                        Buffer* native, std::size_t offset, void* destination, std::size_t length,
                        bool trusted_getter) noexcept {
    if (!state || !owner || !native) return ir::RegistryResult::UnknownResource;
    std::unique_lock<std::mutex> lock(state->mutex);
    auto device = state->devices.find(owner);
    if (state->global_unsafe || device == state->devices.end() || device->second.unsafe ||
        device->second.reset || device->second.reset_recovery)
        return ir::RegistryResult::Unavailable;
    const auto found = state->native.find(native);
    if (found == state->native.end()) {
        if (!trusted_getter || device->second.persistent.empty() || persistent_identity_query)
            return ir::RegistryResult::UnknownResource;
        // The caller holds a native getter reference. Canonicalize outside the
        // mutex: an interface pointer can be a tear-off and its address alone
        // is not stable after the last facade reference is released.
        lock.unlock();
        struct QueryScope { QueryScope() { persistent_identity_query = true; } ~QueryScope() { persistent_identity_query = false; } } scope;
        void* identity = nullptr;
        try { identity = Canonical(native); } catch (...) {}
        if (!identity) return ir::RegistryResult::UnknownResource;
        lock.lock();
        device = state->devices.find(owner);
        if (state->global_unsafe || device == state->devices.end() || device->second.unsafe ||
            device->second.reset || device->second.reset_recovery) return ir::RegistryResult::Unavailable;
        const auto shadow = device->second.persistent.find(identity);
        if (shadow == device->second.persistent.end() || shadow->second.vertex != Traits<Buffer>::kVertex)
            return ir::RegistryResult::UnknownResource;
        return state->shadows.Read(shadow->second.token, offset, destination, length);
    }
    if (found->second->owner != owner || found->second->vertex != Traits<Buffer>::kVertex)
        return ir::RegistryResult::UnknownResource;
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
                                              const std::shared_ptr<PrivateObjectBridge>& bridge,
                                              bool retain_buffers,
                                              const std::shared_ptr<t3render::markers::PrivateMarkerStore>& markers) noexcept {
    LastError error;
    if (!state_ || !owner || !facade || !bridge) return false;
    if (markers && (retain_buffers || !markers->BindBufferObserver(state_))) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto* device = state_->DeviceLocked(owner);
    if (!device || device->closing || device->private_facade) return false;
    device->private_facade = facade;
    device->private_bridge = bridge;
    device->retain_buffers = retain_buffers;
    device->markers = markers;
    return true;
}
void BufferProxy::StopObservation() noexcept {
    LastError error;
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->PoisonAllLocked();
}
void BufferProxy::InvalidateDevice(IDirect3DDevice8* owner, InvalidationCause cause) noexcept {
    LastError error;
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->PoisonLocked(owner, __LINE__, cause);
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
DrawFailure BufferProxy::DescribeDrawFailure(IDirect3DDevice8* owner) noexcept {
    LastError error;
    if (!state_) return {"manager_unavailable", "unknown", 0};
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->global_unsafe) return {"global_unsafe", "internal", state_->global_poison_line};
    const auto device = state_->devices.find(owner);
    if (device == state_->devices.end()) return {"device_unknown", "unknown", 0};
    if (device->second.unsafe)
        return {"device_unsafe", CauseName(device->second.poison_cause), device->second.poison_line,
                device->second.unlock_failure};
    return {"window_unavailable", "unknown", 0};
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
    return Read(state_, owner, native, offset, destination, length, false);
}
ir::RegistryResult BufferProxy::ReadIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                           std::size_t offset, void* destination, std::size_t length) noexcept {
    LastError error;
    return Read(state_, owner, native, offset, destination, length, false);
}
ir::RegistryResult BufferProxy::ReadVertexFromGetter(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                                     std::size_t offset, void* destination, std::size_t length) noexcept {
    LastError error;
    return Read(state_, owner, native, offset, destination, length, true);
}
ir::RegistryResult BufferProxy::ReadIndexFromGetter(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                                    std::size_t offset, void* destination, std::size_t length) noexcept {
    LastError error;
    return Read(state_, owner, native, offset, destination, length, true);
}
BufferProxy::PreparedVertex BufferProxy::PrepareVertexGetter(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                                             std::uint64_t draw) noexcept {
    LastError error;
    PreparedVertex result;
    result.token_ = PrepareGetterToken(state_, owner, native, draw);
    if (result.token_.identity && result.token_.generation) {
        result.state_ = state_; result.owner_ = owner; result.native_ = native; result.draw_ = draw;
    }
    return result;
}
BufferProxy::PreparedIndex BufferProxy::PrepareIndexGetter(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                                           std::uint64_t draw) noexcept {
    LastError error;
    PreparedIndex result;
    result.token_ = PrepareGetterToken(state_, owner, native, draw);
    if (result.token_.identity && result.token_.generation) {
        result.state_ = state_; result.owner_ = owner; result.native_ = native; result.draw_ = draw;
    }
    return result;
}
ir::RegistryResult BufferProxy::PreparedVertex::Read(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                                     std::size_t offset, void* destination, std::size_t length) const noexcept {
    LastError error;
    return ReadPrepared(state_, owner_, native_, token_, draw_, owner, native, offset, destination, length);
}
ir::RegistryResult BufferProxy::PreparedIndex::Read(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                                    std::size_t offset, void* destination, std::size_t length) const noexcept {
    LastError error;
    return ReadPrepared(state_, owner_, native_, token_, draw_, owner, native, offset, destination, length);
}

}  // namespace t3render::buffers
