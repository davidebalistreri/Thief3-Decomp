#pragma once

// Experimental, opt-in D3D8 buffer observation. The native resource and its
// vtable are never modified. This is not a complete D3D8 device wrapper.
#include <windows.h>
#include <d3d8.h>

#include "../ir/buffer_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace t3render::buffers {

using SnapshotReader = bool (*)(void* context, const void* source, void* destination, std::size_t length);

class BufferProxy final {
public:
    struct State;

    class VertexPin final {
    public:
        VertexPin() noexcept = default;
        ~VertexPin();
        VertexPin(VertexPin&& other) noexcept;
        VertexPin& operator=(VertexPin&& other) noexcept;
        VertexPin(const VertexPin&) = delete;
        VertexPin& operator=(const VertexPin&) = delete;
        IDirect3DVertexBuffer8* native() const noexcept { return native_; }
    private:
        friend class BufferProxy;
        VertexPin(IDirect3DVertexBuffer8* native, IUnknown* guard) noexcept : native_(native), guard_(guard) {}
        IDirect3DVertexBuffer8* native_ = nullptr;
        IUnknown* guard_ = nullptr;
    };

    class IndexPin final {
    public:
        IndexPin() noexcept = default;
        ~IndexPin();
        IndexPin(IndexPin&& other) noexcept;
        IndexPin& operator=(IndexPin&& other) noexcept;
        IndexPin(const IndexPin&) = delete;
        IndexPin& operator=(const IndexPin&) = delete;
        IDirect3DIndexBuffer8* native() const noexcept { return native_; }
    private:
        friend class BufferProxy;
        IndexPin(IDirect3DIndexBuffer8* native, IUnknown* guard) noexcept : native_(native), guard_(guard) {}
        IDirect3DIndexBuffer8* native_ = nullptr;
        IUnknown* guard_ = nullptr;
    };

    // All wrappers share a single byte and resource budget. Reader must be
    // fault-safe, return true only on a complete copy, and never re-enter this
    // object or a D3D method. Its context must outlive all emitted wrappers.
    static std::shared_ptr<BufferProxy> Create(SnapshotReader reader, void* context,
                                                std::size_t budget = 32u * 1024u * 1024u,
                                                std::size_t max_resources = 4096) noexcept;

    // Adopts the native Create/Get +1 reference. On success returns one wrapper
    // reference and consumes native; on fallback returns native unchanged.
    // A getter (freshly_created=false) cannot create new tracked data.
    IDirect3DVertexBuffer8* AdoptVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                         UINT size, bool freshly_created) noexcept;
    IDirect3DIndexBuffer8* AdoptIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                       UINT size, bool freshly_created) noexcept;

    // Input is identified by pointer lookup only, never dereferenced. A pin
    // retains an issued wrapper while its native resource is passed to D3D.
    // Raw non-null inputs forward unchanged but invalidate owner evidence.
    VertexPin UnwrapVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* input) noexcept;
    IndexPin UnwrapIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* input) noexcept;

    // Conservative invalidation. Once a device escapes, its data remains
    // unavailable; wrappers already issued must still be unwrapped normally.
    // Once capture stops, free all shadows while retaining the weak wrapper
    // mappings needed to forward already-issued COM objects until Release.
    void StopObservation() noexcept;
    void InvalidateDevice(IDirect3DDevice8* owner) noexcept;
    void InvalidateVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* input) noexcept;

    // A nonzero token marks a draw window from before snapshot until after the
    // native draw returns. EndDraw returns false if overlap/escape invalidated
    // the window. Direct writes to a mapped pointer remain unobservable.
    std::uint64_t BeginDraw(IDirect3DDevice8* owner) noexcept;
    bool EndDraw(IDirect3DDevice8* owner, std::uint64_t token) noexcept;

    // Cover the entire native SetStreamSource/SetIndices/ProcessVertices call.
    // Any overlap with a draw or another binding mutation poisons the device.
    std::uint64_t BeginBindingMutation(IDirect3DDevice8* owner) noexcept;
    void EndBindingMutation(IDirect3DDevice8* owner, std::uint64_t token) noexcept;

    // Exact native-pointer lookup only. Aliases, dead wrappers, invalid owners,
    // unknown ranges and incomplete data never become synthetic zeroes.
    ir::RegistryResult ReadVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                   std::size_t offset, void* destination, std::size_t length) noexcept;
    ir::RegistryResult ReadIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                  std::size_t offset, void* destination, std::size_t length) noexcept;

private:
    explicit BufferProxy(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

}  // namespace t3render::buffers
