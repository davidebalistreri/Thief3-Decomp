#pragma once

#include "scene_bridge_asset.hpp"

#include <cstddef>
#include <cstdint>

namespace t3render::scene_bridge {

// These are bounded, transient diagnostics for one in-process observation.
// Addresses must never enter a serialized Scene IR or be treated as stable IDs.
constexpr std::size_t kMaxProbeEntries = 128;
constexpr std::size_t kMaxTargetObjects = 65536;
constexpr std::size_t kMaxTargetListEntries = 8192;

struct ProbeValue {
    std::uint32_t value = 0;
    bool available = false;
};

struct ProbeCamera {
    ProbeValue address;
    std::uint32_t source_matrix_bits[16]{};  // camera+0x04; semantics not yet established
    std::uint32_t projection_bits[16]{};     // camera+0x64; raw IEEE-754 bits
    std::uint32_t derived_matrix_bits[16]{}; // camera+0xe4; semantics not yet established
    std::uint32_t combined_matrix_bits[16]{}; // camera+0x124; raw view/projection candidate
    std::uint32_t viewport_bits[6]{};        // camera+0x164; raw D3D8 viewport fields
    bool source_matrix_available = false;
    bool projection_available = false;
    bool derived_matrix_available = false;
    bool combined_matrix_available = false;
    bool viewport_available = false;
    bool stable = false;
};

struct ProbeEntry {
    std::uint32_t list_index = 0;
    ProbeValue address;
    ProbeValue vtable;
    ProbeValue type;     // element+0x44, semantics remain unverified
    ProbeValue flags;    // byte element+0x48, widened for serialization
    ProbeValue owner_candidate; // element+0x54 only for the verified static-mesh type
    ProbeValue owner_object_index;
    ProbeValue owner_fname;
    ProbeValue owner_class;
    ProbeValue owner_backlink;
    bool owner_table_member = false;
    bool owner_stable = false;
    bool owner_name_available = false;
    char owner_name[64]{}; // FName base text; instance number remains in owner_fname
    bool owner_location_available = false;
    bool owner_rotation_available = false;
    bool object_matrix_available = false;
    std::uint32_t owner_location_bits[3]{};
    std::uint32_t owner_rotation_bits[3]{};
    std::uint32_t object_matrix_bits[16]{};
};

struct ProbeNamedObject {
    ProbeValue address;
    ProbeValue index;
    ProbeValue fname;
    ProbeValue outer;
    bool member = false;
    bool stable = false;
    bool name_available = false;
    char name[64]{};
};

struct ProbeTarget {
    bool object_scan_complete = false;
    bool list_scan_complete = false;
    bool object_scan_truncated = false;
    bool list_scan_truncated = false;
    std::uint32_t objects_scanned = 0;
    std::uint32_t actors_named_target = 0;
    std::uint32_t list_scanned = 0;
    std::uint32_t list_occurrences = 0;
    bool entry_available = false;
    ProbeEntry entry;
    ProbeNamedObject owner_class;
    ProbeNamedObject outer[4];
    std::uint32_t outer_count = 0;
    bool outer_truncated = false;
    AssetCandidate asset; // target only; candidate until runtime evidence is corroborated
};

struct ProbeSnapshot {
    std::uint32_t thread_id = 0;
    std::uint32_t self = 0;  // saved ECX at 0x10926800
    std::uint32_t caller = 0;
    bool known_caller = false; // direct call at 0x109271a9 resumes at 0x109271ae
    ProbeValue pass;
    bool pass_stable = false;
    ProbeCamera configured_camera;
    ProbeCamera current_camera;
    ProbeCamera culling_camera;
    ProbeValue count;
    ProbeValue array;
    bool object_table_complete = false;
    std::uint32_t entries_attempted = 0;
    bool entries_truncated = false;
    ProbeEntry entries[kMaxProbeEntries]{};
    ProbeTarget target;  // targeted search of StaticMeshActor__1520, independently bounded
};

// Begin marks the entire sampling interval. Zero declines the observation;
// End must copy the snapshot synchronously, before the callback returns.
using BeginProbe = std::uint64_t (*)(std::uint32_t thread_id) noexcept;
using EndProbe = void (*)(std::uint64_t ticket, const ProbeSnapshot&) noexcept;
// Called on a nested hook entry. Must be lock-free and must not call game code.
using MarkProbeReentry = void (*)() noexcept;

enum class ProbeStatus {
    Installed, Disabled, InvalidArguments, AlreadyAttempted, UnsupportedProcessor,
    ExecutableIdentityUnavailable, ExecutableMismatch, ImageMismatch, PrologueMismatch,
    ModulePinFailed, HookEngineFailed, HookCreateFailed, HookEnableFailed, HookDisableFailed
};

// Explicit opt-in, called outside loader lock. Validates the executable hash,
// mapped PE, image base, timestamp and entry bytes before modifying code.
bool InstallProbe(BeginProbe begin, EndProbe end, MarkProbeReentry reentry, ProbeStatus* status) noexcept;

// Restores the original entry but retains the trampoline/thunk until process
// exit, because a thread may already be in flight when the hook is disabled.
bool DisableProbe(ProbeStatus* status) noexcept;

#if defined(T3RENDER_SCENE_BRIDGE_TEST_FIXTURE)
// Available only in a separate fixture executable, never in the game proxy.
bool InstallFixtureProbe(void* target, BeginProbe begin, EndProbe end, MarkProbeReentry reentry, ProbeStatus* status) noexcept;
#endif

}  // namespace t3render::scene_bridge
