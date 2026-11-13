#pragma once

// Fixture-only private-data subset. A native Get is allowed only for GUIDs
// successfully written as opaque bytes through this wrapper. IUnknown and
// unknown provenance remain unsupported: returning a native COM interface
// could escape the facade. Never hold the mutex while calling the runtime.
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

class PrivateBlobState final {
public:
    template<class Call>
    HRESULT Set(REFGUID guid, DWORD flags, Call call) noexcept {
        const DWORD before = GetLastError();
        if (flags != 0 || !Begin(guid, true)) { SetLastError(before); return E_NOTIMPL; }
        SetLastError(before);
        const HRESULT hr = call();
        const DWORD after = GetLastError();
        End(guid, FAILED(hr));
        SetLastError(after);
        return hr;
    }

    template<class Call>
    HRESULT Get(REFGUID guid, Call call) noexcept {
        const DWORD before = GetLastError();
        if (!Begin(guid, false)) { SetLastError(before); return E_NOTIMPL; }
        SetLastError(before);
        const HRESULT hr = call();
        const DWORD after = GetLastError();
        End(guid, false);
        SetLastError(after);
        return hr;
    }

    template<class Call>
    HRESULT Free(REFGUID guid, Call call) noexcept {
        const DWORD before = GetLastError();
        if (!Begin(guid, false)) { SetLastError(before); return E_NOTIMPL; }
        SetLastError(before);
        const HRESULT hr = call();
        const DWORD after = GetLastError();
        // A successful free establishes that the GUID is absent; retaining the
        // entry permits a subsequent native NOTFOUND result. A failed call may
        // have changed the value, so its provenance becomes unknown.
        End(guid, FAILED(hr));
        SetLastError(after);
        return hr;
    }

private:
    static constexpr std::size_t kMaximumKeys = 64;
    std::mutex mutex_;
    std::vector<GUID> keys_;
    bool busy_ = false;

    bool Begin(REFGUID guid, bool create) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_) return false;
        const auto found = std::find_if(keys_.begin(), keys_.end(),
                                        [&](const GUID& item) { return IsEqualIID(item, guid); });
        if (found == keys_.end()) {
            if (!create || keys_.size() >= kMaximumKeys) return false;
            try { keys_.push_back(guid); } catch (...) { return false; }
        }
        busy_ = true;
        return true;
    }

    void End(REFGUID guid, bool forget) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (forget) {
            const auto found = std::find_if(keys_.begin(), keys_.end(),
                                            [&](const GUID& item) { return IsEqualIID(item, guid); });
            if (found != keys_.end()) keys_.erase(found);
        }
        busy_ = false;
    }
};

}  // namespace t3render::buffers
