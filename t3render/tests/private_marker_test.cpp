#include "../src/private_marker.hpp"

#include <cassert>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

using namespace t3render::markers;

namespace {
struct Native final {
    IUnknown* marker = nullptr;
    bool collision = false, fail_store = false, retain_on_failure = false;
    unsigned stores = 0;
    std::function<void()> on_probe, on_store;
    ~Native() { Clear(); }
    HRESULT GetPrivateData(REFGUID key, void* data, DWORD* size) {
        assert(IsEqualIID(key, kPrivateMarkerGuid) && !data && size);
        if (on_probe) on_probe();
        return marker || collision ? S_OK : D3DERR_NOTFOUND;
    }
    HRESULT SetPrivateData(REFGUID key, const void* data, DWORD size, DWORD flags) {
        assert(IsEqualIID(key, kPrivateMarkerGuid) && data && size == sizeof(IUnknown*) && flags == D3DSPD_IUNKNOWN);
        ++stores;
        if (on_store) on_store();
        if (!fail_store || retain_on_failure) {
            assert(!marker);
            marker = const_cast<IUnknown*>(static_cast<const IUnknown*>(data));
            marker->AddRef();
        }
        return fail_store ? E_FAIL : S_OK;
    }
    void Clear() {
        if (marker) { auto* old = marker; marker = nullptr; old->Release(); }
    }
};
void* Identity(unsigned n) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(n)); }
}

int main() {
    const GUID data_key{42};
    PrivateMarkerStore ownership(2);
    auto owner = std::make_shared<int>(1);
    auto other = std::make_shared<int>(2);
    assert(ownership.BindBufferObserver(owner) && ownership.BindBufferObserver(owner));
    assert(!ownership.BindBufferObserver(other));
    owner.reset();
    assert(!ownership.BindBufferObserver(other));
    PrivateMarkerStore store(2);
    Native first;
    first.on_store = [&] { assert(!store.Acquire(Identity(1), &first, false)); };
    auto* marker = store.Acquire(Identity(1), &first, true);
    assert(marker && first.marker && first.stores == 1);
    assert(marker->state.Set(data_key, 0, [] { return S_OK; }) == S_OK);
    marker->Release(); // only the native resource owns the marker
    auto* reacquired = store.Acquire(Identity(1), &first, false);
    assert(reacquired && reacquired->state.Get(data_key, [] { return S_OK; }) == S_OK);
    reacquired->Release();
    first.Clear(); // final marker release unregisters the identity
    assert(!store.Acquire(Identity(1), &first, false));
    first.on_store = {};
    auto* fresh = store.Acquire(Identity(1), &first, true);
    assert(fresh && fresh->state.Get(data_key, [] { return S_OK; }) == E_NOTIMPL);
    fresh->Release();

    Native collision; collision.collision = true;
    assert(!store.Acquire(Identity(2), &collision, true) && collision.stores == 0);
    Native failed; failed.fail_store = true; failed.retain_on_failure = true;
    assert(!store.Acquire(Identity(2), &failed, true) && failed.marker && failed.stores == 1);
    assert(!store.Acquire(Identity(2), &failed, false));
    assert(!store.Acquire(Identity(2), &failed, true) && failed.stores == 1);
    failed.Clear();

    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, proceed = false;
    Native concurrent;
    concurrent.on_probe = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; condition.notify_one();
        condition.wait(lock, [&] { return proceed; });
    };
    PrivateMarker* from_thread = nullptr;
    std::thread worker([&] { from_thread = store.Acquire(Identity(2), &concurrent, true); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    assert(!store.Acquire(Identity(2), &concurrent, false));
    assert(!store.Acquire(Identity(2), &concurrent, true));
    Native over_capacity;
    assert(!store.Acquire(Identity(3), &over_capacity, true));
    {
        std::lock_guard<std::mutex> lock(mutex); proceed = true;
    }
    condition.notify_one(); worker.join();
    assert(from_thread && concurrent.stores == 1);
    from_thread->Release(); concurrent.Clear();
    auto* third = store.Acquire(Identity(3), &over_capacity, true);
    assert(third && over_capacity.stores == 1);
    third->Release(); over_capacity.Clear(); first.Clear();
}
