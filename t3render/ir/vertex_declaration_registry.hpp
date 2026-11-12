#pragma once

#include "vertex_declaration.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace t3render::ir {

// Portable experimental registry used by the optional proxy observer. A device token
// is unique to this registry lifetime and must represent one observed COM
// device lifetime; it is not a native pointer or persistent asset identity.
// Entries are candidates from application memory, not proof of driver input,
// programmable-shader identity, or a distinction between FVF and handle.
// Construction preallocates metadata outside the mutex: at most 64 devices,
// 4096 handles per device and 65536 total slots. If that fails, OpenDevice
// returns 0. Publish only after construction and destroy after all callers end.
enum class ShaderOperation { None, Create, Delete, Set, Reset, Draw };
struct ShaderTicket {
    std::uint64_t device = 0;
    std::uint64_t sequence = 0;
    ShaderOperation kind = ShaderOperation::None;
    explicit operator bool() const noexcept { return device && sequence && kind != ShaderOperation::None; }
};
enum class ShaderObservationStatus { Candidate, Unknown, Unavailable, Unsafe };
// The last conservative invalidation of a recorded generation; this is
// diagnostic metadata, never proof of what the driver consumed.
enum class ShaderUnavailableReason { None, ScanIncomplete, InvalidSnapshot, HandleCollision,
                                     ResetAttempt, DeleteFailed, ObservationError, Unknown };
struct ShaderObservation {
    ShaderObservationStatus status = ShaderObservationStatus::Unknown;
    std::uint64_t generation = 0;
    ShaderUnavailableReason reason = ShaderUnavailableReason::None;
    bool has_scan_status = false;
    DeclarationStatus scan_status = DeclarationStatus::Unreadable;
    std::vector<std::uint32_t> words;
    // Quarantined pre-reset application data. This is a distinct hypothesis,
    // never a candidate or proof of the post-reset driver input.
    std::uint64_t reset_epoch = 0, rebind_epoch = 0;
    std::uint32_t reset_hr = 0;
    std::vector<std::uint32_t> pre_reset_words;
};
struct ShaderRegistryLimits {
    std::size_t devices = 8;
    std::size_t handles_per_device = 1024;
    std::size_t words_per_declaration = 1024;
    std::size_t words_per_device = 65536;
    bool preserve_reset_history = false;
};

class VertexDeclarationRegistry final {
public:
    explicit VertexDeclarationRegistry(ShaderRegistryLimits limits = {}) noexcept;
    VertexDeclarationRegistry(const VertexDeclarationRegistry&) = delete;
    VertexDeclarationRegistry& operator=(const VertexDeclarationRegistry&) = delete;

    std::uint64_t OpenDevice() noexcept; // 0 on failure; never reuses an ID
    void CloseDevice(std::uint64_t device) noexcept;
    // Unsafe escape or unobserved mutation permanently invalidates this device.
    void Invalidate(std::uint64_t device) noexcept;
    // Marks unsafe without draining/deallocating retired payloads. The caller
    // must subsequently call Invalidate or CloseDevice outside its own locks.
    void MarkUnsafe(std::uint64_t device) noexcept;

    // All begin/end windows bracket the corresponding native operation, but
    // never hold this registry's lock across it. Any overlap poisons the
    // device. Delete invalidates its handle before forwarding regardless of
    // HRESULT; Reset marks entries unavailable before forwarding, also on failure.
    ShaderTicket BeginCreate(std::uint64_t device) noexcept;
    ShaderTicket BeginDelete(std::uint64_t device, std::uint32_t handle) noexcept;
    ShaderTicket BeginSet(std::uint64_t device) noexcept;
    ShaderTicket BeginReset(std::uint64_t device) noexcept;
    ShaderTicket BeginDraw(std::uint64_t device) noexcept;

    // On a successful native Create, `handle_known` must attest a complete,
    // fault-safe DWORD read of the output. Unknown output poisons the device.
    // Snapshot is sampled before the native call; invalid/unreadable snapshots
    // register an unavailable handle, never a fabricated declaration.
    bool EndCreate(ShaderTicket ticket, bool native_success, bool handle_known,
                   std::uint32_t handle, DeclarationSnapshot snapshot) noexcept;
    // Only a successful native Delete removes the unavailable handle; a failed
    // Delete keeps the tombstone so a later Create cannot silently reuse it.
    bool EndDelete(ShaderTicket ticket, bool native_success) noexcept;
    // HRESULT is preserved as raw 32-bit bits. Failed Reset or Set never
    // authorizes pre-reset history; native calls are never made here.
    bool EndReset(ShaderTicket ticket, std::uint32_t hr) noexcept;
    bool EndSet(ShaderTicket ticket, std::uint32_t hr, std::uint32_t value) noexcept;
    bool End(ShaderTicket ticket) noexcept;

    // Only during a live Draw ticket. The caller must also require End(draw)
    // to succeed after the native draw, otherwise discard this observation.
    // A numeric match is not proof of shader vs FVF or exact driver input. Its
    // returned vector is caller-owned and outside the resident payload budget.
    ShaderObservation Observe(ShaderTicket draw, std::uint32_t shader_value) noexcept;

private:
    struct Device;
    struct Payload {
        std::unique_ptr<std::uint32_t[]> words;
        std::size_t size = 0;
    };
    struct Entry {
        bool occupied = false, available = false, queued = false;
        std::uint32_t handle = 0;
        std::uint64_t generation = 0;
        ShaderUnavailableReason reason = ShaderUnavailableReason::None;
        DeclarationStatus scan_status = DeclarationStatus::Unreadable;
        bool reset_history = false;
        std::uint64_t reset_epoch = 0;
        std::uint32_t reset_hr = 0;
        Device* owner = nullptr;
        Entry* retired_next = nullptr;
        std::shared_ptr<const Payload> payload;
    };
    void EnqueueLocked(Entry& entry) noexcept;
    struct Device {
        bool active = false, closing = false, unsafe = false;
        std::uint64_t id = 0;
        std::uint64_t next_operation = 1, next_generation = 1;
        ShaderTicket pending{};
        std::uint32_t pending_handle = 0;
        std::uint32_t rebind_value = 0;
        std::uint64_t rebind_epoch = 0;
        std::size_t words = 0;
        std::size_t preparing = 0, retiring = 0;
        std::vector<Entry> handles;
    };
    void PoisonLocked(Device& device) noexcept;
    Device* FindLocked(std::uint64_t id, bool include_closing = false) noexcept;
    Entry* FindHandleLocked(Device& device, std::uint32_t handle) noexcept;
    Entry* FreeHandleLocked(Device& device) noexcept;
    void FinishClosingLocked(Device& device) noexcept;
    void FinishPreparation(std::uint64_t device, std::size_t words) noexcept;
    void Drain() noexcept;
    ShaderTicket Begin(std::uint64_t device, ShaderOperation kind, std::uint32_t handle) noexcept;
    bool Matches(const Device& device, ShaderTicket ticket) const noexcept;

    const ShaderRegistryLimits limits_;
    std::mutex mutex_;
    std::uint64_t next_device_ = 1;
    std::vector<Device> devices_;
    Entry* retired_head_ = nullptr;
    Entry* retired_tail_ = nullptr;
    std::size_t retired_count_ = 0;
    std::uint64_t retire_epoch_ = 0;
};

} // namespace t3render::ir
