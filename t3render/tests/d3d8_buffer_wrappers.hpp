#pragma once

// Fixture-only COM wrappers. The underlying resources and their vtables remain
// unchanged. A real device must receive Real(), never the wrapper interface.
// This is not a general device proxy or a complete COM alias mapper.
#include <windows.h>
#include <d3d8.h>

#include "../ir/buffer_registry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>

namespace t3render::harness {

using SnapshotReader = bool (*)(void*, const void*, void*, std::size_t);

template<class Buffer> struct BufferTraits;
template<> struct BufferTraits<IDirect3DVertexBuffer8> {
    using Desc = D3DVERTEXBUFFER_DESC;
    static const IID& Iid() { return IID_IDirect3DVertexBuffer8; }
};
template<> struct BufferTraits<IDirect3DIndexBuffer8> {
    using Desc = D3DINDEXBUFFER_DESC;
    static const IID& Iid() { return IID_IDirect3DIndexBuffer8; }
};

template<class Buffer>
class BufferWrapper final : public Buffer {
public:
    // Adopts one reference only on success. The caller continues owning it on
    // failure. The reader must copy all bytes or return false, must be fault
    // safe, and must never call the wrapper or D3D runtime recursively.
    static BufferWrapper* Adopt(Buffer* real, UINT size, SnapshotReader reader, void* context) noexcept {
        if (!real || !reader) return nullptr;
        try { return new (std::nothrow) BufferWrapper(real, size, reader, context); }
        catch (...) { return nullptr; }
    }

    // Borrowed pointer. Explicitly unwrap every resource passed to the device.
    Buffer* Real() const noexcept { return real_; }

    ir::RegistryResult ReadObserved(std::size_t offset, void* destination, std::size_t length) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return shadows_.Read(token_, offset, destination, length);
    }

    void MarkUnknown() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        Abandon();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDirect3DResource8) ||
            IsEqualIID(iid, BufferTraits<Buffer>::Iid())) {
            *output = static_cast<Buffer*>(this);
            AddRef();
            return S_OK;
        }
        // An unknown interface can escape around this observer. Forward the
        // runtime's result but never claim subsequent bytes remain known.
        const DWORD previous = GetLastError();
        MarkUnknown();
        SetLastError(previous);
        return real_->QueryInterface(iid, output);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (!left) {
            const DWORD previous = GetLastError();
            Buffer* real = real_;
            delete this;
            SetLastError(previous);
            real->Release();
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** output) override {
        // The returned raw device is not wrapped in this fixture-only harness.
        const HRESULT hr = real_->GetDevice(output);
        const DWORD error = GetLastError();
        if (SUCCEEDED(hr)) MarkUnknown();
        SetLastError(error);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void* data, DWORD size, DWORD flags) override {
        return real_->SetPrivateData(guid, data, size, flags);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void* data, DWORD* size) override {
        return real_->GetPrivateData(guid, data, size);
    }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override { return real_->FreePrivateData(guid); }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override { return real_->SetPriority(priority); }
    DWORD STDMETHODCALLTYPE GetPriority() override { return real_->GetPriority(); }
    void STDMETHODCALLTYPE PreLoad() override { real_->PreLoad(); }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return real_->GetType(); }
    HRESULT STDMETHODCALLTYPE GetDesc(typename BufferTraits<Buffer>::Desc* desc) override { return real_->GetDesc(desc); }

    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT length, BYTE** output, DWORD flags) override {
        const DWORD previous = GetLastError();
        const auto operation = BeginLock(offset, length, flags);
        SetLastError(previous);
        const HRESULT hr = real_->Lock(offset, length, output, flags);
        const DWORD error = GetLastError();
        BYTE* data = nullptr;
        bool copied = false;
        if (operation && SUCCEEDED(hr) && output) {
            try { copied = reader_(reader_context_, output, &data, sizeof(data)) && data; }
            catch (...) { copied = false; }
        }
        FinishLock(operation, hr, copied, data);
        SetLastError(error);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Unlock() override {
        const DWORD previous = GetLastError();
        const auto operation = BeginUnlock();
        SetLastError(previous);
        const HRESULT hr = real_->Unlock();
        const DWORD error = GetLastError();
        FinishUnlock(operation, hr);
        SetLastError(error);
        return hr;
    }

private:
    static constexpr std::size_t kBudget = 512u * 1024u;
    static constexpr std::size_t kSnapshotLimit = 256u * 1024u;
    struct ReaderContext { SnapshotReader reader; void* context; const void* source; };
    static bool StageRead(void* value, void* destination, std::size_t length) {
        auto& state = *static_cast<ReaderContext*>(value);
        return state.reader(state.context, state.source, destination, length);
    }

    BufferWrapper(Buffer* real, UINT size, SnapshotReader reader, void* context) noexcept
        : real_(real), size_(size), reader_(reader), reader_context_(context), shadows_(kBudget, 1) {
        ir::BufferAvailability availability{};
        shadows_.Register(reinterpret_cast<std::uintptr_t>(real), size, token_, availability);
    }
    ~BufferWrapper() = default;
    void Abandon() noexcept {
        shadows_.Abandon(token_);
        lock_pending_ = false; locked_ = false; unlock_pending_ = false;
        data_ = nullptr; length_ = 0; operation_ = 0;
    }
    std::uint64_t BeginLock(UINT offset, UINT length, DWORD flags) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        constexpr DWORD allowed = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD;
        if ((flags & ~allowed) || ((flags & D3DLOCK_READONLY) && (flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) ||
            ((flags & D3DLOCK_DISCARD) && (flags & D3DLOCK_NOOVERWRITE)) ||
            lock_pending_ || locked_ || unlock_pending_ || !next_operation_) {
            Abandon(); return 0;
        }
        const std::size_t normalized = length ? length : (offset == 0 ? size_ : 0);
        const auto kind = flags & D3DLOCK_READONLY ? ir::BufferLock::ReadOnly :
                          flags & D3DLOCK_DISCARD ? ir::BufferLock::Discard : ir::BufferLock::Write;
        if (shadows_.BeginLock(token_, offset, normalized, kind) != ir::RegistryResult::Ok) {
            Abandon(); return 0;
        }
        operation_ = next_operation_++;
        lock_pending_ = true;
        read_only_ = kind == ir::BufferLock::ReadOnly;
        length_ = normalized;
        return operation_;
    }
    void FinishLock(std::uint64_t operation, HRESULT hr, bool copied, const void* data) noexcept {
        if (!operation) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (operation_ != operation || !lock_pending_) return;
        if (FAILED(hr) || !copied) { Abandon(); return; }
        lock_pending_ = false;
        locked_ = true;
        data_ = data;
    }
    std::uint64_t BeginUnlock() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lock_pending_ || !locked_ || unlock_pending_ || !next_operation_) { Abandon(); return 0; }
        operation_ = next_operation_++;
        unlock_pending_ = true;
        if (!read_only_ && length_ <= kSnapshotLimit) {
            ReaderContext context{reader_, reader_context_, data_};
            shadows_.StageUnlockFromReader(token_, StageRead, &context);
        }
        return operation_;
    }
    void FinishUnlock(std::uint64_t operation, HRESULT hr) noexcept {
        if (!operation) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (operation_ != operation || !unlock_pending_) return;
        shadows_.EndUnlock(token_, SUCCEEDED(hr));
        unlock_pending_ = false; locked_ = false; data_ = nullptr; length_ = 0; operation_ = 0;
    }

    Buffer* const real_;
    const UINT size_;
    SnapshotReader const reader_;
    void* const reader_context_;
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    ir::BufferRegistry shadows_;
    ir::BufferToken token_{};
    std::uint64_t next_operation_ = 1, operation_ = 0;
    bool lock_pending_ = false, locked_ = false, unlock_pending_ = false, read_only_ = false;
    const void* data_ = nullptr;
    std::size_t length_ = 0;
};

using VertexBufferWrapper = BufferWrapper<IDirect3DVertexBuffer8>;
using IndexBufferWrapper = BufferWrapper<IDirect3DIndexBuffer8>;

inline HRESULT ProcessVerticesWrapped(IDirect3DDevice8* device, UINT source, UINT destination, UINT count,
                                      VertexBufferWrapper* target, DWORD flags) {
    const DWORD previous = GetLastError();
    if (target) target->MarkUnknown();  // GPU writes are not observed by Lock.
    SetLastError(previous);
    return device->ProcessVertices(source, destination, count, target ? target->Real() : nullptr, flags);
}

// Fixture-only getter helpers. Identity is restored only when the returned raw
// pointer exactly matches a still-live expected wrapper. Otherwise the real
// reference is returned unchanged and the observation is explicitly unknown.
inline HRESULT GetWrappedStream(IDirect3DDevice8* device, UINT stream, VertexBufferWrapper* expected,
                                IDirect3DVertexBuffer8** output, UINT* stride, bool& matched) {
    matched = false;
    const HRESULT hr = device->GetStreamSource(stream, output, stride);
    const DWORD error = GetLastError();
    if (SUCCEEDED(hr) && output && *output && expected && *output == expected->Real()) {
        expected->AddRef();
        (*output)->Release();
        *output = expected;
        matched = true;
    } else if (expected) expected->MarkUnknown();
    SetLastError(error);
    return hr;
}

inline HRESULT GetWrappedIndices(IDirect3DDevice8* device, IndexBufferWrapper* expected,
                                 IDirect3DIndexBuffer8** output, UINT* base, bool& matched) {
    matched = false;
    const HRESULT hr = device->GetIndices(output, base);
    const DWORD error = GetLastError();
    if (SUCCEEDED(hr) && output && *output && expected && *output == expected->Real()) {
        expected->AddRef();
        (*output)->Release();
        *output = expected;
        matched = true;
    } else if (expected) expected->MarkUnknown();
    SetLastError(error);
    return hr;
}
}  // namespace t3render::harness
