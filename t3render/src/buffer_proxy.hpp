#pragma once

// Experimental, opt-in D3D8 buffer observation. The native resource and its
// vtable are never modified. This is not a complete D3D8 device wrapper.
#include <windows.h>
#include <d3d8.h>

#include "../ir/buffer_registry.hpp"
#include "buffer_diagnostics.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace t3render::markers { class PrivateMarkerStore; }

namespace t3render::buffers {

struct PrivateObjectBridge;
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
        bool wrapped() const noexcept { return guard_ != nullptr; }
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
        bool wrapped() const noexcept { return guard_ != nullptr; }
    private:
        friend class BufferProxy;
        IndexPin(IDirect3DIndexBuffer8* native, IUnknown* guard) noexcept : native_(native), guard_(guard) {}
        IDirect3DIndexBuffer8* native_ = nullptr;
        IUnknown* guard_ = nullptr;
    };

    // Prepared outside the capture-writer mutex, using a caller-owned native
    // getter reference and an active draw window. Read never calls COM. The
    // caller must keep that native +1 and device alive until the draw ends.
    class PreparedVertex final {
    public:
        bool valid() const noexcept { return state_ && token_.identity && token_.generation && draw_; }
        ir::RegistryResult Read(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                std::size_t offset, void* destination, std::size_t length) const noexcept;
    private:
        friend class BufferProxy;
        std::shared_ptr<State> state_;
        IDirect3DDevice8* owner_ = nullptr;
        IDirect3DVertexBuffer8* native_ = nullptr;
        ir::BufferToken token_{};
        std::uint64_t draw_ = 0;
    };
    class PreparedIndex final {
    public:
        bool valid() const noexcept { return state_ && token_.identity && token_.generation && draw_; }
        ir::RegistryResult Read(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                std::size_t offset, void* destination, std::size_t length) const noexcept;
    private:
        friend class BufferProxy;
        std::shared_ptr<State> state_;
        IDirect3DDevice8* owner_ = nullptr;
        IDirect3DIndexBuffer8* native_ = nullptr;
        ir::BufferToken token_{};
        std::uint64_t draw_ = 0;
    };

    // All wrappers share a single byte and resource budget. Reader must be
    // fault-safe, return true only on a complete copy, and never re-enter this
    // object or a D3D method. Its context must outlive all emitted wrappers.
    static std::shared_ptr<BufferProxy> Create(SnapshotReader reader, void* context,
                                                std::size_t budget = 32u * 1024u * 1024u,
                                                std::size_t max_resources = 4096) noexcept;

    // Opt-in experiment, before any device or resource is registered. A
    // recognized repeated Unlock discards that buffer's shadow before the
    // native call and holds a device mutation window through its completion.
    bool EnableRepeatedUnlockExperiment() noexcept;
    struct RepeatedUnlockStats { bool enabled = false; std::uint64_t completed = 0; };
    RepeatedUnlockStats GetRepeatedUnlockStats(IDirect3DDevice8* owner) noexcept;

    // Adopts the native Create/Get +1 reference. On success returns one wrapper
    // reference and consumes native; on fallback returns native unchanged.
    // A getter (freshly_created=false) cannot create new tracked data.
    IDirect3DVertexBuffer8* AdoptVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                         UINT size, bool freshly_created) noexcept;
    IDirect3DIndexBuffer8* AdoptIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                       UINT size, bool freshly_created) noexcept;

    // Isolated facade experiment only. The caller must own both device
    // references, facade must retain owner, and the caller must intercept
    // every path that can accept an emitted wrapper.
    // A live wrapper retains facade, and GetDevice returns it only after
    // verifying the native parent's canonical identity. On final Release a
    // dormant wrapper retains its native resource and can be reacquired while
    // the facade lives; it does not retain the facade in that state. Unknown
    // interfaces and unobserved private data fail closed; observed opaque
    // blobs and, if a fixture bridge is explicitly installed, recognized
    // same-device COM private data have limited support. On failure these methods consume
    // native and return null instead of exposing a raw buffer. They do not
    // close aliases returned by textures, surfaces, swapchains or state blocks;
    // this is not safe to enable in TDS.
    IDirect3DVertexBuffer8* AdoptVertexForFacade(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                                                  IDirect3DVertexBuffer8* native, UINT size, bool freshly_created) noexcept;
    IDirect3DIndexBuffer8* AdoptIndexForFacade(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                                                IDirect3DIndexBuffer8* native, UINT size, bool freshly_created) noexcept;

    // The facade must call this before its own final destruction, with no
    // live buffer references. It prevents reacquisition, retires dormant
    // wrappers and releases their native resources outside the registry lock.
    // Returns false and retains any still-live wrapper on contract violation.
    bool CloseFacadeDevice(IDirect3DDevice8* owner, IDirect3DDevice8* facade) noexcept;

    // Fixture-only reset preparation: permanently invalidate observed bytes,
    // detach dormant facade buffers and release their native references outside
    // the registry lock. Later getter/create wrappers remain untracked.
    void RetireFacadeDormantForReset(IDirect3DDevice8* owner, IDirect3DDevice8* facade) noexcept;

    // Fixture-only reset epoch. The caller must have a closed resource/device
    // boundary. A nonzero ticket excludes overlapping buffer operations; only
    // a stable successful reset permits tracking newly created buffers again.
    // Surviving or re-adopted buffers have unknown bytes until replaced.
    std::uint64_t BeginFacadeReset(IDirect3DDevice8* owner, IDirect3DDevice8* facade) noexcept;
    bool EndFacadeReset(IDirect3DDevice8* owner, std::uint64_t ticket, bool succeeded) noexcept;

    // Input is identified by pointer lookup only, never dereferenced. A pin
    // retains an issued wrapper while its native resource is passed to D3D.
    // Raw non-null inputs forward unchanged but invalidate owner evidence.
    VertexPin UnwrapVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* input) noexcept;
    IndexPin UnwrapIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* input) noexcept;

    // Fixture-only private-data bridge: recognize an issued live VB/IB
    // wrapper by address without dereferencing unknown inputs or poisoning
    // observation on a miss. The returned guard pins the wrapper until the
    // native SetPrivateData has acquired its own reference.
    IUnknown* PinPrivateBuffer(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                               const void* input, IUnknown** native, bool* vertex) noexcept;

    // Fixture-only: the device owns the bridge; the buffer registry retains
    // it weakly, avoiding a bridge/registry ownership cycle. With eager
    // retirement, a reacquired buffer's previous shadow is unknown.
    bool InstallFacadePrivateBridge(IDirect3DDevice8* owner, IDirect3DDevice8* facade,
                                    const std::shared_ptr<PrivateObjectBridge>& bridge,
                                    bool retain_buffers = true,
                                    const std::shared_ptr<t3render::markers::PrivateMarkerStore>& markers = {}) noexcept;

    // Conservative invalidation. Once a device escapes, its data remains
    // unavailable; wrappers already issued must still be unwrapped normally.
    // Once capture stops, free all shadows while retaining the weak wrapper
    // mappings needed to forward already-issued COM objects until Release.
    void StopObservation() noexcept;
    void InvalidateDevice(IDirect3DDevice8* owner, InvalidationCause cause = InvalidationCause::Explicit) noexcept;
    void InvalidateVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* input) noexcept;

    // A nonzero token marks a draw window from before snapshot until after the
    // native draw returns. EndDraw returns false if overlap/escape invalidated
    // the window. Direct writes to a mapped pointer remain unobservable.
    std::uint64_t BeginDraw(IDirect3DDevice8* owner) noexcept;
    // Diagnostic only: explains why a draw ticket was unavailable. It never
    // restores a poisoned device or attests the safety of subsequent calls.
    DrawFailure DescribeDrawFailure(IDirect3DDevice8* owner) noexcept;
    bool EndDraw(IDirect3DDevice8* owner, std::uint64_t token) noexcept;

    // Cover the entire native SetStreamSource/SetIndices/ProcessVertices call.
    // Facade Create/Get calls must also cover their native call and adoption,
    // otherwise a reset could complete while a returned buffer is in flight.
    // Any overlap with a draw, reset or another mutation poisons the device.
    std::uint64_t BeginBindingMutation(IDirect3DDevice8* owner) noexcept;
    void EndBindingMutation(IDirect3DDevice8* owner, std::uint64_t token) noexcept;

    // Exact native-pointer lookup only; unknown pointers are not dereferenced.
    // Unknown or unstable data never become synthetic zeroes.
    ir::RegistryResult ReadVertex(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                   std::size_t offset, void* destination, std::size_t length) noexcept;
    ir::RegistryResult ReadIndex(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                  std::size_t offset, void* destination, std::size_t length) noexcept;
    // Isolated persistent experiment only: the caller guarantees a valid +1
    // native COM reference obtained from a getter for the duration of this
    // call. A missing live wrapper may be resolved by canonical IUnknown.
    ir::RegistryResult ReadVertexFromGetter(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                            std::size_t offset, void* destination, std::size_t length) noexcept;
    ir::RegistryResult ReadIndexFromGetter(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                           std::size_t offset, void* destination, std::size_t length) noexcept;
    PreparedVertex PrepareVertexGetter(IDirect3DDevice8* owner, IDirect3DVertexBuffer8* native,
                                       std::uint64_t draw) noexcept;
    PreparedIndex PrepareIndexGetter(IDirect3DDevice8* owner, IDirect3DIndexBuffer8* native,
                                     std::uint64_t draw) noexcept;

private:
    explicit BufferProxy(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};

}  // namespace t3render::buffers
