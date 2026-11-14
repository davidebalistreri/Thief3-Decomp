#pragma once

// Fixture-only private-data provenance. Never hold the mutex while calling
// the runtime or an identity mapper. Unobserved provenance fails closed.
#include <windows.h>

#ifdef max
#pragma push_macro("max")
#undef max
#define T3RENDER_PRIVATE_BLOB_RESTORE_MAX
#endif
#ifdef min
#pragma push_macro("min")
#undef min
#define T3RENDER_PRIVATE_BLOB_RESTORE_MIN
#endif
#include <algorithm>
#include <mutex>
#include <vector>
#ifdef T3RENDER_PRIVATE_BLOB_RESTORE_MIN
#pragma pop_macro("min")
#undef T3RENDER_PRIVATE_BLOB_RESTORE_MIN
#endif
#ifdef T3RENDER_PRIVATE_BLOB_RESTORE_MAX
#pragma pop_macro("max")
#undef T3RENDER_PRIVATE_BLOB_RESTORE_MAX
#endif

namespace t3render::buffers {

enum class PrivateDataKind {
    Blob, VertexBuffer, IndexBuffer, Surface, Texture, Cube, VolumeTexture, Volume, SwapChain
};

struct PrivateObjectPin final {
    IUnknown* native = nullptr; // borrowed while guard is held
    IUnknown* guard = nullptr;  // owned facade reference
    PrivateDataKind kind = PrivateDataKind::Blob;
    ~PrivateObjectPin() {
        if (guard) { const DWORD error = GetLastError(); guard->Release(); SetLastError(error); }
    }
    PrivateObjectPin() = default;
    PrivateObjectPin(const PrivateObjectPin&) = delete;
    PrivateObjectPin& operator=(const PrivateObjectPin&) = delete;
};

// Fixture-only bridge. An implementation recognizes issued live facade
// pointers and consumes the native GetPrivateData reference when adopting.
struct PrivateObjectBridge {
    virtual bool Pin(const void* candidate, PrivateObjectPin& pin) noexcept = 0;
    virtual IUnknown* Adopt(PrivateDataKind kind, IUnknown* native) noexcept = 0;
    virtual ~PrivateObjectBridge() = default;
};

class PrivateBlobState final {
public:
    template<class Call>
    HRESULT Set(REFGUID guid, DWORD flags, Call call) noexcept {
        if (flags != 0) return E_NOTIMPL;
        return SetTyped(guid, PrivateDataKind::Blob, call);
    }

    // The caller must verify and pin an issued facade object before invoking
    // this, and must never forward the facade pointer into native private data.
    template<class Call>
    HRESULT SetTyped(REFGUID guid, PrivateDataKind kind, Call call) noexcept {
        const DWORD before = GetLastError();
        PrivateDataKind previous{};
        if (!Begin(guid, true, previous)) { SetLastError(before); return E_NOTIMPL; }
        SetLastError(before);
        const HRESULT hr = call();
        const DWORD after = GetLastError();
        End(guid, FAILED(hr), &kind);
        SetLastError(after);
        return hr;
    }

    template<class Call>
    HRESULT Get(REFGUID guid, Call call) noexcept {
        return GetTyped(guid, [&](PrivateDataKind kind) {
            return kind == PrivateDataKind::Blob ? call() : E_NOTIMPL;
        });
    }

    template<class Call>
    HRESULT GetTyped(REFGUID guid, Call call) noexcept {
        const DWORD before = GetLastError();
        PrivateDataKind kind{};
        if (!Begin(guid, false, kind)) { SetLastError(before); return E_NOTIMPL; }
        SetLastError(before);
        const HRESULT hr = call(kind);
        const DWORD after = GetLastError();
        End(guid, false, nullptr);
        SetLastError(after);
        return hr;
    }

    template<class Call>
    HRESULT Free(REFGUID guid, Call call) noexcept {
        const DWORD before = GetLastError();
        PrivateDataKind kind{};
        if (!Begin(guid, false, kind)) { SetLastError(before); return E_NOTIMPL; }
        SetLastError(before);
        const HRESULT hr = call();
        const DWORD after = GetLastError();
        // A successful free establishes that the GUID is absent; retaining the
        // entry permits a subsequent native NOTFOUND result. A failed call may
        // have changed the value, so its provenance becomes unknown.
        End(guid, FAILED(hr), nullptr);
        SetLastError(after);
        return hr;
    }

private:
    static constexpr std::size_t kMaximumKeys = 64;
    struct Entry { GUID guid; PrivateDataKind kind; };
    std::mutex mutex_;
    std::vector<Entry> keys_;
    bool busy_ = false;

    bool Begin(REFGUID guid, bool create, PrivateDataKind& kind) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_) return false;
        auto found = std::find_if(keys_.begin(), keys_.end(),
                                  [&](const Entry& item) { return IsEqualIID(item.guid, guid); });
        if (found == keys_.end()) {
            if (!create || keys_.size() >= kMaximumKeys) return false;
            try { keys_.push_back({guid, PrivateDataKind::Blob}); } catch (...) { return false; }
            found = keys_.end() - 1;
        }
        kind = found->kind;
        busy_ = true;
        return true;
    }

    void End(REFGUID guid, bool forget, const PrivateDataKind* replacement) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = std::find_if(keys_.begin(), keys_.end(),
                                        [&](const Entry& item) { return IsEqualIID(item.guid, guid); });
        if (found != keys_.end() && forget) keys_.erase(found);
        else if (found != keys_.end() && replacement) found->kind = *replacement;
        busy_ = false;
    }
};

}  // namespace t3render::buffers
