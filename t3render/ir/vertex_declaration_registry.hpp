#pragma once

#include "vertex_declaration.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace t3render::ir {

// Portable experimental registry, not connected to the proxy. A device token
// is unique to this registry lifetime and must represent one observed COM
// device lifetime; it is not a native pointer or persistent asset identity.
// Entries are candidates from application memory, not proof of driver input,
// programmable-shader identity, or a distinction between FVF and handle.
enum class ShaderOperation { None, Create, Delete, Set, Reset, Draw };
struct ShaderTicket {
    std::uint64_t device = 0;
    std::uint64_t sequence = 0;
    ShaderOperation kind = ShaderOperation::None;
    explicit operator bool() const noexcept { return device && sequence && kind != ShaderOperation::None; }
};
enum class ShaderObservationStatus { Candidate, Unknown, Unavailable, Unsafe };
struct ShaderObservation {
    ShaderObservationStatus status = ShaderObservationStatus::Unknown;
    std::uint64_t generation = 0;
    std::vector<std::uint32_t> words;
};
struct ShaderRegistryLimits {
    std::size_t devices = 8;
    std::size_t handles_per_device = 1024;
    std::size_t words_per_declaration = 1024;
    std::size_t words_per_device = 65536;
};

class VertexDeclarationRegistry final {
public:
    explicit VertexDeclarationRegistry(ShaderRegistryLimits limits = {}) noexcept : limits_(limits) {}
    VertexDeclarationRegistry(const VertexDeclarationRegistry&) = delete;
    VertexDeclarationRegistry& operator=(const VertexDeclarationRegistry&) = delete;

    std::uint64_t OpenDevice() noexcept; // 0 on failure; never reuses an ID
    void CloseDevice(std::uint64_t device) noexcept;
    // Unsafe escape or unobserved mutation permanently invalidates this device.
    void Invalidate(std::uint64_t device) noexcept;

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
    bool End(ShaderTicket ticket) noexcept;

    // Only during a live Draw ticket. The caller must also require End(draw)
    // to succeed after the native draw, otherwise discard this observation.
    // A numeric match is not proof of shader vs FVF or exact driver input.
    ShaderObservation Observe(ShaderTicket draw, std::uint32_t shader_value) noexcept;

private:
    struct Entry { std::uint64_t generation = 0; bool available = false; std::vector<std::uint32_t> words; };
    struct Device {
        bool unsafe = false;
        std::uint64_t next_operation = 1, next_generation = 1;
        ShaderTicket pending{};
        std::uint32_t pending_handle = 0;
        std::size_t words = 0;
        std::unordered_map<std::uint32_t, Entry> handles;
    };
    void Poison(Device& device) noexcept;
    ShaderTicket Begin(std::uint64_t device, ShaderOperation kind, std::uint32_t handle) noexcept;
    bool Matches(const Device& device, ShaderTicket ticket) const noexcept;

    const ShaderRegistryLimits limits_;
    std::mutex mutex_;
    std::uint64_t next_device_ = 1;
    std::unordered_map<std::uint64_t, Device> devices_;
};

} // namespace t3render::ir
