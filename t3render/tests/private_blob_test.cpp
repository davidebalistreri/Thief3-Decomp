#include "../src/private_blob.hpp"

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>

using t3render::buffers::PrivateBlobState;
using t3render::buffers::PrivateDataKind;

int main() {
    PrivateBlobState state;
    const GUID first{1}, second{2};
    unsigned calls = 0;
    auto success = [&] { ++calls; SetLastError(22); return S_OK; };
    auto failure = [&] { ++calls; SetLastError(23); return E_FAIL; };

    SetLastError(11);
    assert(state.Get(first, success) == E_NOTIMPL && state.Free(first, success) == E_NOTIMPL);
    assert(state.Set(first, 1, success) == E_NOTIMPL && calls == 0 && GetLastError() == 11);
    assert(state.Set(first, 0, success) == S_OK && calls == 1 && GetLastError() == 22);
    assert(state.Set(first, 1, success) == E_NOTIMPL); // rejected COM write leaves blob provenance intact
    assert(state.Get(first, success) == S_OK && state.Get(second, success) == E_NOTIMPL);
    assert(state.Free(first, success) == S_OK && state.Get(first, success) == S_OK);
    assert(state.Set(first, 0, failure) == E_FAIL && GetLastError() == 23);
    assert(state.Get(first, success) == E_NOTIMPL);
    assert(state.Set(first, 0, success) == S_OK && state.Free(first, failure) == E_FAIL);
    assert(state.Get(first, success) == E_NOTIMPL);

    assert(state.Set(first, 0, [&] {
        assert(state.Get(first, success) == E_NOTIMPL);
        assert(state.Set(second, 0, success) == E_NOTIMPL);
        return S_OK;
    }) == S_OK);
    assert(state.Get(first, success) == S_OK && state.Get(second, success) == E_NOTIMPL);

    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, release = false;
    std::thread worker([&] {
        assert(state.Set(second, 0, [&] {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true; condition.notify_one();
            condition.wait(lock, [&] { return release; });
            return S_OK;
        }) == S_OK);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    assert(state.Get(first, success) == E_NOTIMPL && state.Free(second, success) == E_NOTIMPL);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_one(); worker.join();
    assert(state.Get(second, success) == S_OK);

    PrivateBlobState typed;
    auto kind_is = [&](PrivateDataKind expected) {
        return typed.GetTyped(first, [&](PrivateDataKind actual) {
            assert(actual == expected); return S_OK;
        });
    };
    assert(typed.SetTyped(first, PrivateDataKind::VertexBuffer, success) == S_OK);
    assert(kind_is(PrivateDataKind::VertexBuffer) == S_OK);
    assert(typed.SetTyped(first, PrivateDataKind::IndexBuffer, success) == S_OK);
    assert(kind_is(PrivateDataKind::IndexBuffer) == S_OK);
    assert(typed.SetTyped(first, PrivateDataKind::ExternalUnknown, success) == S_OK);
    assert(kind_is(PrivateDataKind::ExternalUnknown) == S_OK);
    assert(typed.Get(first, success) == E_NOTIMPL); // never expose a native object as a blob
    assert(typed.Set(first, 0, success) == S_OK && kind_is(PrivateDataKind::Blob) == S_OK);
    assert(typed.SetTyped(first, PrivateDataKind::VertexBuffer, failure) == E_FAIL);
    assert(typed.GetTyped(first, [&](PrivateDataKind) { return S_OK; }) == E_NOTIMPL);
    assert(typed.SetTyped(first, PrivateDataKind::VertexBuffer, success) == S_OK);
    assert(typed.Free(first, success) == S_OK && kind_is(PrivateDataKind::VertexBuffer) == S_OK);

    PrivateBlobState bounded;
    for (int n = 0; n != 64; ++n) assert(bounded.Set(GUID{n}, 0, success) == S_OK);
    assert(bounded.Set(GUID{64}, 0, success) == E_NOTIMPL);
    assert(bounded.Set(GUID{0}, 0, success) == S_OK);
    assert(bounded.Set(GUID{0}, 0, failure) == E_FAIL);
    assert(bounded.Set(GUID{64}, 0, success) == S_OK);
    assert(bounded.Get(GUID{0}, success) == E_NOTIMPL);
}
