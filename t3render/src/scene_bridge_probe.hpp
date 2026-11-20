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
constexpr std::size_t kMaxMeshNodeListEntries = 8192;
constexpr std::size_t kMaxMeshNodeSections = 16;
constexpr std::size_t kMaxProbeBspBlocks = 16;
constexpr std::size_t kMaxProbeBspSections = 16;
constexpr std::size_t kMaxMapWitnessCandidates = 8;

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

// Runtime mesh metadata, sampled through the target Flesh element. This is
// diagnostic only: no COM objects, geometry payloads or durable IDs.
struct ProbeMeshSection {
    bool name_available = false, material_name_available = false;
    char name[128]{}, material_name[128]{};
    std::uint32_t triangles = 0, first_vertex = 0, vertex_count = 0;
    std::uint32_t first_index = 0, index_count = 0, unknown = 0;
};

struct ProbeMeshNode {
    bool pool_scan_complete = false;
    bool pool_scan_truncated = false;
    std::uint32_t nodes_scanned = 0, occurrences = 0;
    ProbeValue address, vtable, name_address;
    bool name_available = false;
    char name[128]{};
    bool counts_available = false, bounds_available = false, stable = false;
    std::uint32_t vertex_count = 0, triangle_count = 0, index_count = 0, section_count = 0;
    std::uint32_t bounds_bits[6]{};
    ProbeValue section_array, instance_materials;
    bool sections_available = false, sections_truncated = false;
    std::uint32_t sections_copied = 0;
    ProbeMeshSection sections[kMaxMeshNodeSections]{};
};

struct ProbeBspSection {
    bool material_name_available = false;
    char material_name[128]{};
    std::uint32_t triangles = 0, first_vertex = 0, vertex_count = 0;
    std::uint32_t first_index = 0, index_count = 0, unknown = 0;
};

struct ProbeBspBlock {
    std::uint32_t list_index = 0, source_index = 0;
    ProbeValue address;
    bool metadata_available = false, stable = false;
    std::uint32_t vertex_count = 0, triangle_count = 0, index_count = 0, section_count = 0;
    std::uint32_t bounds_bits[6]{}, transform_bits[16]{};
    ProbeValue section_array;
    bool sections_available = false, sections_truncated = false;
    std::uint32_t sections_copied = 0;
    ProbeBspSection sections[kMaxProbeBspSections]{};
};

struct ProbeBsp {
    ProbeValue renderer, source, blocks;
    std::uint32_t block_count = 0, type5_seen = 0, member_count = 0, candidates_copied = 0;
    bool source_stable = false, candidates_truncated = false;
    ProbeBspBlock candidates[kMaxProbeBspBlocks]{};
};

struct ProbeTarget {
    // Live fast path proves uniqueness only within the complete visible list;
    // the full diagnostic path separately scans every UObject for the name.
    bool visible_list_only = false;
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
    ProbeMeshNode mesh_node;
};

// One current visible StaticMeshActor can witness a map name when the bed is
// absent. This is a bounded candidate, not proof of the active map or VBSP
// ownership; it contains transient addresses and is never serialized as IR.
struct ProbeMapWitness {
    bool available = false, list_stable = false, object_table_complete = false;
    ProbeEntry entry;
    ProbeValue owner_outer;
    ProbeNamedObject owner_class, outer;
};

// Transient evidence for one selected visible static mesh. It is sampled
// inside its submission scope and never becomes a serialized engine pointer.
enum class StaticMeshSampleStage : std::uint32_t {
    Initial, Header, List, Unique, Owner, Names, Assets, Complete
};
struct ProbeStaticMesh {
    StaticMeshSampleStage diagnostic_stage = StaticMeshSampleStage::Initial;
    bool list_stable = false, object_table_complete = false;
    std::uint32_t name_occurrences = 0;
    ProbeEntry entry;
    ProbeValue owner_outer;
    ProbeNamedObject owner_class, outer;
    AssetCandidate asset;
    ProbeMeshNode mesh_node;
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
    ProbeMapWitness map_witness;
    ProbeBsp bsp;        // candidates proven members of the current VBSP array
};

// Begin marks the entire sampling interval. Zero declines the observation;
// End must copy the snapshot synchronously, before the callback returns.
using BeginProbe = std::uint64_t (*)(std::uint32_t thread_id) noexcept;
using EndProbe = void (*)(std::uint64_t ticket, const ProbeSnapshot&) noexcept;
// Return true only for the lightweight live sample. Full trace captures keep
// the complete diagnostic scan and its stronger uniqueness evidence.
using SelectLiveProbe = bool (*)(std::uint64_t ticket) noexcept;
// Called on a nested hook entry. Must be lock-free and must not call game code.
using MarkProbeReentry = void (*)() noexcept;
// Callbacks bracket the virtual draw call at the verified section callsite.
// Values are transient process-local diagnostics, never SceneIR identifiers.
using EnterSubmission = void (*)(std::uint32_t element, std::uint32_t stage,
                                 std::uint32_t section) noexcept;
using LeaveSubmission = void (*)() noexcept;

enum class ProbeStatus {
    Installed, Disabled, InvalidArguments, AlreadyAttempted, UnsupportedProcessor,
    ExecutableIdentityUnavailable, ExecutableMismatch, ImageMismatch, PrologueMismatch,
    ModulePinFailed, HookEngineFailed, HookCreateFailed, HookEnableFailed, HookDisableFailed
};

// Explicit opt-in, called outside loader lock. Validates the executable hash,
// mapped PE, image base, timestamp and entry bytes before modifying code.
bool InstallProbe(BeginProbe begin, EndProbe end, MarkProbeReentry reentry, ProbeStatus* status,
                  SelectLiveProbe live_sample = nullptr) noexcept;

// Restores the original entry but retains the trampoline/thunk until process
// exit, because a thread may already be in flight when the hook is disabled.
bool DisableProbe(ProbeStatus* status) noexcept;

bool InstallSubmissionProbe(EnterSubmission enter, LeaveSubmission leave, ProbeStatus* status) noexcept;
bool DisableSubmissionProbe(ProbeStatus* status) noexcept;

#if defined(T3RENDER_SCENE_BRIDGE_TEST_FIXTURE)
// Available only in a separate fixture executable, never in the game proxy.
bool InstallFixtureProbe(void* target, BeginProbe begin, EndProbe end, MarkProbeReentry reentry, ProbeStatus* status,
                         SelectLiveProbe live_sample = nullptr) noexcept;
bool InstallFixtureSubmissionProbe(void* target, EnterSubmission enter, LeaveSubmission leave, ProbeStatus* status) noexcept;
#endif

}  // namespace t3render::scene_bridge
