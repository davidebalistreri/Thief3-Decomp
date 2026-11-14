#pragma once

// Fixture-only provenance attached to a native resource. The marker does not
// retain that resource, its facade or its device. Do not use this in TDS.
#include <windows.h>
#include <d3d8.h>
#include "../src/private_blob.hpp"

#ifdef max
#pragma push_macro("max")
#undef max
#define T3RENDER_MARKER_RESTORE_MAX
#endif
#ifdef min
#pragma push_macro("min")
#undef min
#define T3RENDER_MARKER_RESTORE_MIN
#endif
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#ifdef T3RENDER_MARKER_RESTORE_MIN
#pragma pop_macro("min")
#undef T3RENDER_MARKER_RESTORE_MIN
#endif
#ifdef T3RENDER_MARKER_RESTORE_MAX
#pragma pop_macro("max")
#undef T3RENDER_MARKER_RESTORE_MAX
#endif

namespace t3render::resource_fixture {

#ifdef _WIN32
inline const GUID kPrivateMarkerGuid{0x6fe28a31, 0xb561, 0x45e7, {0xb7, 0x19, 0x83, 0x5d, 0x4a, 0x71, 0xc2, 0x90}};
#else
inline constexpr GUID kPrivateMarkerGuid{0x6fe28a31};
#endif

class PrivateMarker;
struct PrivateMarkerCore final {
    explicit PrivateMarkerCore(std::size_t maximum_) noexcept : maximum(maximum_) {}
    std::mutex mutex;
    std::unordered_map<void*, PrivateMarker*> entries; // weak; marker removes itself on final Release
    const std::size_t maximum;
};

class PrivateMarker final : public IUnknown {
public:
    PrivateMarker(std::shared_ptr<PrivateMarkerCore> core, void* identity) noexcept
        : core_(std::move(core)), identity_(identity) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown)) return E_NOINTERFACE;
        if (!AddRef()) return E_FAIL;
        *output = static_cast<IUnknown*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        ULONG value = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!value || value == (std::numeric_limits<ULONG>::max)()) return value;
            if (refs_.compare_exchange_weak(value, value + 1, std::memory_order_acquire,
                                             std::memory_order_relaxed)) return value + 1;
        }
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG value = refs_.load(std::memory_order_relaxed);
        for (;;) {
            if (!value || value == (std::numeric_limits<ULONG>::max)()) return value;
            if (refs_.compare_exchange_weak(value, value - 1, std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) break;
        }
        if (value != 1) return value - 1;
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            const auto found = core_->entries.find(identity_);
            if (found != core_->entries.end() && found->second == this) core_->entries.erase(found);
        }
        delete this;
        return 0;
    }
    bool TryAddRef() noexcept { return AddRef() != 0; }
    t3render::buffers::PrivateBlobState state;
    bool ready = false; // accessed only with core mutex held
private:
    std::shared_ptr<PrivateMarkerCore> core_;
    void* const identity_;
    std::atomic<ULONG> refs_{1};
};

class PrivateMarkerStore final {
public:
    explicit PrivateMarkerStore(std::size_t maximum) : core_(std::make_shared<PrivateMarkerCore>(maximum)) {}

    // The caller keeps the native source alive and supplies its canonical
    // identity. Native calls are outside the registry mutex. A collision with
    // the reserved GUID is never overwritten. Overlaps fail closed.
    template<class Native>
    PrivateMarker* Acquire(void* identity, Native* native, bool create) noexcept {
        if (!identity || !native) return nullptr;
        PrivateMarker* candidate = create ? new (std::nothrow) PrivateMarker(core_, identity) : nullptr;
        PrivateMarker* result = nullptr;
        bool published = false;
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            const auto found = core_->entries.find(identity);
            if (found != core_->entries.end()) {
                if (found->second->ready && found->second->TryAddRef()) result = found->second;
            } else if (candidate && core_->entries.size() < core_->maximum) {
                try { published = core_->entries.emplace(identity, candidate).second; } catch (...) {}
                if (published) result = candidate;
            }
        }
        if (candidate && !published) candidate->Release();
        if (!published) return result;

        DWORD size = 0;
        const HRESULT existing = native->GetPrivateData(kPrivateMarkerGuid, nullptr, &size);
        const HRESULT stored = existing == D3DERR_NOTFOUND ?
            native->SetPrivateData(kPrivateMarkerGuid, static_cast<IUnknown*>(result), sizeof(IUnknown*), D3DSPD_IUNKNOWN) : E_FAIL;
        if (!FAILED(stored)) {
            std::lock_guard<std::mutex> lock(core_->mutex);
            result->ready = true;
            return result;
        }
        {
            std::lock_guard<std::mutex> lock(core_->mutex);
            const auto found = core_->entries.find(identity);
            if (found != core_->entries.end() && found->second == result) core_->entries.erase(found);
        }
        result->Release();
        return nullptr;
    }
private:
    std::shared_ptr<PrivateMarkerCore> core_;
};

class PrivateMarkerCache final {
public:
    ~PrivateMarkerCache() { if (marker_) marker_->Release(); }
    PrivateMarkerCache() = default;
    PrivateMarkerCache(const PrivateMarkerCache&) = delete;
    PrivateMarkerCache& operator=(const PrivateMarkerCache&) = delete;

    template<class Native, class Identity>
    t3render::buffers::PrivateBlobState* Acquire(PrivateMarkerStore& store, Native* native,
                                                  bool create, Identity identity) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (marker_) return &marker_->state;
            if (busy_) return nullptr;
            busy_ = true;
        }
        PrivateMarker* acquired = store.Acquire(identity(native), native, create);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            marker_ = acquired;
            busy_ = false;
        }
        return acquired ? &acquired->state : nullptr;
    }
private:
    std::mutex mutex_;
    bool busy_ = false;
    PrivateMarker* marker_ = nullptr;
};

} // namespace t3render::resource_fixture
