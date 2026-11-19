// Read-only, opt-in D3D8 diagnostic snapshots. A capture is deliberately not a
// replay: resource contents and calls that do not have getters are unavailable.
#include "capture.hpp"
#include "scene_bridge_submission.hpp"
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
#include "scene_live_runtime.hpp"
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace t3render {
namespace {

constexpr unsigned kLightLimit = 32;
constexpr unsigned kStageLimit = 8;
constexpr unsigned kStreamLimit = 16;
constexpr unsigned kWorldLimit = 4;
constexpr uint64_t kUpComponentLimit = 256 * 1024;
constexpr uint64_t kUpFrameLimit = 8 * 1024 * 1024;
constexpr uint64_t kBufferComponentLimit = 256 * 1024;
constexpr uint64_t kBufferFrameLimit = 8 * 1024 * 1024;
constexpr uint64_t kHistoryObservationWords = 1024;
constexpr uint64_t kHistoryFrameWords = 262144;
constexpr uint64_t kHistoryDeviceWords = 8192;
constexpr uint64_t kProgramObservationWords = 1024;
constexpr uint64_t kProgramFrameWords = 262144;
constexpr uint64_t kVertexConstantRegisters = 96;
constexpr uint64_t kVertexConstantFrameWords = 262144;
constexpr DWORD kClearRectLimit = 256;
constexpr size_t kPendingOperationLimit = 4096;
constexpr size_t kBridgeObservationLimit = 16;
constexpr size_t kSubmissionLimit = 4096;
constexpr size_t kSubmissionDepth = 16;
// Nested hook entries can occur while the writer mutex is held. Report them
// without acquiring a lock; saturation fails closed for all later frames.
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "probe reentry counter must be lock-free");
std::atomic<std::uint32_t> bridge_reentry_count{0};
enum class OperationKind { Draw, Clear };

struct BridgeObservation {
    uint64_t frame = 0, before_operation = 0;
    std::uint32_t thread_id = 0;
    uint64_t thread_birth = 0;
};

struct BridgeThread {
    std::uint32_t id = 0;
    uint64_t birth = 0;
};

BridgeThread CurrentBridgeThread() noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    BridgeThread result;
    result.id = static_cast<std::uint32_t>(GetCurrentThreadId());
    if (!result.id || !GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return {};
    result.birth = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    if (!result.birth) return {};
    return result;
}

// The writer owns no D3D objects; resource interfaces returned by getters are
// released immediately. All state is protected because Present can be called
// from more than one thread when a multithreaded device is used.
struct CaptureState {
    std::mutex mutex;
    bool initialized = false;
    bool enabled = false;
    bool complete = false;
    bool key_trigger = false;
    bool file_trigger = false;
    bool key_down = false;
    bool triggered = false;
    bool buffer_requested = false;
    bool declaration_requested = false;
    bool history_requested = false;
    bool vertex_programs_requested = false;
    bool vertex_constants_requested = false;
    bool frame_images_requested = false;
    bool frame_image_attempted = false;
    bool bridge_requested = false;
    bool submission_requested = false;
    scene_bridge::ProbeStatus submission_install_status = scene_bridge::ProbeStatus::InvalidArguments;
    uint64_t submission_next_ticket = 1, submission_started = 0, submission_completed = 0;
    uint64_t submission_pending = 0, submission_dropped = 0;
    bool bridge_install_known = false;
    scene_bridge::ProbeStatus bridge_install_status = scene_bridge::ProbeStatus::InvalidArguments;
    bool bridge_epoch_unknown = false;
    bool bridge_previous_present_known = false;
    std::uint32_t bridge_previous_present_thread = 0;
    uint64_t bridge_previous_present_birth = 0;
    uint64_t bridge_next_ticket = 1;
    std::uint32_t bridge_reentry_baseline = 0;
    uint64_t bridge_present_ticket = 0;
    std::uint32_t bridge_present_thread = 0;
    uint64_t bridge_present_birth = 0;
    uint64_t bridge_reset_ticket = 0, bridge_reset_birth = 0;
    std::uint32_t bridge_reset_thread = 0;
    bool bridge_reset_seen = false;
    std::unordered_map<uint64_t, BridgeObservation> bridge_pending;
    uint64_t bridge_started = 0, bridge_completed = 0, bridge_dropped = 0;
    bool bridge_probe_overlap = false, bridge_present_overlap = false, bridge_operation_overlap = false;
    std::uint32_t bridge_draw_thread = 0, bridge_probe_thread = 0;
    uint64_t bridge_draw_birth = 0, bridge_probe_birth = 0;
    bool bridge_draw_multithread = false, bridge_probe_multithread = false;
    bool experimental_facade = false;
    bool experimental_repeated_unlock = false;
    bool experimental_repeat_recovery = false;
    bool begun = false;
    bool had_reset = false;
    uint64_t frame = 0;
    uint64_t target = 0;
    unsigned frames_requested = 1;
    unsigned frames_written = 0;
    uint64_t sample_interval_ms = 0, last_sample_tick = 0;
    std::vector<uint64_t> sampled_live_frames;
    uint64_t draws = 0;
    uint64_t up_bytes = 0;  // successfully emitted raw bytes, not hex characters
    uint64_t buffer_bytes = 0;
    uint64_t history_words = 0;
    uint64_t vertex_program_words = 0;
    uint64_t vertex_constant_words = 0;
    uint64_t next_operation = 1;
    bool concurrent_operations = false;
    std::unordered_map<uint64_t, OperationKind> pending_operations;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    std::wstring trigger_path;
    IDirect3DDevice8* device = nullptr;  // identity only; never dereferenced after replacement
    D3DPRESENT_PARAMETERS presentation{};
    bool has_presentation = false;
    D3DCAPS8 caps{};
    bool has_caps = false;

    ~CaptureState() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

// Driver getters/readback run under the writer mutex. If a callback into the
// engine reaches the probe on this same thread, taking that mutex recursively
// would deadlock. Decline it and mark the interval ambiguous when the native
// query returns; no game state is changed.
thread_local unsigned bridge_query_depth = 0;
thread_local bool bridge_reentered_query = false;
struct SubmissionScope {
    uint64_t ticket = 0, frame = 0;
    std::uint32_t thread = 0;
    scene_bridge::SubmissionSample sample{};
};
struct SubmissionStack {
    unsigned depth = 0;
    SubmissionScope scopes[kSubmissionDepth]{};
};
thread_local SubmissionStack submission_stack;

bool SubmissionRead(void*, std::uint32_t address, void* destination, std::size_t size) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
                             destination, size, &read) && read == size;
}
struct BridgeQueryGuard {
    CaptureState& state;
    explicit BridgeQueryGuard(CaptureState& value) noexcept : state(value) { ++bridge_query_depth; }
    ~BridgeQueryGuard() {
        if (--bridge_query_depth == 0 && bridge_reentered_query) {
            bridge_reentered_query = false;
            if (state.bridge_requested && state.triggered && state.frame == state.target) {
                state.bridge_operation_overlap = true;
                ++state.bridge_dropped;
            }
        }
    }
};

CaptureState& State() {
    static CaptureState state;
    return state;
}

// No exceptions, invalid handles or partial writes propagate to the game.
void Stop(CaptureState& s) {
    s.enabled = false;
    if (s.file != INVALID_HANDLE_VALUE) {
        CloseHandle(s.file);
        s.file = INVALID_HANDLE_VALUE;
    }
}

bool Write(CaptureState& s, const std::string& line) {
    if (!s.enabled) return false;
    size_t offset = 0;
    while (offset < line.size()) {
        const size_t left = line.size() - offset;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 0x7fffffff));
        DWORD written = 0;
        if (!WriteFile(s.file, line.data() + offset, chunk, &written, nullptr) || written == 0) {
            Stop(s);
            return false;
        }
        offset += written;
    }
    return true;
}

std::wstring Environment(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required || required > 32767) return {};
    std::vector<wchar_t> value(required);
    const DWORD actual = GetEnvironmentVariableW(name, value.data(), required);
    if (!actual || actual >= required) return {};
    return std::wstring(value.data(), actual);
}

std::ostringstream Json() {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    return out;
}

const char* BridgeStatusName(scene_bridge::ProbeStatus status) noexcept {
    using scene_bridge::ProbeStatus;
    switch (status) {
        case ProbeStatus::Installed: return "installed";
        case ProbeStatus::Disabled: return "disabled";
        case ProbeStatus::InvalidArguments: return "invalid_arguments";
        case ProbeStatus::AlreadyAttempted: return "already_attempted";
        case ProbeStatus::UnsupportedProcessor: return "unsupported_processor";
        case ProbeStatus::ExecutableIdentityUnavailable: return "executable_identity_unavailable";
        case ProbeStatus::ExecutableMismatch: return "executable_mismatch";
        case ProbeStatus::ImageMismatch: return "image_mismatch";
        case ProbeStatus::PrologueMismatch: return "prologue_mismatch";
        case ProbeStatus::ModulePinFailed: return "module_pin_failed";
        case ProbeStatus::HookEngineFailed: return "hook_engine_failed";
        case ProbeStatus::HookCreateFailed: return "hook_create_failed";
        case ProbeStatus::HookEnableFailed: return "hook_enable_failed";
        case ProbeStatus::HookDisableFailed: return "hook_disable_failed";
    }
    return "unknown";
}

void Number(std::ostream& out, float value) {
    if (std::isfinite(value)) out << value;
    else out << "null";  // JSON does not have NaN or infinity
}

void Id(std::ostream& out, const void* pointer) {
    if (!pointer) { out << "null"; return; }
    out << '"' << "0x" << std::hex << reinterpret_cast<uintptr_t>(pointer) << std::dec << '"';
}

void BridgeValue(std::ostream& out, const scene_bridge::ProbeValue& value) {
    if (value.available) out << value.value;
    else out << "null";
}

void BridgeBits(std::ostream& out, const std::uint32_t* data, size_t count, bool available) {
    if (!available) { out << "null"; return; }
    out << '[';
    for (size_t i = 0; i < count; ++i) { if (i) out << ','; out << data[i]; }
    out << ']';
}

void BridgeText(std::ostream& out, const char* text, size_t capacity, bool available) {
    if (!available) { out << "null"; return; }
    static constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (size_t i = 0; i < capacity && text[i]; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32 || c >= 127) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << static_cast<char>(c);
    }
    out << '"';
}

void BridgeCamera(std::ostream& out, const scene_bridge::ProbeCamera& camera) {
    out << "{\"address\":"; BridgeValue(out, camera.address);
    out << ",\"stable\":" << (camera.stable ? "true" : "false") << ",\"source_matrix_bits\":";
    BridgeBits(out, camera.source_matrix_bits, 16, camera.source_matrix_available);
    out << ",\"projection_bits\":"; BridgeBits(out, camera.projection_bits, 16, camera.projection_available);
    out << ",\"derived_matrix_bits\":"; BridgeBits(out, camera.derived_matrix_bits, 16, camera.derived_matrix_available);
    out << ",\"combined_matrix_bits\":"; BridgeBits(out, camera.combined_matrix_bits, 16, camera.combined_matrix_available);
    out << ",\"viewport_bits\":"; BridgeBits(out, camera.viewport_bits, 6, camera.viewport_available);
    out << '}';
}

void BridgeEntry(std::ostream& out, const scene_bridge::ProbeEntry& entry) {
    out << "{\"list_index\":" << entry.list_index << ",\"address\":"; BridgeValue(out, entry.address);
    out << ",\"vtable\":"; BridgeValue(out, entry.vtable);
    out << ",\"type\":"; BridgeValue(out, entry.type);
    out << ",\"flags\":"; BridgeValue(out, entry.flags);
    out << ",\"owner_candidate\":"; BridgeValue(out, entry.owner_candidate);
    out << ",\"owner_object_index\":"; BridgeValue(out, entry.owner_object_index);
    out << ",\"owner_fname\":"; BridgeValue(out, entry.owner_fname);
    out << ",\"owner_class\":"; BridgeValue(out, entry.owner_class);
    out << ",\"owner_backlink\":"; BridgeValue(out, entry.owner_backlink);
    out << ",\"owner_table_member\":" << (entry.owner_table_member ? "true" : "false")
        << ",\"owner_stable\":" << (entry.owner_stable ? "true" : "false") << ",\"owner_name\":";
    BridgeText(out, entry.owner_name, sizeof(entry.owner_name), entry.owner_name_available);
    out << ",\"owner_location_bits\":"; BridgeBits(out, entry.owner_location_bits, 3, entry.owner_location_available);
    out << ",\"owner_rotation_bits\":"; BridgeBits(out, entry.owner_rotation_bits, 3, entry.owner_rotation_available);
    out << ",\"object_matrix_bits\":"; BridgeBits(out, entry.object_matrix_bits, 16, entry.object_matrix_available);
    out << '}';
}

void BridgeNamedObject(std::ostream& out, const scene_bridge::ProbeNamedObject& object) {
    out << "{\"address\":"; BridgeValue(out, object.address);
    out << ",\"index\":"; BridgeValue(out, object.index);
    out << ",\"fname\":"; BridgeValue(out, object.fname);
    out << ",\"outer\":"; BridgeValue(out, object.outer);
    out << ",\"member\":" << (object.member ? "true" : "false")
        << ",\"stable\":" << (object.stable ? "true" : "false") << ",\"name\":";
    BridgeText(out, object.name, sizeof(object.name), object.name_available);
    out << '}';
}

void BridgeMapWitness(std::ostream& out, const scene_bridge::ProbeMapWitness& witness) {
    if (!witness.available) { out << "null"; return; }
    out << "{\"list_stable\":" << (witness.list_stable ? "true" : "false")
        << ",\"object_table_complete\":" << (witness.object_table_complete ? "true" : "false")
        << ",\"entry\":"; BridgeEntry(out,witness.entry);
    out << ",\"owner_outer\":"; BridgeValue(out,witness.owner_outer);
    out << ",\"owner_class\":"; BridgeNamedObject(out,witness.owner_class);
    out << ",\"outer\":"; BridgeNamedObject(out,witness.outer); out << '}';
}

const char* BridgeAssetStatusName(scene_bridge::AssetStatus status) noexcept {
    using scene_bridge::AssetStatus;
    switch (status) {
        case AssetStatus::Unknown: return "unknown";
        case AssetStatus::Unreadable: return "unreadable";
        case AssetStatus::DescriptorUnsupported: return "descriptor_unsupported";
        case AssetStatus::LocalPropertyMissing: return "local_property_missing";
        case AssetStatus::PropertyMalformed: return "property_malformed";
        case AssetStatus::ObjectNotUnique: return "object_not_unique";
        case AssetStatus::ProxyMismatch: return "proxy_mismatch";
        case AssetStatus::ResourceMismatch: return "resource_mismatch";
        case AssetStatus::Candidate: return "candidate";
    }
    return "unknown";
}

void BridgeAsset(std::ostream& out, const scene_bridge::AssetCandidate& asset) {
    out << "{\"status\":\"" << BridgeAssetStatusName(asset.status) << "\",\"descriptor_match\":"
        << (asset.descriptor_match ? "true" : "false") << ",\"local_property_found\":"
        << (asset.local_property_found ? "true" : "false") << ",\"proxy_live_member\":"
        << (asset.proxy_live_member ? "true" : "false") << ",\"proxy_vtable_match\":"
        << (asset.proxy_vtable_match ? "true" : "false") << ",\"resource_vtable_match\":"
        << (asset.resource_vtable_match ? "true" : "false") << ",\"stable\":"
        << (asset.stable ? "true" : "false") << ",\"name\":";
    BridgeText(out, asset.name, sizeof(asset.name), asset.name_available);
    out << ",\"audit\":";
    if (!asset.audit.available) out << "null";
    else {
        const auto& a = asset.audit;
        out << "{\"descriptor_table\":" << a.descriptor_table << ",\"descriptor_array\":" << a.descriptor_array
            << ",\"descriptor\":" << a.descriptor << ",\"descriptor_vtable\":" << a.descriptor_vtable
            << ",\"descriptor_copy\":" << a.descriptor_copy << ",\"descriptor_count\":" << a.descriptor_count
            << ",\"descriptor_size\":" << a.descriptor_size << ",\"descriptor_flags\":" << a.descriptor_flags
            << ",\"owner_flags\":" << a.owner_flags << ",\"property_hash\":" << a.property_hash
            << ",\"bucket_head\":" << a.bucket_head << ",\"slot_index\":" << a.slot_index
            << ",\"slot_next\":" << a.slot_next << ",\"slot_id_flags\":" << a.slot_id_flags
            << ",\"slot_value\":" << a.slot_value << ",\"chain\":[";
        for (std::size_t i = 0; i < a.chain_count; ++i) {
            if (i) out << ',';
            const auto& slot = a.chain[i];
            out << "{\"index\":" << slot.index << ",\"next\":" << slot.next
                << ",\"id_flags\":" << slot.id_flags << ",\"value\":" << slot.value << '}';
        }
        out << "],\"objects_data\":" << a.objects_data
            << ",\"objects_count\":" << a.objects_count << ",\"objects_capacity\":" << a.objects_capacity
            << ",\"object_index\":" << a.object_index << ",\"proxy\":" << a.proxy
            << ",\"proxy_vtable\":" << a.proxy_vtable << ",\"proxy_index\":" << a.proxy_index
            << ",\"resource\":" << a.resource
            << ",\"resource_vtable\":" << a.resource_vtable << ",\"name_address\":" << a.name_address
            << ",\"name_length\":" << a.name_length << '}';
    }
    out << '}';
}

void BridgeMeshNode(std::ostream& out, const scene_bridge::ProbeMeshNode& node) {
    out << "{\"pool_scan_complete\":" << (node.pool_scan_complete ? "true" : "false")
        << ",\"pool_scan_truncated\":" << (node.pool_scan_truncated ? "true" : "false")
        << ",\"nodes_scanned\":" << node.nodes_scanned << ",\"occurrences\":" << node.occurrences
        << ",\"address\":"; BridgeValue(out, node.address);
    out << ",\"vtable\":"; BridgeValue(out, node.vtable);
    out << ",\"name_address\":"; BridgeValue(out, node.name_address);
    out << ",\"name\":"; BridgeText(out, node.name, sizeof(node.name), node.name_available);
    out << ",\"counts\":";
    if (!node.counts_available) out << "null";
    else out << "{\"vertices\":" << node.vertex_count << ",\"triangles\":" << node.triangle_count
             << ",\"indices\":" << node.index_count << ",\"sections\":" << node.section_count << '}';
    out << ",\"bounds_bits\":"; BridgeBits(out, node.bounds_bits, 6, node.bounds_available);
    out << ",\"stable\":" << (node.stable ? "true" : "false") << ",\"section_array\":";
    BridgeValue(out, node.section_array);
    out << ",\"instance_materials\":"; BridgeValue(out, node.instance_materials);
    out << ",\"sections_truncated\":" << (node.sections_truncated ? "true" : "false") << ",\"sections\":";
    if (!node.sections_available) out << "null";
    else {
        out << '[';
        for (std::uint32_t i = 0; i < node.sections_copied; ++i) {
            if (i) out << ',';
            const auto& section = node.sections[i];
            out << "{\"name\":"; BridgeText(out, section.name, sizeof(section.name), section.name_available);
            out << ",\"material_name\":";
            BridgeText(out, section.material_name, sizeof(section.material_name), section.material_name_available);
            out << ",\"triangles\":" << section.triangles << ",\"first_vertex\":" << section.first_vertex
                << ",\"vertex_count\":" << section.vertex_count << ",\"first_index\":" << section.first_index
                << ",\"index_count\":" << section.index_count << ",\"unknown\":" << section.unknown << '}';
        }
        out << ']';
    }
    out << '}';
}

void BridgeTarget(std::ostream& out, const scene_bridge::ProbeTarget& target) {
    out << "{\"object_scan_complete\":" << (target.object_scan_complete ? "true" : "false")
        << ",\"list_scan_complete\":" << (target.list_scan_complete ? "true" : "false")
        << ",\"object_scan_truncated\":" << (target.object_scan_truncated ? "true" : "false")
        << ",\"list_scan_truncated\":" << (target.list_scan_truncated ? "true" : "false")
        << ",\"objects_scanned\":" << target.objects_scanned << ",\"actors_named_target\":" << target.actors_named_target
        << ",\"list_scanned\":" << target.list_scanned << ",\"list_occurrences\":" << target.list_occurrences
        << ",\"entry\":";
    if (target.entry_available) BridgeEntry(out, target.entry); else out << "null";
    out << ",\"owner_class\":"; BridgeNamedObject(out, target.owner_class);
    out << ",\"outer_count\":" << target.outer_count
        << ",\"outer_truncated\":" << (target.outer_truncated ? "true" : "false") << ",\"outer\":[";
    for (std::size_t i = 0; i < target.outer_count; ++i) {
        if (i) out << ',';
        BridgeNamedObject(out, target.outer[i]);
    }
    out << "],\"asset\":"; BridgeAsset(out, target.asset);
    out << ",\"mesh_node\":"; BridgeMeshNode(out, target.mesh_node); out << '}';
}

void BridgeBsp(std::ostream& out, const scene_bridge::ProbeBsp& bsp) {
    out << "{\"renderer\":"; BridgeValue(out, bsp.renderer);
    out << ",\"source\":"; BridgeValue(out, bsp.source);
    out << ",\"blocks\":"; BridgeValue(out, bsp.blocks);
    out << ",\"block_count\":" << bsp.block_count << ",\"type5_seen\":" << bsp.type5_seen
        << ",\"member_count\":" << bsp.member_count << ",\"source_stable\":" << (bsp.source_stable ? "true" : "false")
        << ",\"candidates_truncated\":" << (bsp.candidates_truncated ? "true" : "false") << ",\"candidates\":[";
    for (std::uint32_t i = 0; i < bsp.candidates_copied; ++i) {
        if (i) out << ',';
        const auto& block = bsp.candidates[i];
        out << "{\"list_index\":" << block.list_index << ",\"source_index\":" << block.source_index << ",\"address\":";
        BridgeValue(out, block.address);
        out << ",\"counts\":";
        if (!block.metadata_available) out << "null";
        else out << "{\"vertices\":" << block.vertex_count << ",\"triangles\":" << block.triangle_count
                 << ",\"indices\":" << block.index_count << ",\"sections\":" << block.section_count << '}';
        out << ",\"bounds_bits\":"; BridgeBits(out, block.bounds_bits, 6, block.metadata_available);
        out << ",\"transform_bits\":"; BridgeBits(out, block.transform_bits, 16, block.metadata_available);
        out << ",\"stable\":" << (block.stable ? "true" : "false") << ",\"section_array\":";
        BridgeValue(out, block.section_array);
        out << ",\"sections_truncated\":" << (block.sections_truncated ? "true" : "false") << ",\"sections\":";
        if (!block.sections_available) out << "null";
        else {
            out << '[';
            for (std::uint32_t j = 0; j < block.sections_copied; ++j) {
                if (j) out << ',';
                const auto& section = block.sections[j];
                out << "{\"material_name\":";
                BridgeText(out, section.material_name, sizeof(section.material_name), section.material_name_available);
                out << ",\"triangles\":" << section.triangles << ",\"first_vertex\":" << section.first_vertex
                    << ",\"vertex_count\":" << section.vertex_count << ",\"first_index\":" << section.first_index
                    << ",\"index_count\":" << section.index_count << ",\"unknown\":" << section.unknown << '}';
            }
            out << ']';
        }
        out << '}';
    }
    out << "]}";
}

template<typename T> struct ComRef {
    T* value = nullptr;
    ~ComRef() { if (value) value->Release(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef() = default;
};

void Initialize(CaptureState& s) {
    if (s.initialized) return;
    s.initialized = true;
    const std::wstring path = Environment(L"T3RENDER_CAPTURE_PATH");
    const std::wstring frame = Environment(L"T3RENDER_CAPTURE_FRAME");
    const std::wstring key = Environment(L"T3RENDER_CAPTURE_KEY");
    const std::wstring trigger_path = Environment(L"T3RENDER_CAPTURE_TRIGGER_PATH");
    s.buffer_requested = Environment(L"T3RENDER_CAPTURE_BUFFERS") == L"1";
    s.declaration_requested = Environment(L"T3RENDER_CAPTURE_DECLARATIONS") == L"1";
    s.history_requested = Environment(L"T3RENDER_CAPTURE_DECLARATION_HISTORY") == L"1";
    s.vertex_programs_requested = Environment(L"T3RENDER_CAPTURE_VERTEX_PROGRAMS") == L"1";
    s.vertex_constants_requested = Environment(L"T3RENDER_CAPTURE_VERTEX_CONSTANTS") == L"1";
    s.frame_images_requested = Environment(L"T3RENDER_CAPTURE_SCREENSHOTS") == L"1";
    s.bridge_requested = Environment(L"T3RENDER_SCENE_BRIDGE") == L"1";
    // Missing means the legacy single frame; an explicitly empty, truncated,
    // or malformed value must not silently start a capture.
    SetLastError(ERROR_SUCCESS);
    const DWORD interval_length = GetEnvironmentVariableW(L"T3RENDER_CAPTURE_INTERVAL_MS", nullptr, 0);
    if ((!interval_length && GetLastError() != ERROR_ENVVAR_NOT_FOUND) || interval_length > 6) return;
    const std::wstring interval = Environment(L"T3RENDER_CAPTURE_INTERVAL_MS");
    if (interval_length && interval.empty()) return;
    if (!interval.empty()) {
        if (interval[0] == L'0' || interval.size() > 5) return;
        for (wchar_t c : interval) if (c < L'0' || c > L'9') return;
        const auto parsed = std::wcstoul(interval.c_str(), nullptr, 10);
        if (parsed < 1000 || parsed > 10000) return;
        s.sample_interval_ms = parsed;
    }
    wchar_t frames[4]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD frames_length = GetEnvironmentVariableW(L"T3RENDER_CAPTURE_FRAMES", frames, 4);
    if (!frames_length) {
        if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) return;
    } else if (frames_length >= 4 || frames[0] < L'1' || frames[0] > L'9') {
        return;
    } else {
        for (DWORD i = 1; i < frames_length; ++i) if (frames[i] < L'0' || frames[i] > L'9') return;
        const auto parsed = std::wcstoul(frames, nullptr, 10);
        if (parsed < 1 || parsed > (s.sample_interval_ms ? 15u : 3u)) return;
        s.frames_requested = static_cast<unsigned>(parsed);
    }
    if (s.sample_interval_ms && s.frames_requested < 2) return;
    const unsigned triggers = static_cast<unsigned>(!frame.empty()) +
        static_cast<unsigned>(!key.empty()) + static_cast<unsigned>(!trigger_path.empty());
    if (path.empty() || triggers != 1) return;
    if (!key.empty()) {
        if (key != L"F11") return;
        s.key_trigger = true;
    } else if (!trigger_path.empty()) {
        // A stale signal must never accidentally capture the launch screen.
        // The parent directory must already exist and be accessible.
        if (path == trigger_path || GetFileAttributesW(trigger_path.c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetLastError() != ERROR_FILE_NOT_FOUND) return;
        s.trigger_path = trigger_path;
        s.file_trigger = true;
    } else {
        if (frame.empty() || frame[0] == L'-' || frame[0] == L'+') return;
        for (wchar_t c : frame) if (c < L'0' || c > L'9') return;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long parsed = std::wcstoull(frame.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end ||
            parsed > std::numeric_limits<uint64_t>::max() - s.frames_requested) return;
        s.target = static_cast<uint64_t>(parsed);
        s.triggered = true;
    }
    // No file is created until the requested frame actually begins. This also
    // lets level changes/relaunches before the F11 trigger leave no file behind.
    s.path = path;
    s.enabled = true;
}

bool Open(CaptureState& s) {
    if (s.file != INVALID_HANDLE_VALUE) return true;
    // CREATE_NEW deliberately refuses an existing capture; it never truncates it.
    s.file = CreateFileW(s.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) { Stop(s); return false; }
    auto out = Json();
    out << "{\"event\":\"header\",\"format\":\"t3render-d3d8-diagnostic\",\"version\":1,"
        << "\"trigger\":\"" << (s.key_trigger ? "F11" : s.file_trigger ? "file" : "frame") << "\","
        << "\"requested_frames\":" << s.frames_requested << ','
        << "\"operation_results\":1,\"clear_rect_limit\":" << kClearRectLimit << ','
        << "\"replayable\":false,\"up_capture\":{\"encoding\":\"hex\",\"component_limit_bytes\":"
        << kUpComponentLimit << ",\"frame_limit_bytes\":" << kUpFrameLimit << "},"
        << "\"buffer_capture\":{\"requested\":" << (s.buffer_requested ? "true" : "false")
        << ",\"encoding\":\"hex\",\"component_limit_bytes\":" << kBufferComponentLimit
        << ",\"frame_limit_bytes\":" << kBufferFrameLimit << "},\"declaration_capture\":{\"requested\":"
        << (s.declaration_requested ? "true" : "false") << '}';
    if (s.history_requested) out << ",\"declaration_history\":{\"requested\":true,\"words_per_observation\":"
        << kHistoryObservationWords << ",\"words_per_frame\":" << kHistoryFrameWords
        << ",\"words_per_device\":" << kHistoryDeviceWords << '}';
    if (s.vertex_programs_requested) out << ",\"vertex_program_capture\":{\"requested\":true,\"words_per_observation\":"
        << kProgramObservationWords << ",\"words_per_frame\":" << kProgramFrameWords << '}';
    if (s.vertex_constants_requested) out << ",\"vertex_constant_capture\":{\"requested\":true,\"registers_per_observation\":"
        << kVertexConstantRegisters << ",\"words_per_frame\":" << kVertexConstantFrameWords << '}';
    if (s.frame_images_requested) out << ",\"frame_images\":{\"requested\":true,\"timing\":\"before_present\"}";
    if (s.sample_interval_ms) out << ",\"sampling\":{\"interval_ms\":" << s.sample_interval_ms << '}';
    if (s.bridge_requested) out << ",\"scene_bridge\":{\"requested\":true,\"install_status\":\""
                                << (s.bridge_install_known ? BridgeStatusName(s.bridge_install_status) : "not_reported")
                                << "\",\"probe_limit\":" << kBridgeObservationLimit << ",\"entries_per_probe\":"
                                << scene_bridge::kMaxProbeEntries
                                << ",\"target_object_limit\":" << scene_bridge::kMaxTargetObjects
                                << ",\"target_list_limit\":" << scene_bridge::kMaxTargetListEntries
                                << ",\"reentry_detection\":\"saturating_counter_v1\""
                                << ",\"asset_audit\":\"local_property_v1\""
                                << ",\"mesh_node\":\"actor_node_v1\""
                                << ",\"mesh_sections\":\"actor_materials_v1\""
                                << ",\"bsp_candidates\":\"vbsp_array_v1\""
                                << ",\"map_witness\":\"visible_static_actor_v1\""
                                << ",\"synchronization\":\"present_interval_and_thread_only\""
                                << (s.submission_requested ? ",\"submission_scope\":\"section_call_v1\",\"submission_install_status\":\"" : "")
                                << (s.submission_requested ? BridgeStatusName(s.submission_install_status) : "")
                                << (s.submission_requested ? "\"" : "") << '}';
    if (s.experimental_facade) {
        out << ",\"experimental_facade\":{\"enabled\":true,\"identity_verified\":false,\"concurrency_verified\":false";
        if (s.experimental_repeated_unlock) out << ",\"repeated_unlock_resource_discard\":true";
        if (s.experimental_repeat_recovery) out << ",\"repeated_unlock_post_write_recovery\":true";
        out << '}';
    }
#ifdef T3RENDER_EXPERIMENTAL_COMBINED_PROXY
    if (const auto* session = live::RuntimeSessionId()) out << ",\"live_session_id\":\"" << session << '"';
#endif
    out << ",\"limitations\":["
        << "\"texture bytes and T3 asset names are not captured\","
        << "\"optional buffer shadows are bounded observations, not proof of driver input\","
        << "\"UP data is bounded, may be unavailable, and may change between snapshot and draw\","
        << "\"indexed UP vertex window uses MinVertexIndex times stride; nonzero behavior is not runtime verified\","
        << "\"draw and clear events describe attempts; results are paired by op_id\","
        << "\"concurrent device calls may have a different driver submission order\","
        << "\"concurrent_operations only covers draw/clear overlap, not Present or Reset\","
        << "\"vertex programs, declarations and constants are optional observations; pixel programs and constants are not captured\","
        << "\"light indices 0 through 31 only\","
        << "\"world transform indices 0 through 3 only\","
        << "\"texture stages at most 8 and vertex streams at most 16\","
        << "\"render and texture stage states limited to selected numeric IDs; failed getters omitted\","
        << "\"presentation is the last successful device creation or reset\","
        << "\"object identifiers are transient process addresses\","
        << "\"only one device is followed; already-created concurrent devices are not adopted\"]}" << '\n';
    return Write(s, out.str());
}

void Matrix(std::ostream& out, IDirect3DDevice8* device, D3DTRANSFORMSTATETYPE type) {
    D3DMATRIX value{};
    if (FAILED(device->GetTransform(type, &value))) { out << "null"; return; }
    out << '[';
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
        if (row || col) out << ',';
        Number(out, value.m[row][col]);
    }
    out << ']';
}

void Color(std::ostream& out, const D3DCOLORVALUE& value) {
    out << '['; Number(out, value.r); out << ','; Number(out, value.g); out << ',';
    Number(out, value.b); out << ','; Number(out, value.a); out << ']';
}

void Vector(std::ostream& out, const D3DVECTOR& value) {
    out << '['; Number(out, value.x); out << ','; Number(out, value.y); out << ',';
    Number(out, value.z); out << ']';
}

void Begin(CaptureState& s) {
    if (s.begun || !s.enabled || !Open(s)) return;
    auto out = Json();
    out << "{\"event\":\"frame_begin\",\"frame\":" << s.frame << ",\"device\":";
    Id(out, s.device);
    out << ",\"presentation\":";
    if (!s.has_presentation) out << "null";
    else {
        const auto& p = s.presentation;
        out << "{\"width\":" << p.BackBufferWidth << ",\"height\":" << p.BackBufferHeight
            << ",\"format\":" << static_cast<unsigned>(p.BackBufferFormat)
            << ",\"count\":" << p.BackBufferCount
            << ",\"multisample\":" << static_cast<unsigned>(p.MultiSampleType)
            << ",\"swap_effect\":" << static_cast<unsigned>(p.SwapEffect)
            << ",\"windowed\":" << (p.Windowed ? "true" : "false")
            << ",\"auto_depth_stencil\":" << (p.EnableAutoDepthStencil ? "true" : "false")
            << ",\"depth_stencil_format\":" << static_cast<unsigned>(p.AutoDepthStencilFormat)
            << ",\"flags\":" << p.Flags << ",\"refresh_hz\":" << p.FullScreen_RefreshRateInHz
            << ",\"present_interval\":" << p.FullScreen_PresentationInterval << '}';
    }
    out << ",\"caps\":";
    if (!s.has_caps) out << "null";
    else {
        out << "{\"texture_blend_stages\":" << s.caps.MaxTextureBlendStages
             << ",\"simultaneous_textures\":" << s.caps.MaxSimultaneousTextures
             << ",\"streams\":" << s.caps.MaxStreams
             << ",\"active_lights\":" << s.caps.MaxActiveLights;
        if (s.vertex_constants_requested) out << ",\"vertex_shader_constants\":" << s.caps.MaxVertexShaderConst;
        out << '}';
    }
    out << "}\n";
    if (Write(s, out.str())) s.begun = true;
}

void Texture(std::ostream& out, IDirect3DBaseTexture8* texture) {
    if (!texture) { out << "null"; return; }
    const auto type = texture->GetType();
    out << "{\"id\":"; Id(out, texture);
    out << ",\"type\":" << static_cast<unsigned>(type) << ",\"levels\":" << texture->GetLevelCount();
    if (type == D3DRTYPE_TEXTURE || type == D3DRTYPE_CUBETEXTURE) {
        D3DSURFACE_DESC d{};
        const HRESULT hr = type == D3DRTYPE_TEXTURE
            ? static_cast<IDirect3DTexture8*>(texture)->GetLevelDesc(0, &d)
            : static_cast<IDirect3DCubeTexture8*>(texture)->GetLevelDesc(0, &d);
        if (SUCCEEDED(hr)) out << ",\"format\":" << static_cast<unsigned>(d.Format)
                               << ",\"usage\":" << d.Usage << ",\"pool\":" << static_cast<unsigned>(d.Pool)
                               << ",\"width\":" << d.Width << ",\"height\":" << d.Height;
        else out << ",\"description_hr\":" << static_cast<int32_t>(hr);
    } else if (type == D3DRTYPE_VOLUMETEXTURE) {
        D3DVOLUME_DESC d{};
        const HRESULT hr = static_cast<IDirect3DVolumeTexture8*>(texture)->GetLevelDesc(0, &d);
        if (SUCCEEDED(hr)) out << ",\"format\":" << static_cast<unsigned>(d.Format)
                               << ",\"usage\":" << d.Usage << ",\"pool\":" << static_cast<unsigned>(d.Pool)
                               << ",\"width\":" << d.Width << ",\"height\":" << d.Height
                               << ",\"depth\":" << d.Depth;
        else out << ",\"description_hr\":" << static_cast<int32_t>(hr);
    }
    out << '}';
}

void Lights(std::ostream& out, IDirect3DDevice8* device) {
    out << '[';
    bool comma = false;
    for (unsigned index = 0; index < kLightLimit; ++index) {
        D3DLIGHT8 light{};
        if (FAILED(device->GetLight(index, &light))) continue;
        if (comma) out << ',';
        comma = true;
        out << "{\"index\":" << index << ",\"type\":" << static_cast<unsigned>(light.Type) << ",\"enabled\":";
        BOOL enabled = FALSE;
        const HRESULT hr = device->GetLightEnable(index, &enabled);
        if (FAILED(hr)) out << "null";
        else out << (enabled ? "true" : "false");
        out << ",\"diffuse\":"; Color(out, light.Diffuse);
        out << ",\"specular\":"; Color(out, light.Specular);
        out << ",\"ambient\":"; Color(out, light.Ambient);
        out << ",\"position\":"; Vector(out, light.Position);
        out << ",\"direction\":"; Vector(out, light.Direction);
        out << ",\"range\":"; Number(out, light.Range);
        out << ",\"falloff\":"; Number(out, light.Falloff);
        out << ",\"attenuation\":["; Number(out, light.Attenuation0); out << ',';
        Number(out, light.Attenuation1); out << ','; Number(out, light.Attenuation2);
        out << "],\"theta\":"; Number(out, light.Theta);
        out << ",\"phi\":"; Number(out, light.Phi); out << '}';
    }
    out << ']';
}

void BufferBindings(std::ostream& out, IDirect3DDevice8* device, unsigned stream_count) {
    out << "\"streams\":[";
    for (unsigned stream = 0; stream < stream_count; ++stream) {
        if (stream) out << ',';
        ComRef<IDirect3DVertexBuffer8> vb;
        UINT stride = 0;
        const HRESULT hr = device->GetStreamSource(stream, &vb.value, &stride);
        out << "{\"stream\":" << stream << ",\"hr\":" << static_cast<int32_t>(hr) << ",\"buffer\":";
        Id(out, vb.value);
        if (SUCCEEDED(hr)) out << ",\"stride\":" << stride;
        if (vb.value) {
            D3DVERTEXBUFFER_DESC d{};
            const HRESULT desc = vb.value->GetDesc(&d);
            if (SUCCEEDED(desc)) out << ",\"description\":{\"format\":" << static_cast<unsigned>(d.Format)
                << ",\"type\":" << static_cast<unsigned>(d.Type) << ",\"usage\":" << d.Usage
                << ",\"pool\":" << static_cast<unsigned>(d.Pool) << ",\"size\":" << d.Size
                << ",\"fvf\":" << d.FVF << '}';
            else out << ",\"description_hr\":" << static_cast<int32_t>(desc);
        }
        out << '}';
    }
    out << "],\"indices\":{";
    ComRef<IDirect3DIndexBuffer8> ib;
    UINT base = 0;
    const HRESULT hr = device->GetIndices(&ib.value, &base);
    out << "\"hr\":" << static_cast<int32_t>(hr) << ",\"buffer\":"; Id(out, ib.value);
    if (SUCCEEDED(hr)) out << ",\"base_vertex_index\":" << base;
    if (ib.value) {
        D3DINDEXBUFFER_DESC d{};
        const HRESULT desc = ib.value->GetDesc(&d);
        if (SUCCEEDED(desc)) out << ",\"description\":{\"format\":" << static_cast<unsigned>(d.Format)
            << ",\"type\":" << static_cast<unsigned>(d.Type) << ",\"usage\":" << d.Usage
            << ",\"pool\":" << static_cast<unsigned>(d.Pool) << ",\"size\":" << d.Size << '}';
        else out << ",\"description_hr\":" << static_cast<int32_t>(desc);
    }
    out << '}';
}

const char* CallName(DrawKind kind) {
    switch (kind) {
    case DrawKind::Primitive: return "DrawPrimitive";
    case DrawKind::IndexedPrimitive: return "DrawIndexedPrimitive";
    case DrawKind::PrimitiveUP: return "DrawPrimitiveUP";
    case DrawKind::IndexedPrimitiveUP: return "DrawIndexedPrimitiveUP";
    }
    return "unknown";
}

// SDK-defined observable D3D8 state identifiers. The debug-monitor token is a
// special command, not an ordinary render state, and is intentionally omitted.
// Getters that fail are omitted. Values remain raw DWORDs (some are float bits
// or packed colors), never silently converted into modern material parameters.
constexpr D3DRENDERSTATETYPE kRenderStates[] = {
    D3DRS_ZENABLE,
    D3DRS_FILLMODE,
    D3DRS_SHADEMODE,
    D3DRS_LINEPATTERN,
    D3DRS_ZWRITEENABLE,
    D3DRS_ALPHATESTENABLE,
    D3DRS_LASTPIXEL,
    D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,
    D3DRS_CULLMODE,
    D3DRS_ZFUNC,
    D3DRS_ALPHAREF,
    D3DRS_ALPHAFUNC,
    D3DRS_DITHERENABLE,
    D3DRS_ALPHABLENDENABLE,
    D3DRS_FOGENABLE,
    D3DRS_SPECULARENABLE,
    D3DRS_ZVISIBLE,
    D3DRS_FOGCOLOR,
    D3DRS_FOGTABLEMODE,
    D3DRS_FOGSTART,
    D3DRS_FOGEND,
    D3DRS_FOGDENSITY,
    D3DRS_EDGEANTIALIAS,
    D3DRS_ZBIAS,
    D3DRS_RANGEFOGENABLE,
    D3DRS_STENCILENABLE,
    D3DRS_STENCILFAIL,
    D3DRS_STENCILZFAIL,
    D3DRS_STENCILPASS,
    D3DRS_STENCILFUNC,
    D3DRS_STENCILREF,
    D3DRS_STENCILMASK,
    D3DRS_STENCILWRITEMASK,
    D3DRS_TEXTUREFACTOR,
    D3DRS_WRAP0,
    D3DRS_WRAP1,
    D3DRS_WRAP2,
    D3DRS_WRAP3,
    D3DRS_WRAP4,
    D3DRS_WRAP5,
    D3DRS_WRAP6,
    D3DRS_WRAP7,
    D3DRS_CLIPPING,
    D3DRS_LIGHTING,
    D3DRS_AMBIENT,
    D3DRS_FOGVERTEXMODE,
    D3DRS_COLORVERTEX,
    D3DRS_LOCALVIEWER,
    D3DRS_NORMALIZENORMALS,
    D3DRS_DIFFUSEMATERIALSOURCE,
    D3DRS_SPECULARMATERIALSOURCE,
    D3DRS_AMBIENTMATERIALSOURCE,
    D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_VERTEXBLEND,
    D3DRS_CLIPPLANEENABLE,
    D3DRS_SOFTWAREVERTEXPROCESSING,
    D3DRS_POINTSIZE,
    D3DRS_POINTSIZE_MIN,
    D3DRS_POINTSPRITEENABLE,
    D3DRS_POINTSCALEENABLE,
    D3DRS_POINTSCALE_A,
    D3DRS_POINTSCALE_B,
    D3DRS_POINTSCALE_C,
    D3DRS_MULTISAMPLEANTIALIAS,
    D3DRS_MULTISAMPLEMASK,
    D3DRS_PATCHEDGESTYLE,
    D3DRS_PATCHSEGMENTS,
    D3DRS_POINTSIZE_MAX,
    D3DRS_INDEXEDVERTEXBLENDENABLE,
    D3DRS_COLORWRITEENABLE,
    D3DRS_TWEENFACTOR,
    D3DRS_BLENDOP,
    D3DRS_POSITIONORDER,
    D3DRS_NORMALORDER,
};

constexpr D3DTEXTURESTAGESTATETYPE kStageStates[] = {
    D3DTSS_COLOROP,
    D3DTSS_COLORARG1,
    D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP,
    D3DTSS_ALPHAARG1,
    D3DTSS_ALPHAARG2,
    D3DTSS_BUMPENVMAT00,
    D3DTSS_BUMPENVMAT01,
    D3DTSS_BUMPENVMAT10,
    D3DTSS_BUMPENVMAT11,
    D3DTSS_TEXCOORDINDEX,
    D3DTSS_ADDRESSU,
    D3DTSS_ADDRESSV,
    D3DTSS_BORDERCOLOR,
    D3DTSS_MAGFILTER,
    D3DTSS_MINFILTER,
    D3DTSS_MIPFILTER,
    D3DTSS_MIPMAPLODBIAS,
    D3DTSS_MAXMIPLEVEL,
    D3DTSS_MAXANISOTROPY,
    D3DTSS_BUMPENVLSCALE,
    D3DTSS_BUMPENVLOFFSET,
    D3DTSS_TEXTURETRANSFORMFLAGS,
    D3DTSS_ADDRESSW,
    D3DTSS_COLORARG0,
    D3DTSS_ALPHAARG0,
    D3DTSS_RESULTARG,
};

// These copies are diagnostic snapshots, not a promise that the driver saw the
// same bytes: game threads may change the memory between our read and draw.
struct UpBytes {
    const char* status = "unreadable";
    const char* shadow_reason = nullptr;
    const char* shadow_abandon_cause = nullptr;
    const char* shadow_observation = nullptr;
    const buffers::BufferProvenance* provenance = nullptr;
    uint64_t byte_count = 0;
    bool byte_count_known = false;
    std::string hex;
};

bool Product(uint64_t a, uint64_t b, uint64_t& result) {
    if (a && b > std::numeric_limits<uint64_t>::max() / a) return false;
    result = a * b;
    return true;
}

bool PrimitiveElements(D3DPRIMITIVETYPE type, UINT primitives, uint64_t& elements) {
    if (!primitives) {
        switch (type) {
        case D3DPT_POINTLIST: case D3DPT_LINELIST: case D3DPT_LINESTRIP:
        case D3DPT_TRIANGLELIST: case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN:
            elements = 0; return true;
        default: return false;
        }
    }
    switch (type) {
    case D3DPT_POINTLIST: elements = primitives; return true;
    case D3DPT_LINELIST: elements = uint64_t(primitives) * 2; return true;
    case D3DPT_LINESTRIP: elements = uint64_t(primitives) + 1; return true;
    case D3DPT_TRIANGLELIST: elements = uint64_t(primitives) * 3; return true;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN:
        elements = uint64_t(primitives) + 2; return true;
    default: return false;
    }
}

// Inspect every page before ReadProcessMemory: reject guard, uncommitted and
// non-readable pages. A concurrent unmap/protection change remains possible;
// the kernel read is still checked for failure and short copies and no caller
// memory is dereferenced by this code.
bool ReadableRange(uintptr_t address, size_t length) {
    size_t remaining = length;
    while (remaining) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) return false;
        const DWORD protection = info.Protect;
        if (info.State != MEM_COMMIT || (protection & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        switch (protection & 0xff) {
        case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
        default: return false;
        }
        const uintptr_t start = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (info.RegionSize > std::numeric_limits<uintptr_t>::max() - start) return false;
        const uintptr_t end = start + info.RegionSize;
        if (address < start || address >= end) return false;
        const size_t step = std::min<size_t>(remaining, end - address);
        if (!step) return false;
        address += step;
        remaining -= step;
    }
    return true;
}

UpBytes ReadUp(CaptureState& s, const void* pointer, uint64_t offset, uint64_t count) {
    UpBytes result;
    result.byte_count = count;
    result.byte_count_known = true;
    if (!count) { result.status = "empty"; return result; }
    if (!pointer) { result.status = "null_pointer"; return result; }
    const auto base = reinterpret_cast<uintptr_t>(pointer);
    const auto max_address = std::numeric_limits<uintptr_t>::max();
    if (offset > max_address || base > max_address - static_cast<uintptr_t>(offset)) {
        result.status = "overflow"; return result;
    }
    const uintptr_t address = base + static_cast<uintptr_t>(offset);
    if (count > max_address - address) { result.status = "overflow"; return result; }
    if (count > kUpComponentLimit) { result.status = "per_draw_limit"; return result; }
    if (count > kUpFrameLimit - s.up_bytes) { result.status = "frame_limit"; return result; }
    const size_t size = static_cast<size_t>(count);
    if (!ReadableRange(address, size)) { result.status = "unreadable"; return result; }
    std::vector<BYTE> copy(size);
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                           copy.data(), size, &copied) || copied != size) {
        result.status = "unreadable"; return result;
    }
    constexpr char digits[] = "0123456789abcdef";
    result.hex.reserve(size * 2);
    for (BYTE value : copy) {
        result.hex.push_back(digits[value >> 4]);
        result.hex.push_back(digits[value & 0x0f]);
    }
    s.up_bytes += count;
    result.status = "captured";
    return result;
}

void BytesJson(std::ostream& out, const UpBytes& data) {
    out << "\"status\":\"" << data.status << "\",\"byte_count\":";
    if (data.byte_count_known) out << data.byte_count; else out << "null";
    out << ",\"encoding\":\"hex\",\"data\":";
    if (std::strcmp(data.status, "captured") == 0 || std::strcmp(data.status, "empty") == 0)
        out << '"' << data.hex << '"';
    else out << "null";
    if (data.shadow_reason) out << ",\"shadow_reason\":\"" << data.shadow_reason << '\"';
    if (data.shadow_abandon_cause) out << ",\"shadow_abandon_cause\":\"" << data.shadow_abandon_cause << '\"';
    if (data.shadow_observation) out << ",\"shadow_observation\":\"" << data.shadow_observation << '\"';
    if (data.provenance && data.provenance->lookup) {
        const auto& p = *data.provenance;
        out << ",\"shadow_provenance\":{\"lookup\":\"" << p.lookup << "\",\"reset_attempts\":"
            << p.reset_attempts << ",\"reset_successes\":" << p.reset_successes << ",\"reset_failures\":"
            << p.reset_failures << ",\"last_reset\":\"" << p.last_reset << "\",\"history_known\":"
            << (p.history_known ? "true" : "false");
        if (p.history_known) {
            out << ",\"resets_since_creation\":" << p.resets_since_creation
                << ",\"lock_attempts\":" << p.lock_attempts << ",\"successful_locks\":" << p.successful_locks
                << ",\"successful_write_unlocks\":" << p.successful_write_unlocks;
            if (p.last_lock_known) {
                out << ",\"last_lock\":{\"offset\":" << p.last_lock_offset << ",\"requested_bytes\":"
                    << p.last_lock_bytes << ",\"flags\":" << p.last_lock_flags << ",\"hr\":";
                if (p.last_lock_result_known) out << p.last_lock_hr; else out << "null";
                out << '}';
            }
        }
        out << '}';
    }
}

const char* ShadowReason(buffers::ShadowReadStatus status) noexcept {
    using buffers::ShadowReadStatus;
    switch (status) {
    case ShadowReadStatus::UnknownResource: return "unknown_resource";
    case ShadowReadStatus::UnknownData: return "unknown_data";
    case ShadowReadStatus::Abandoned: return "abandoned";
    case ShadowReadStatus::AbandonedRepeatedUnlock: return "abandoned";
    case ShadowReadStatus::BudgetExceeded: return "budget_exceeded";
    case ShadowReadStatus::TooLarge: return "too_large";
    case ShadowReadStatus::AllocationFailed: return "allocation_failed";
    case ShadowReadStatus::Busy: return "busy";
    case ShadowReadStatus::InvalidRange: return "invalid_range";
    case ShadowReadStatus::Unavailable: return "unavailable";
    case ShadowReadStatus::Other: return "other";
    case ShadowReadStatus::Captured: return nullptr;
    case ShadowReadStatus::CapturedPostRepeatedUnlock: return nullptr;
    }
    return "other";
}

// Read only from already-tracked CPU shadows. No additional D3D Lock occurs.
// These are pre-draw observations, not a guarantee about driver input: the
// paired result reports whether the intercepted draw window stayed stable.
template<typename Resource, typename Reader, typename DetailedReader>
UpBytes ReadShadow(CaptureState& s, const BufferObserver& observer, IDirect3DDevice8* device,
                   Resource* resource, Reader reader, DetailedReader detailed, uint64_t offset, uint64_t count) {
    UpBytes result;
    result.byte_count = count;
    result.byte_count_known = true;
    if (!observer.valid) { result.status = "window_unavailable"; return result; }
    if (!reader && !detailed) { result.status = "unknown"; return result; }
    if (!count) { result.status = "empty"; return result; }
    if (count > kBufferComponentLimit) { result.status = "per_draw_limit"; return result; }
    if (count > kBufferFrameLimit - s.buffer_bytes) { result.status = "frame_limit"; return result; }
    if (offset > std::numeric_limits<size_t>::max() || count > std::numeric_limits<size_t>::max()) {
        result.status = "overflow"; return result;
    }
    const size_t size = static_cast<size_t>(count);
    std::vector<BYTE> copy(size);
    bool read = false;
    if (detailed) {
        const auto observed = detailed(observer.context, device, resource, static_cast<size_t>(offset), copy.data(), size);
        read = observed == buffers::ShadowReadStatus::Captured ||
               observed == buffers::ShadowReadStatus::CapturedPostRepeatedUnlock;
        if (!read) result.shadow_reason = ShadowReason(observed);
        if (observed == buffers::ShadowReadStatus::AbandonedRepeatedUnlock)
            result.shadow_abandon_cause = "repeated_unlock";
        if (observed == buffers::ShadowReadStatus::CapturedPostRepeatedUnlock)
            result.shadow_observation = "post_repeated_unlock_write";
    } else read = reader(observer.context, device, resource, static_cast<size_t>(offset), copy.data(), size);
    if (!read) {
        result.status = "unknown"; return result;
    }
    constexpr char digits[] = "0123456789abcdef";
    result.hex.reserve(size * 2);
    for (BYTE value : copy) {
        result.hex.push_back(digits[value >> 4]);
        result.hex.push_back(digits[value & 0x0f]);
    }
    s.buffer_bytes += count;
    result.status = "captured";
    return result;
}

void CaptureBuffered(std::ostream& out, CaptureState& s, IDirect3DDevice8* device, const DrawCall& draw) {
    const BufferObserver& observer = *draw.buffer_observer;
    const bool indexed = draw.kind == DrawKind::IndexedPrimitive;
    ComRef<IDirect3DVertexBuffer8> owned_vb;
    ComRef<IDirect3DIndexBuffer8> owned_ib;
    IDirect3DVertexBuffer8* vb = nullptr;
    IDirect3DIndexBuffer8* ib = nullptr;
    UINT stride = 0, base = 0;
    HRESULT vertex_hr = E_FAIL, index_hr = E_FAIL;
    if (draw.buffer_bindings) {
        if (draw.buffer_bindings->device == device) {
            vertex_hr = draw.buffer_bindings->vertex_result;
            vb = draw.buffer_bindings->vertex;
            stride = draw.buffer_bindings->stride;
            if (indexed) {
                index_hr = draw.buffer_bindings->index_result;
                ib = draw.buffer_bindings->index;
                base = draw.buffer_bindings->base;
            }
        }
    } else {
        vertex_hr = device->GetStreamSource(0, &owned_vb.value, &stride);
        vb = owned_vb.value;
        if (indexed) {
            index_hr = device->GetIndices(&owned_ib.value, &base);
            ib = owned_ib.value;
        }
    }

    uint64_t elements = 0;
    const bool elements_known = PrimitiveElements(draw.primitive_type, draw.primitive_count, elements);
    uint64_t first = 0, vertex_count = 0, vertex_offset = 0, vertex_size = 0;
    const bool first_known = !indexed || (SUCCEEDED(index_hr) && ib);
    if (first_known) first = indexed ? uint64_t(base) + draw.min_vertex_index : draw.start_vertex;
    const bool vertex_count_known = indexed || elements_known;
    if (vertex_count_known) vertex_count = indexed ? (draw.primitive_count ? draw.num_vertices : 0) : elements;
    const bool stride_known = SUCCEEDED(vertex_hr) && vb;
    const bool offset_known = first_known && stride_known && Product(first, stride, vertex_offset);
    const bool vertex_size_known = vertex_count_known && stride_known && Product(vertex_count, stride, vertex_size);
    UpBytes vertex;
    vertex.provenance = &observer.vertex_provenance;
    if (draw.buffer_bindings && !observer.valid) vertex.status = "window_unavailable";
    else if (FAILED(vertex_hr)) vertex.status = "getter_failed";
    else if (!vb) vertex.status = "unbound";
    else if (!first_known || !vertex_count_known) vertex.status = "unknown_range";
    else if (!offset_known || !vertex_size_known || first > UINT64_C(0xffffffff) ||
             vertex_count > UINT64_C(0x100000000) - first) vertex.status = "overflow";
    else if (vertex_count && !stride) vertex.status = "invalid_stride";
    else {
        D3DVERTEXBUFFER_DESC desc{};
        if (FAILED(vb->GetDesc(&desc))) vertex.status = "descriptor_failed";
        else if (vertex_offset > desc.Size || vertex_size > uint64_t(desc.Size) - vertex_offset)
            vertex.status = "out_of_bounds";
        else vertex = ReadShadow(s, observer, device, vb, observer.read_vertex, observer.read_vertex_detailed, vertex_offset, vertex_size);
    }
    vertex.provenance = &observer.vertex_provenance;

    uint64_t index_offset = 0, index_size = 0;
    bool index_offset_known = false;
    unsigned index_format = 0;
    bool index_format_known = false;
    UpBytes index;
    if (indexed) {
        if (draw.buffer_bindings && !observer.valid) index.status = "window_unavailable";
        else if (FAILED(index_hr)) index.status = "getter_failed";
        else if (!ib) index.status = "unbound";
        else {
            D3DINDEXBUFFER_DESC desc{};
            if (FAILED(ib->GetDesc(&desc))) index.status = "descriptor_failed";
            else {
                index_format = static_cast<unsigned>(desc.Format);
                index_format_known = true;
                const uint64_t width = desc.Format == D3DFMT_INDEX16 ? 2 : desc.Format == D3DFMT_INDEX32 ? 4 : 0;
                if (!width) index.status = "unsupported_index_format";
                else if (!elements_known) index.status = "unsupported_topology";
                else if (!Product(draw.start_index, width, index_offset) || !Product(elements, width, index_size))
                    index.status = "overflow";
                else {
                    index_offset_known = true;
                    if (index_offset > desc.Size || index_size > uint64_t(desc.Size) - index_offset)
                        index.status = "out_of_bounds";
                    else index = ReadShadow(s, observer, device, ib, observer.read_index, observer.read_index_detailed, index_offset, index_size);
                }
            }
        }
    }
    index.provenance = &observer.index_provenance;

    out << ",\"buffer_data\":{\"source\":\"application_lock_shadow\"";
    if (observer.repeated_unlock_policy)
        out << ",\"repeated_unlocks_completed\":" << observer.repeated_unlocks;
    if (!observer.valid && observer.failure_status && observer.failure_cause) {
        out << ",\"window_failure\":{\"status\":\"" << observer.failure_status
            << "\",\"cause\":\"" << observer.failure_cause << "\",\"source_line\":";
        if (observer.failure_line) out << observer.failure_line; else out << "null";
        const auto& unlock = observer.failure_unlock;
        if (unlock.present) {
            out << ",\"unlock\":{\"resource\":\"" << (unlock.vertex ? "vertex" : "index")
                << "\",\"pending_lock\":" << (unlock.pending_lock ? "true" : "false")
                << ",\"locked\":" << (unlock.locked ? "true" : "false")
                << ",\"pending_unlock\":" << (unlock.pending_unlock ? "true" : "false")
                << ",\"operation_exhausted\":" << (unlock.operation_exhausted ? "true" : "false")
                << ",\"last_lock_hr\":";
            if (unlock.last_lock_known) out << unlock.last_lock_hr; else out << "null";
            out << ",\"unlock_hr_candidate\":";
            if (unlock.unlock_result_known) out << unlock.unlock_hr_candidate; else out << "null";
            out << '}';
        }
        const auto& lock = observer.failure_lock;
        if (lock.present) {
            out << ",\"lock\":{\"resource\":\"" << (lock.vertex ? "vertex" : "index")
                << "\",\"registry_result\":\"" << lock.registry_result << "\",\"recovery_attempted\":"
                << (lock.recovery_attempted ? "true" : "false") << ",\"offset\":" << lock.offset
                << ",\"requested_bytes\":" << lock.requested_bytes << ",\"normalized_bytes\":" << lock.normalized_bytes
                << ",\"buffer_size\":" << lock.buffer_size << ",\"flags\":" << lock.flags << '}';
        }
        out << '}';
    }
    out << ",\"vertex\":{";
    BytesJson(out, vertex);
    out << ",\"resource\":"; Id(out, vb);
    out << ",\"stream\":0,\"first_vertex\":";
    if (first_known) out << first; else out << "null";
    out << ",\"vertex_count\":";
    if (vertex_count_known) out << vertex_count; else out << "null";
    out << ",\"stride\":";
    if (stride_known) out << stride; else out << "null";
    out << ",\"byte_offset\":";
    if (offset_known) out << vertex_offset; else out << "null";
    out << "},\"index\":";
    if (!indexed) out << "null";
    else {
        out << '{'; BytesJson(out, index);
        out << ",\"resource\":"; Id(out, ib);
        out << ",\"index_count\":";
        if (elements_known) out << elements; else out << "null";
        out << ",\"index_format\":";
        if (index_format_known) out << index_format; else out << "null";
        out << ",\"byte_offset\":";
        if (index_offset_known) out << index_offset; else out << "null";
        out << '}';
    }
    out << '}';
}

void CaptureUp(CaptureState& s, const DrawCall& draw, UpBytes& vertex, UpBytes& index,
               uint64_t& vertex_count, uint64_t& vertex_offset, uint64_t& index_count,
               bool& vertex_count_known, bool& offset_known, bool& index_count_known) {
    uint64_t elements = 0;
    const bool known = PrimitiveElements(draw.primitive_type, draw.primitive_count, elements);
    const bool indexed = draw.kind == DrawKind::IndexedPrimitiveUP;
    vertex_count_known = indexed || known;
    vertex_count = indexed ? (draw.primitive_count ? draw.num_vertices : 0) : elements;
    const uint64_t first = indexed ? draw.min_vertex_index : 0;
    offset_known = Product(first, draw.vertex_stride, vertex_offset);
    if (!vertex_count_known) vertex.status = "unsupported_topology";
    else if (!offset_known || (indexed && first + vertex_count > UINT64_C(0x100000000)))
        vertex.status = "overflow";
    else if (vertex_count && !draw.vertex_stride) vertex.status = "invalid_stride";
    else {
        uint64_t size = 0;
        if (!Product(vertex_count, draw.vertex_stride, size)) vertex.status = "overflow";
        else vertex = ReadUp(s, draw.vertex_data, vertex_offset, size);
    }
    if (!indexed) return;
    index_count_known = known;
    index_count = elements;
    if (!known) { index.status = "unsupported_topology"; return; }
    uint64_t width = 0;
    if (draw.index_format == D3DFMT_INDEX16) width = 2;
    else if (draw.index_format == D3DFMT_INDEX32) width = 4;
    else { index.status = "unsupported_index_format"; return; }
    uint64_t size = 0;
    if (!Product(index_count, width, size)) index.status = "overflow";
    else index = ReadUp(s, draw.index_data, 0, size);
}

const char* DeclarationScanStatus(ir::DeclarationStatus value) noexcept {
    switch (value) {
    case ir::DeclarationStatus::Complete: return "complete";
    case ir::DeclarationStatus::InvalidConfiguration: return "invalid_configuration";
    case ir::DeclarationStatus::NullAddress: return "null_address";
    case ir::DeclarationStatus::AddressOverflow: return "address_overflow";
    case ir::DeclarationStatus::Unreadable: return "unreadable";
    case ir::DeclarationStatus::WordLimit: return "word_limit";
    case ir::DeclarationStatus::UnsupportedToken: return "unsupported_token";
    case ir::DeclarationStatus::AllocationFailure: return "allocation_failure";
    }
    return "unknown";
}

const char* DeclarationUnavailableReason(ir::ShaderUnavailableReason value) noexcept {
    switch (value) {
    case ir::ShaderUnavailableReason::ScanIncomplete: return "scan_incomplete";
    case ir::ShaderUnavailableReason::InvalidSnapshot: return "invalid_snapshot";
    case ir::ShaderUnavailableReason::HandleCollision: return "handle_collision";
    case ir::ShaderUnavailableReason::ResetAttempt: return "reset_attempt";
    case ir::ShaderUnavailableReason::DeleteFailed: return "delete_failed";
    case ir::ShaderUnavailableReason::ObservationError: return "observation_error";
    case ir::ShaderUnavailableReason::None: case ir::ShaderUnavailableReason::Unknown: return "unknown";
    }
    return "unknown";
}

const char* ProgramUnavailableReason(ir::ShaderProgramStatus value) noexcept {
    switch (value) {
    case ir::ShaderProgramStatus::NullAddress: return "null_address";
    case ir::ShaderProgramStatus::InvalidConfiguration: return "invalid_configuration";
    case ir::ShaderProgramStatus::AddressOverflow: return "address_overflow";
    case ir::ShaderProgramStatus::Unreadable: return "unreadable";
    case ir::ShaderProgramStatus::UnsupportedVersion: return "unsupported_version";
    case ir::ShaderProgramStatus::UnsupportedInstruction: return "unsupported_instruction";
    case ir::ShaderProgramStatus::InvalidParameter: return "invalid_parameter";
    case ir::ShaderProgramStatus::WordLimit: return "word_limit";
    case ir::ShaderProgramStatus::AllocationFailure: return "allocation_failure";
    case ir::ShaderProgramStatus::NotRequested: case ir::ShaderProgramStatus::Complete: return "unknown";
    }
    return "unknown";
}

// Validate framing without allocating inside the capture mutex. CONST/EXT
// payload DWORDs are opaque and can themselves equal the END token.
bool FramedHistory(const std::vector<std::uint32_t>& words) noexcept {
    if (words.empty() || words.size() > kHistoryObservationWords) return false;
    size_t position = 0;
    while (position < words.size()) {
        const auto token = words[position++];
        if (token == 0xffffffffu) return position == words.size();
        const unsigned type = token >> 29;
        size_t payload = 0;
        switch (type) {
        case 0: if (token != 0) return false; break;
        case 1: case 2: case 3: break;
        case 4: payload = ((token >> 25) & 0x0fu) * 4u; break;
        case 5: payload = (token >> 24) & 0x1fu; break;
        default: return false;
        }
        if (payload > words.size() - position) return false;
        position += payload;
    }
    return false;
}

bool Draw(CaptureState& s, IDirect3DDevice8* device, const DrawCall& draw, uint64_t operation) {
    const bool up = draw.kind == DrawKind::PrimitiveUP || draw.kind == DrawKind::IndexedPrimitiveUP;
    UpBytes vertex, index;
    uint64_t vertex_count = 0, vertex_offset = 0, index_count = 0;
    bool vertex_count_known = false, offset_known = false, index_count_known = false;
    if (up) CaptureUp(s, draw, vertex, index, vertex_count, vertex_offset, index_count,
                      vertex_count_known, offset_known, index_count_known);
    auto out = Json();
    out << "{\"event\":\"draw\",\"frame\":" << s.frame << ",\"op_id\":" << operation;
    if (s.bridge_requested) out << ",\"thread_id\":" << GetCurrentThreadId();
    if (s.submission_requested && submission_stack.depth && submission_stack.depth <= kSubmissionDepth) {
        const auto& scope = submission_stack.scopes[submission_stack.depth - 1];
        if (scope.ticket && scope.frame == s.frame) out << ",\"submission_ticket\":" << scope.ticket;
    }
    out << ",\"call\":\"" << CallName(draw.kind)
        << "\",\"primitive_type\":" << static_cast<unsigned>(draw.primitive_type)
        << ",\"start_vertex\":" << draw.start_vertex << ",\"min_vertex_index\":" << draw.min_vertex_index
        << ",\"num_vertices\":" << draw.num_vertices << ",\"start_index\":" << draw.start_index
        << ",\"primitive_count\":" << draw.primitive_count << ",\"vertex_stride\":" << draw.vertex_stride
        << ",\"index_format\":" << static_cast<unsigned>(draw.index_format) << ",\"viewport\":";
    D3DVIEWPORT8 viewport{};
    if (SUCCEEDED(device->GetViewport(&viewport))) {
        out << "{\"x\":" << viewport.X << ",\"y\":" << viewport.Y
            << ",\"width\":" << viewport.Width << ",\"height\":" << viewport.Height << ",\"min_z\":";
        Number(out, viewport.MinZ); out << ",\"max_z\":"; Number(out, viewport.MaxZ); out << '}';
    } else out << "null";
    out << ",\"transforms\":{\"view\":"; Matrix(out, device, D3DTS_VIEW);
    out << ",\"projection\":"; Matrix(out, device, D3DTS_PROJECTION);
    out << ",\"world\":[";
    for (unsigned i = 0; i < kWorldLimit; ++i) {
        if (i) out << ',';
        Matrix(out, device, static_cast<D3DTRANSFORMSTATETYPE>(256 + i));
    }
    out << "],\"texture\":[";
    for (unsigned i = 0; i < kStageLimit; ++i) {
        if (i) out << ',';
        Matrix(out, device, static_cast<D3DTRANSFORMSTATETYPE>(16 + i));
    }
    out << "]},\"render_states\":{";
    bool comma = false;
    for (D3DRENDERSTATETYPE name : kRenderStates) {
        DWORD value = 0;
        if (FAILED(device->GetRenderState(name, &value))) continue;
        if (comma) out << ',';
        comma = true;
        out << '"' << static_cast<unsigned>(name) << "\":" << value;
    }
    out << "},\"material\":";
    D3DMATERIAL8 material{};
    if (SUCCEEDED(device->GetMaterial(&material))) {
        out << "{\"diffuse\":"; Color(out, material.Diffuse);
        out << ",\"ambient\":"; Color(out, material.Ambient);
        out << ",\"specular\":"; Color(out, material.Specular);
        out << ",\"emissive\":"; Color(out, material.Emissive);
        out << ",\"power\":"; Number(out, material.Power); out << '}';
    } else out << "null";
    out << ",\"lights\":"; Lights(out, device);
    out << ",\"stages\":[";
    const unsigned stages = s.has_caps ? std::min<unsigned>(kStageLimit, std::max<unsigned>(
        s.caps.MaxTextureBlendStages, s.caps.MaxSimultaneousTextures)) : kStageLimit;
    for (unsigned stage = 0; stage < stages; ++stage) {
        if (stage) out << ',';
        out << "{\"stage\":" << stage << ",\"states\":{";
        comma = false;
        for (D3DTEXTURESTAGESTATETYPE name : kStageStates) {
            DWORD value = 0;
            if (FAILED(device->GetTextureStageState(stage, name, &value))) continue;
            if (comma) out << ',';
            comma = true;
            out << '"' << static_cast<unsigned>(name) << "\":" << value;
        }
        out << "},\"texture\":";
        ComRef<IDirect3DBaseTexture8> texture;
        const HRESULT hr = device->GetTexture(stage, &texture.value);
        Texture(out, texture.value);
        if (FAILED(hr)) out << ",\"texture_hr\":" << static_cast<int32_t>(hr);
        out << '}';
    }
    DWORD vs = 0, ps = 0;
    HRESULT vs_hr = E_FAIL;
    if (draw.shader_snapshot) {
        vs = draw.shader_snapshot->value;
        vs_hr = draw.shader_snapshot->getter_result;
    } else {
        vs_hr = device->GetVertexShader(&vs);
    }
    const HRESULT ps_hr = device->GetPixelShader(&ps);
    out << "],\"vertex_shader\":";
    uint64_t history_words_emitted = 0;
    uint64_t program_words_emitted = 0;
    uint64_t constant_words_emitted = 0;
    if (SUCCEEDED(vs_hr)) out << vs; else out << "null";
    if (draw.shader_snapshot && s.declaration_requested) {
        const auto& declaration = draw.shader_snapshot->declaration;
        const char* status = "unknown";
        switch (declaration.status) {
        case ir::ShaderObservationStatus::Candidate: status = "candidate"; break;
        case ir::ShaderObservationStatus::Unavailable: status = "unavailable"; break;
        case ir::ShaderObservationStatus::Unsafe: status = "unsafe"; break;
        case ir::ShaderObservationStatus::Unknown: break;
        }
        out << ",\"vertex_declaration\":{\"status\":\"" << status << "\",\"generation\":";
        if (declaration.generation) out << declaration.generation; else out << "null";
        out << ",\"words\":";
        if (declaration.status == ir::ShaderObservationStatus::Candidate) {
            out << '[';
            for (size_t i = 0; i < declaration.words.size(); ++i) {
                if (i) out << ',';
                out << declaration.words[i];
            }
            out << ']';
        } else out << "null";
        if (declaration.status == ir::ShaderObservationStatus::Unavailable && declaration.generation &&
            declaration.has_scan_status) {
            out << ",\"reason\":\"" << DeclarationUnavailableReason(declaration.reason)
                << "\",\"scan_status\":\"" << DeclarationScanStatus(declaration.scan_status) << '\"';
        }
        out << '}';
        const auto program_count = declaration.program_words.size();
        if (s.vertex_programs_requested && s.declaration_requested && SUCCEEDED(vs_hr) &&
            declaration.status == ir::ShaderObservationStatus::Candidate && declaration.generation &&
            declaration.program_status != ir::ShaderProgramStatus::NotRequested) {
            const bool framed = declaration.program_status == ir::ShaderProgramStatus::Complete &&
                program_count >= 2 && program_count <= kProgramObservationWords &&
                ir::IsVertexShader1x(declaration.program_words.front()) && declaration.program_words.back() == 0x0000ffffu;
            const bool budget = framed && s.vertex_program_words <= kProgramFrameWords &&
                program_count <= kProgramFrameWords - s.vertex_program_words;
            if (budget) {
                out << ",\"vertex_program\":{\"status\":\"candidate\",\"version\":\""
                    << (declaration.program_words.front() == 0xfffe0100u ? "vs_1_0" : "vs_1_1") << "\",\"generation\":"
                    << declaration.generation << ",\"words\":[";
                for (size_t i = 0; i < program_count; ++i) {
                    if (i) out << ',';
                    out << declaration.program_words[i];
                }
                out << "]}";
                program_words_emitted = program_count;
            } else {
                const char* reason = declaration.program_status != ir::ShaderProgramStatus::Complete
                    ? ProgramUnavailableReason(declaration.program_status) : framed ? "frame_word_limit" : "invalid_snapshot";
                out << ",\"vertex_program\":{\"status\":\"unavailable\",\"generation\":"
                    << declaration.generation << ",\"reason\":\"" << reason << "\",\"words\":null";
                if (declaration.program_status == ir::ShaderProgramStatus::UnsupportedVersion &&
                    declaration.program_first_word_known)
                    out << ",\"first_word_observed\":" << declaration.program_first_word;
                out << '}';
            }
        }
        const auto count = declaration.pre_reset_words.size();
        if (s.history_requested && s.declaration_requested && SUCCEEDED(vs_hr) &&
            declaration.status == ir::ShaderObservationStatus::Unavailable &&
            declaration.reason == ir::ShaderUnavailableReason::ResetAttempt && declaration.has_scan_status &&
            declaration.scan_status == ir::DeclarationStatus::Complete && declaration.generation &&
            declaration.reset_epoch && declaration.rebind_epoch > declaration.reset_epoch &&
            !(declaration.reset_hr & 0x80000000u) && FramedHistory(declaration.pre_reset_words) &&
            s.history_words <= kHistoryFrameWords && count <= kHistoryFrameWords - s.history_words) {
            out << ",\"vertex_declaration_pre_reset\":{\"status\":\"hypothesis\","
                << "\"source\":\"application_memory_before_reset\",\"generation\":" << declaration.generation
                << ",\"reset_epoch\":" << declaration.reset_epoch << ",\"reset_hr\":" << declaration.reset_hr
                << ",\"rebind_epoch\":" << declaration.rebind_epoch << ",\"words\":[";
            for (size_t i = 0; i < count; ++i) {
                if (i) out << ',';
                out << declaration.pre_reset_words[i];
            }
            out << "]}";
            history_words_emitted = count;
        }
        const auto prior_program_count = declaration.pre_reset_program_words.size();
        if (history_words_emitted && s.vertex_programs_requested &&
            prior_program_count >= 2 && prior_program_count <= kProgramObservationWords &&
            ir::IsVertexShader1x(declaration.pre_reset_program_words.front()) &&
            declaration.pre_reset_program_words.back() == 0x0000ffffu &&
            s.vertex_program_words <= kProgramFrameWords && prior_program_count <= kProgramFrameWords - s.vertex_program_words) {
            out << ",\"vertex_program_pre_reset\":{\"status\":\"hypothesis\","
                << "\"source\":\"application_memory_before_reset\",\"generation\":" << declaration.generation
                << ",\"reset_epoch\":" << declaration.reset_epoch << ",\"reset_hr\":" << declaration.reset_hr
                << ",\"rebind_epoch\":" << declaration.rebind_epoch << ",\"words\":[";
            for (size_t i = 0; i < prior_program_count; ++i) {
                if (i) out << ',';
                out << declaration.pre_reset_program_words[i];
            }
            out << "]}";
            program_words_emitted = prior_program_count;
        }
    }
    if (s.vertex_constants_requested && s.declaration_requested && draw.shader_snapshot &&
        draw.shader_snapshot->constants_requested) {
        const auto& sample = *draw.shader_snapshot;
        const uint64_t count = sample.constants_count;
        const uint64_t words = count * 4;
        const char* reason = nullptr;
        if (!count) reason = "caps_unavailable";
        else if (count > kVertexConstantRegisters) reason = "invalid_count";
        else if (FAILED(sample.constants_result)) reason = "getter_failed";
        else if (FAILED(sample.constants_verify_result)) reason = "verification_failed";
        else if (!sample.constants_match) reason = "inconsistent_read";
        else if (s.vertex_constant_words > kVertexConstantFrameWords ||
                 words > kVertexConstantFrameWords - s.vertex_constant_words) reason = "frame_word_limit";
        if (reason) {
            out << ",\"vertex_constants\":{\"status\":\"unavailable\",\"reason\":\"" << reason
                << "\",\"count\":" << count << ",\"getter_hr\":" << static_cast<int32_t>(sample.constants_result)
                << ",\"verify_hr\":" << static_cast<int32_t>(sample.constants_verify_result)
                << ",\"bits\":null}";
        } else {
            out << ",\"vertex_constants\":{\"status\":\"sampled\",\"start_register\":0,\"count\":" << count
                << ",\"getter_hr\":" << static_cast<int32_t>(sample.constants_result)
                << ",\"verify_hr\":" << static_cast<int32_t>(sample.constants_verify_result) << ",\"bits\":[";
            for (uint64_t i = 0; i < words; ++i) {
                if (i) out << ',';
                out << sample.constant_bits[static_cast<std::size_t>(i)];
            }
            out << "]}";
            constant_words_emitted = words;
        }
    }
    out << ",\"pixel_shader\":";
    if (SUCCEEDED(ps_hr)) out << ps; else out << "null";
    out << ',';
    const unsigned streams = s.has_caps ? std::min<unsigned>(s.caps.MaxStreams, kStreamLimit) : kStreamLimit;
    BufferBindings(out, device, streams);
    if (!up && draw.buffer_observer) CaptureBuffered(out, s, device, draw);
    if (up) {
        out << ",\"up_data\":{\"vertex\":{";
        BytesJson(out, vertex);
        out << ",\"first_vertex\":" << (draw.kind == DrawKind::IndexedPrimitiveUP ? draw.min_vertex_index : 0)
            << ",\"vertex_count\":";
        if (vertex_count_known) out << vertex_count; else out << "null";
        out << ",\"stride\":" << draw.vertex_stride << ",\"byte_offset\":";
        if (offset_known) out << vertex_offset; else out << "null";
        out << "},\"index\":";
        if (draw.kind != DrawKind::IndexedPrimitiveUP) out << "null";
        else {
            out << '{'; BytesJson(out, index);
            out << ",\"index_count\":";
            if (index_count_known) out << index_count; else out << "null";
            out << ",\"index_format\":" << static_cast<unsigned>(draw.index_format) << '}';
        }
        out << '}';
    }
    out << "}\n";
    if (!Write(s, out.str())) return false;
    s.history_words += history_words_emitted;
    s.vertex_program_words += program_words_emitted;
    s.vertex_constant_words += constant_words_emitted;
    ++s.draws;
    return true;
}

// Prepare a bounded copy of rectangles before calling the driver. The input
// pointer is never dereferenced: a failed or partial kernel read stays unknown.
const char* ClearRectangles(DWORD count, const D3DRECT* source, std::vector<D3DRECT>& rectangles) {
    if (!count) return source ? "inconsistent" : "none";
    if (!source) return "null_pointer";
    if (count > kClearRectLimit) return "count_limit";
    const auto address = reinterpret_cast<uintptr_t>(source);
    const size_t bytes = static_cast<size_t>(count) * sizeof(D3DRECT);
    if (bytes > std::numeric_limits<uintptr_t>::max() - address) return "address_overflow";
    if (!ReadableRange(address, bytes)) return "unreadable";
    rectangles.resize(count);
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), source, rectangles.data(), bytes, &copied) || copied != bytes) {
        rectangles.clear();
        return "unreadable";
    }
    return "captured";
}

// Tokens are unique within the capture and the pending table pairs each
// attempt with its result. Overflow or excessive overlap makes the trace partial.
uint64_t BeginOperation(CaptureState& s, OperationKind kind) {
    if (!s.enabled || !s.begun || s.pending_operations.size() >= kPendingOperationLimit ||
        s.next_operation == std::numeric_limits<uint64_t>::max()) {
        Stop(s);
        return 0;
    }
    if (s.bridge_requested && !s.bridge_pending.empty()) s.bridge_operation_overlap = true;
    if (s.bridge_requested && s.bridge_present_ticket) s.bridge_present_overlap = true;
    if (!s.pending_operations.empty()) s.concurrent_operations = true;
    const uint64_t token = s.next_operation++;
    s.pending_operations.emplace(token, kind);
    return token;
}

void Fail() noexcept;

void FinishOperation(IDirect3DDevice8* device, uint64_t token, HRESULT result, OperationKind kind,
                     int buffer_stable = -1, int shader_stable = -1) {
    if (!token) return;
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device || !s.begun ||
            !s.triggered || s.frame != s.target) return;
        const auto pending = s.pending_operations.find(token);
        if (pending == s.pending_operations.end() || pending->second != kind) {
            Stop(s);
            return;
        }
        auto out = Json();
        out << "{\"event\":\"" << (kind == OperationKind::Draw ? "draw_result" : "clear_result")
            << "\",\"frame\":" << s.frame << ",\"op_id\":" << token
            << ",\"hr\":" << static_cast<int32_t>(result);
        if (kind == OperationKind::Draw && buffer_stable >= 0)
            out << ",\"buffer_stable\":" << (buffer_stable ? "true" : "false");
        if (kind == OperationKind::Draw && shader_stable >= 0)
            out << ",\"shader_stable\":" << (shader_stable ? "true" : "false");
        out << "}\n";
        if (Write(s, out.str())) s.pending_operations.erase(pending);
    } catch (...) { Fail(); }
}

// Captures keyboard state from the calling thread's input queue, without
// consuming a message or changing the game's input. A processed F11 key-down
// arms the *next* frame; releasing F11 rearms the edge detector.
void CheckKey(CaptureState& s) {
    BYTE keys[256]{};
    if (!GetKeyboardState(keys)) return;
    const bool down = (keys[VK_F11] & 0x80) != 0;
    if (!s.triggered && down && !s.key_down &&
        s.frame < std::numeric_limits<uint64_t>::max() - s.frames_requested) {
        s.target = s.frame + 1;
        s.triggered = true;
    }
    s.key_down = down;
}

// An external marker arms the next frame without relying on the game's input
// queue. It is observed only; the proxy does not create or delete the marker.
void CheckTriggerFile(CaptureState& s) {
    if (s.triggered) return;
    const DWORD attributes = GetFileAttributesW(s.trigger_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) Stop(s);
        return;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) { Stop(s); return; }
    if (s.frame < std::numeric_limits<uint64_t>::max() - s.frames_requested) {
        s.target = s.frame + 1;
        s.triggered = true;
    }
}

void Fail() noexcept {
    try {
        auto& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        Stop(state);
    } catch (...) {}
}

}  // namespace

bool CaptureConfigured() {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        Initialize(s);
        return s.enabled;
    } catch (...) { Fail(); return false; }
}

void CaptureBridgeInstallResult(scene_bridge::ProbeStatus status) noexcept {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        Initialize(s);
        if (s.enabled && s.bridge_requested && !s.begun && s.file == INVALID_HANDLE_VALUE) {
            s.bridge_install_known = true;
            s.bridge_install_status = status;
        }
    } catch (...) { Fail(); }
}

void CaptureSubmissionInstallResult(scene_bridge::ProbeStatus status) noexcept {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        Initialize(s);
        if (s.enabled && s.bridge_requested && !s.begun && s.file == INVALID_HANDLE_VALUE) {
            s.submission_requested = true;
            s.submission_install_status = status;
        }
    } catch (...) { Fail(); }
}

void CaptureSubmissionEnter(std::uint32_t element, std::uint32_t stage, std::uint32_t section) noexcept {
    if (submission_stack.depth == std::numeric_limits<unsigned>::max()) {
        CaptureBridgeMarkReentered(); return;
    }
    const auto depth = submission_stack.depth++;
    if (depth >= kSubmissionDepth) { CaptureBridgeMarkReentered(); return; }
    auto& scope = submission_stack.scopes[depth];
    scope = {};
    if (bridge_query_depth) { bridge_reentered_query = true; return; }
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !s.submission_requested ||
            s.submission_install_status != scene_bridge::ProbeStatus::Installed ||
            !s.device || !s.triggered || s.frame != s.target) return;
        const auto thread = CurrentBridgeThread();
        if (!thread.id || !thread.birth || s.bridge_present_ticket || s.bridge_reset_ticket ||
            s.submission_started >= kSubmissionLimit || !s.submission_next_ticket ||
            s.submission_next_ticket == std::numeric_limits<uint64_t>::max()) {
            ++s.submission_dropped; return;
        }
        const scene_bridge::ProbeReader reader{nullptr, SubmissionRead};
        const auto sample = scene_bridge::SampleSubmission(reader, element);
        if (!sample.valid) return;
        Begin(s);
        if (!s.enabled) return;
        const auto ticket = s.submission_next_ticket++;
        auto out = Json();
        out << "{\"event\":\"scene_bridge_submission_begin\",\"frame\":" << s.frame
            << ",\"ticket\":" << ticket << ",\"thread_id\":" << thread.id
            << ",\"stage\":" << stage << ",\"section\":" << section
            << ",\"self\":" << sample.self << ",\"pass\":" << sample.pass
            << ",\"element\":" << sample.element << ",\"vtable\":" << sample.vtable
            << ",\"type\":" << sample.type << ",\"matrix_bits\":";
        BridgeBits(out, sample.matrix, 16, true);
        out << ",\"owner\":" << sample.owner << ",\"owner_index\":" << sample.owner_index
            << ",\"owner_fname\":" << sample.owner_fname << ",\"backlink\":" << sample.backlink
            << ",\"mesh_node\":" << sample.mesh_node << ",\"asset_proxy\":" << sample.asset_proxy
            << ",\"asset_resource\":" << sample.asset_resource << ",\"asset_name\":";
        BridgeText(out, sample.asset_name, sizeof(sample.asset_name), sample.asset_available);
        out << ",\"bsp_source\":" << sample.bsp_source
            << ",\"bsp_blocks\":" << sample.bsp_blocks << ",\"bsp_index\":" << sample.bsp_index << "}\n";
        if (!Write(s, out.str())) return;
        scope.ticket = ticket; scope.frame = s.frame; scope.thread = thread.id; scope.sample = sample;
        ++s.submission_started; ++s.submission_pending;
    } catch (...) { Fail(); }
}

void CaptureSubmissionLeave() noexcept {
    if (!submission_stack.depth) { CaptureBridgeMarkReentered(); return; }
    const auto depth = --submission_stack.depth;
    if (depth >= kSubmissionDepth) return;
    auto scope = submission_stack.scopes[depth];
    submission_stack.scopes[depth] = {};
    if (!scope.ticket) return;
    if (bridge_query_depth) { bridge_reentered_query = true; return; }
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || scope.frame != s.frame || !s.submission_pending) return;
        const scene_bridge::ProbeReader reader{nullptr, SubmissionRead};
        const auto after = scene_bridge::SampleSubmission(reader, scope.sample.element);
        const auto thread = CurrentBridgeThread();
        const bool stable = thread.id == scope.thread && thread.birth &&
                            scene_bridge::SameSubmission(scope.sample, after);
        auto out = Json();
        out << "{\"event\":\"scene_bridge_submission_end\",\"frame\":" << s.frame
            << ",\"ticket\":" << scope.ticket << ",\"thread_id\":" << thread.id
            << ",\"stable\":" << (stable ? "true" : "false") << "}\n";
        if (!Write(s, out.str())) return;
        --s.submission_pending; ++s.submission_completed;
    } catch (...) { Fail(); }
}

bool CaptureBridgeShouldDisable() noexcept {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.bridge_requested && s.bridge_install_known &&
            s.bridge_install_status == scene_bridge::ProbeStatus::Installed && !s.enabled;
    } catch (...) { return false; }
}

void CaptureBridgeMarkReentered() noexcept {
    auto value = bridge_reentry_count.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint32_t>::max() &&
           !bridge_reentry_count.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {}
}

std::uint64_t CaptureBridgeBegin(std::uint32_t thread_id) noexcept {
    if (bridge_query_depth) { bridge_reentered_query = true; return 0; }
    try {
        const auto thread = CurrentBridgeThread();
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !s.bridge_requested || !s.bridge_install_known ||
            s.bridge_install_status != scene_bridge::ProbeStatus::Installed || !s.device || !s.triggered ||
            s.frame != s.target) return 0;
        if (!thread_id || thread_id != thread.id || !thread.birth || s.bridge_present_ticket ||
            s.bridge_started >= kBridgeObservationLimit ||
            !s.bridge_next_ticket || s.bridge_next_ticket == std::numeric_limits<uint64_t>::max()) {
            ++s.bridge_dropped;
            if (s.bridge_present_ticket) s.bridge_present_overlap = true;
            return 0;
        }
        Begin(s);
        if (!s.enabled) return 0;
        if (!s.bridge_pending.empty()) s.bridge_probe_overlap = true;
        if (!s.pending_operations.empty()) s.bridge_operation_overlap = true;
        if (s.bridge_reset_ticket) s.bridge_operation_overlap = true;
        const auto ticket = s.bridge_next_ticket++;
        s.bridge_pending.emplace(ticket, BridgeObservation{s.frame, s.next_operation, thread_id, thread.birth});
        ++s.bridge_started;
        return ticket;
    } catch (...) { Fail(); return 0; }
}

void CaptureBridgeEnd(std::uint64_t ticket, const scene_bridge::ProbeSnapshot& snapshot) noexcept {
    if (!ticket) return;
    try {
        const auto thread = CurrentBridgeThread();
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto item = s.bridge_pending.find(ticket);
        if (!s.enabled || s.complete || item == s.bridge_pending.end() || item->second.frame != s.frame) return;
        const BridgeObservation observation = item->second;
        s.bridge_pending.erase(item);
        if (snapshot.thread_id != observation.thread_id || thread.id != observation.thread_id ||
            thread.birth != observation.thread_birth) s.bridge_probe_multithread = true;
        if (!s.bridge_probe_thread) {
            s.bridge_probe_thread = observation.thread_id; s.bridge_probe_birth = observation.thread_birth;
        } else if (s.bridge_probe_thread != observation.thread_id || s.bridge_probe_birth != observation.thread_birth) {
            s.bridge_probe_multithread = true;
        }
        if (s.bridge_present_ticket) s.bridge_present_overlap = true;
        if (!s.pending_operations.empty()) s.bridge_operation_overlap = true;
        if (snapshot.entries_attempted > scene_bridge::kMaxProbeEntries || snapshot.target.outer_count > 4 ||
            snapshot.bsp.candidates_copied > scene_bridge::kMaxProbeBspBlocks ||
            (snapshot.target.asset.audit.available &&
             snapshot.target.asset.audit.chain_count > scene_bridge::kMaxAssetAuditSlots) ||
            (snapshot.target.mesh_node.sections_available &&
             snapshot.target.mesh_node.sections_copied > scene_bridge::kMaxMeshNodeSections)) {
            ++s.bridge_dropped; return;
        }
        for (std::size_t i = 0; i < snapshot.bsp.candidates_copied; ++i) {
            const auto& block = snapshot.bsp.candidates[i];
            if (block.sections_available && block.sections_copied > scene_bridge::kMaxProbeBspSections) {
                ++s.bridge_dropped; return;
            }
        }
        auto out = Json();
        out << "{\"event\":\"scene_bridge_observation\",\"frame\":" << observation.frame
            << ",\"ticket\":" << ticket << ",\"thread_id\":" << observation.thread_id
            << ",\"next_d3d_operation\":" << observation.before_operation
            << ",\"self\":" << snapshot.self << ",\"caller\":" << snapshot.caller
            << ",\"known_caller\":" << (snapshot.known_caller ? "true" : "false") << ",\"pass\":";
        BridgeValue(out, snapshot.pass);
        out << ",\"pass_stable\":" << (snapshot.pass_stable ? "true" : "false");
        out << ",\"configured_camera\":"; BridgeCamera(out, snapshot.configured_camera);
        out << ",\"current_camera\":"; BridgeCamera(out, snapshot.current_camera);
        out << ",\"culling_camera\":"; BridgeCamera(out, snapshot.culling_camera);
        out << ",\"count\":"; BridgeValue(out, snapshot.count);
        out << ",\"array\":"; BridgeValue(out, snapshot.array);
        out << ",\"object_table_complete\":" << (snapshot.object_table_complete ? "true" : "false")
            << ",\"entries_attempted\":" << snapshot.entries_attempted
            << ",\"entries_truncated\":" << (snapshot.entries_truncated ? "true" : "false")
            << ",\"entries\":[";
        for (size_t i = 0; i < snapshot.entries_attempted; ++i) {
            if (i) out << ',';
            BridgeEntry(out, snapshot.entries[i]);
        }
        out << "],\"target\":"; BridgeTarget(out, snapshot.target);
        out << ",\"map_witness\":"; BridgeMapWitness(out,snapshot.map_witness);
        out << ",\"bsp\":"; BridgeBsp(out, snapshot.bsp); out << "}\n";
        if (Write(s, out.str())) ++s.bridge_completed;
    } catch (...) { Fail(); }
}

std::uint64_t CapturePresentBegin(IDirect3DDevice8* device, std::uint32_t thread_id) noexcept {
    try {
        const auto thread = CurrentBridgeThread();
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !s.bridge_requested || !device || device != s.device) return 0;
        if (!thread_id || thread_id != thread.id || !thread.birth || s.bridge_present_ticket || !s.bridge_next_ticket ||
            s.bridge_next_ticket == std::numeric_limits<uint64_t>::max()) {
            s.bridge_epoch_unknown = true;
            s.bridge_present_overlap = true;
            return 0;
        }
        const auto ticket = s.bridge_next_ticket++;
        s.bridge_present_ticket = ticket;
        s.bridge_present_thread = thread_id;
        s.bridge_present_birth = thread.birth;
        if (!s.bridge_pending.empty()) s.bridge_present_overlap = true;
        if (s.bridge_reset_ticket) { s.bridge_present_overlap = true; s.bridge_reset_seen = true; }
        return ticket;
    } catch (...) { Fail(); return 0; }
}

std::uint64_t CaptureResetBegin(IDirect3DDevice8* device, std::uint32_t thread_id) noexcept {
    try {
        const auto thread = CurrentBridgeThread();
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !s.bridge_requested || !device || device != s.device) return 0;
        if (!thread_id || thread_id != thread.id || !thread.birth || s.bridge_reset_ticket ||
            !s.bridge_next_ticket || s.bridge_next_ticket == std::numeric_limits<uint64_t>::max()) {
            s.bridge_epoch_unknown = true;
            s.bridge_reset_seen = true;
            return 0;
        }
        const auto ticket = s.bridge_next_ticket++;
        s.bridge_reset_ticket = ticket;
        s.bridge_reset_thread = thread_id;
        s.bridge_reset_birth = thread.birth;
        s.bridge_reset_seen = true;
        if (s.bridge_present_ticket) s.bridge_present_overlap = true;
        if (!s.bridge_pending.empty()) s.bridge_operation_overlap = true;
        return ticket;
    } catch (...) { Fail(); return 0; }
}

bool CaptureDeclarationHistoryRequested() {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        Initialize(s);
        return s.enabled && s.declaration_requested && s.history_requested;
    } catch (...) { Fail(); return false; }
}

void CaptureExperimentalFacade(bool repeated_unlock_policy, bool post_repeat_recovery) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.enabled && !s.complete && !s.begun && s.file == INVALID_HANDLE_VALUE) {
            s.experimental_facade = true;
            s.experimental_repeated_unlock = repeated_unlock_policy;
            s.experimental_repeat_recovery = repeated_unlock_policy && post_repeat_recovery;
        }
    } catch (...) { Fail(); }
}

void CaptureDevice(IDirect3DDevice8* device, const D3DPRESENT_PARAMETERS* presentation) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        Initialize(s);
        if (!s.enabled || !device || s.device) return;
        s.device = device;
        if (presentation) { s.presentation = *presentation; s.has_presentation = true; }
        { BridgeQueryGuard guard(s); s.has_caps = SUCCEEDED(device->GetDeviceCaps(&s.caps)); }
    } catch (...) { Fail(); }
}

void CaptureDeviceReleased(IDirect3DDevice8* device) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        // The underlying COM object has already been destroyed: only compare
        // its address, never dereference or release it here.
        if (!s.enabled || s.complete || !device || device != s.device) return;
        if (s.begun || s.frames_written) {
            // A partial frame is evidence, not a completed capture. Close it
            // without fabricating Present, frame_end or capture_end records.
            Stop(s);
            return;
        }
        if (s.file_trigger) {
            // A marker associated with the retiring device must never be
            // silently reapplied to a newly created device or another scene.
            // Check also a marker created since the last Present.
            const DWORD attributes = GetFileAttributesW(s.trigger_path.c_str());
            if (s.triggered || attributes != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND) {
                Stop(s);
                return;
            }
        }
        s.device = nullptr;
        s.presentation = {};
        s.has_presentation = false;
        s.caps = {};
        s.has_caps = false;
        s.frame = 0;
        // Frame zero of a replacement device has no observed prior Present.
        // Keep the monotonically increasing ticket sequence so a callback
        // from the retired device can never match a new observation.
        s.bridge_epoch_unknown = false;
        s.bridge_previous_present_known = false;
        s.bridge_previous_present_thread = 0;
        s.bridge_previous_present_birth = 0;
        s.bridge_present_ticket = 0;
        s.bridge_present_thread = 0;
        s.bridge_present_birth = 0;
        s.bridge_reset_ticket = 0;
        s.bridge_reset_thread = 0;
        s.bridge_reset_birth = 0;
        s.bridge_reset_seen = false;
        s.bridge_pending.clear();
        s.bridge_started = s.bridge_completed = s.bridge_dropped = 0;
        s.submission_started = s.submission_completed = s.submission_pending = s.submission_dropped = 0;
        s.bridge_probe_overlap = s.bridge_present_overlap = s.bridge_operation_overlap = false;
        s.bridge_draw_thread = s.bridge_probe_thread = 0;
        s.bridge_draw_birth = s.bridge_probe_birth = 0;
        s.bridge_draw_multithread = s.bridge_probe_multithread = false;
        s.draws = 0;
        s.up_bytes = 0;
        s.buffer_bytes = 0;
        s.history_words = 0;
        s.vertex_program_words = 0;
        s.vertex_constant_words = 0;
        s.had_reset = false;
        s.next_operation = 1;
        s.pending_operations.clear();
        s.concurrent_operations = false;
        if (s.key_trigger || s.file_trigger) {
            s.triggered = false;
            s.target = 0;
            // Keep key_down: a key still held across device creation is not a
            // fresh press. Releasing and pressing again arms the new device.
        }
        // For a numeric trigger retain its configured target and start that
        // device's Present count from zero. No concurrently created device is
        // adopted retroactively; the next CreateDevice must be observed.
    } catch (...) { Fail(); }
}

void CaptureAbandonDevice(IDirect3DDevice8* device) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.enabled && !s.complete && (!device || device == s.device)) Stop(s);
    } catch (...) { Fail(); }
}

bool CaptureWantsDraw(IDirect3DDevice8* device) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.enabled && !s.complete && device && device == s.device && s.triggered && s.frame == s.target;
    } catch (...) { Fail(); return false; }
}

std::uint32_t CaptureVertexConstantCount(IDirect3DDevice8* device) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device || !s.has_caps ||
            !s.vertex_constants_requested || !s.declaration_requested) return 0;
        return static_cast<std::uint32_t>(std::min<uint64_t>(s.caps.MaxVertexShaderConst, kVertexConstantRegisters));
    } catch (...) { Fail(); return 0; }
}

uint64_t CaptureDraw(IDirect3DDevice8* device, const DrawCall& draw) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device || !s.triggered || s.frame != s.target) return 0;
        Begin(s);
        if (!s.enabled) return 0;
        if (s.bridge_requested) {
            const auto thread = CurrentBridgeThread();
            if (!thread.id || !thread.birth) s.bridge_draw_multithread = true;
            else if (!s.bridge_draw_thread) { s.bridge_draw_thread = thread.id; s.bridge_draw_birth = thread.birth; }
            else if (s.bridge_draw_thread != thread.id || s.bridge_draw_birth != thread.birth)
                s.bridge_draw_multithread = true;
        }
        const uint64_t token = BeginOperation(s, OperationKind::Draw);
        BridgeQueryGuard guard(s);
        if (!token || !Draw(s, device, draw, token)) return 0;
        return token;
    } catch (...) { Fail(); return 0; }
}

void CaptureDrawResult(IDirect3DDevice8* device, uint64_t token, HRESULT result) {
    FinishOperation(device, token, result, OperationKind::Draw);
}

void CaptureBufferedDrawResult(IDirect3DDevice8* device, uint64_t token, HRESULT result, bool stable) {
    FinishOperation(device, token, result, OperationKind::Draw, stable ? 1 : 0);
}

uint64_t CaptureClear(IDirect3DDevice8* device, DWORD count, const D3DRECT* source,
                      DWORD flags, D3DCOLOR color, float depth, DWORD stencil) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device || !s.triggered || s.frame != s.target) return 0;
        Begin(s);
        if (!s.enabled) return 0;
        const uint64_t token = BeginOperation(s, OperationKind::Clear);
        if (!token) return 0;
        std::vector<D3DRECT> rectangles;
        const char* status = ClearRectangles(count, source, rectangles);
        uint32_t depth_bits = 0;
        static_assert(sizeof(depth_bits) == sizeof(depth), "D3D8 clear depth must be 32-bit");
        std::memcpy(&depth_bits, &depth, sizeof(depth_bits));
        auto out = Json();
        out << "{\"event\":\"clear\",\"frame\":" << s.frame << ",\"op_id\":" << token
            << ",\"rect_count\":" << count << ",\"rects_status\":\"" << status << "\",\"rects\":";
        if (std::strcmp(status, "none") == 0 || std::strcmp(status, "captured") == 0) {
            out << '[';
            for (size_t i = 0; i < rectangles.size(); ++i) {
                if (i) out << ',';
                const auto& rect = rectangles[i];
                out << "{\"x1\":" << static_cast<int32_t>(rect.x1) << ",\"y1\":" << static_cast<int32_t>(rect.y1)
                    << ",\"x2\":" << static_cast<int32_t>(rect.x2) << ",\"y2\":" << static_cast<int32_t>(rect.y2) << '}';
            }
            out << ']';
        } else out << "null";
        out << ",\"flags\":" << flags << ",\"color\":" << color << ",\"depth\":";
        Number(out, depth);
        out << ",\"depth_bits\":" << depth_bits << ",\"stencil\":" << stencil << "}\n";
        return Write(s, out.str()) ? token : 0;
    } catch (...) { Fail(); return 0; }
}

void CaptureShaderDrawResult(IDirect3DDevice8* device, uint64_t token, HRESULT result, int buffer_stable, bool shader_stable) {
    FinishOperation(device, token, result, OperationKind::Draw, buffer_stable, shader_stable && SUCCEEDED(result) ? 1 : 0);
}

void CaptureClearResult(IDirect3DDevice8* device, uint64_t token, HRESULT result) {
    FinishOperation(device, token, result, OperationKind::Clear);
}

void CaptureFrameImage(IDirect3DDevice8* device, FrameImageCallback callback, bool source_rect, bool dest_rect) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !s.frame_images_requested || s.frame_image_attempted || !callback || !device ||
            device != s.device || !s.triggered || s.frame != s.target) return;
        Begin(s);
        if (!s.enabled) return;
        s.frame_image_attempted = true;
        FrameImageResult result;
        if (s.pending_operations.empty()) { BridgeQueryGuard guard(s); result = callback(device, s.path, s.frame); }
        else result.stage = "pending_operations";
        auto out = Json();
        out << "{\"event\":\"frame_image\",\"frame\":" << s.frame
            << ",\"status\":\"" << result.status << "\",\"stage\":\"" << result.stage
            << "\",\"hr\":" << static_cast<int32_t>(result.hr)
            << ",\"width\":" << result.width << ",\"height\":" << result.height
            << ",\"format\":" << result.format
            << ",\"present_source_rect\":" << (source_rect ? "true" : "false")
            << ",\"present_dest_rect\":" << (dest_rect ? "true" : "false") << "}\n";
        Write(s, out.str());
    } catch (...) { Fail(); }
}

bool CaptureSampledLiveFrame(std::uint64_t frame) noexcept {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.sample_interval_ms && std::find(s.sampled_live_frames.begin(), s.sampled_live_frames.end(), frame)
            != s.sampled_live_frames.end();
    } catch (...) { return false; }
}

void CapturePresentEnd(IDirect3DDevice8* device, uint64_t ticket, HRESULT result, const std::uint64_t* live_frame_id) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device) return;
        bool present_tracked = false;
        std::uint32_t reentry_count = 0;
        bool probe_reentered = false;
        if (s.bridge_requested) {
            reentry_count = bridge_reentry_count.load(std::memory_order_relaxed);
            probe_reentered = reentry_count == std::numeric_limits<std::uint32_t>::max() ||
                reentry_count != s.bridge_reentry_baseline;
            if (probe_reentered && s.triggered && s.frame == s.target) s.bridge_probe_overlap = true;
            const auto thread = CurrentBridgeThread();
            present_tracked = ticket && ticket == s.bridge_present_ticket &&
                s.bridge_present_thread && s.bridge_present_thread == thread.id &&
                s.bridge_present_birth && s.bridge_present_birth == thread.birth;
            if (!present_tracked) s.bridge_epoch_unknown = true;
            if (!s.bridge_pending.empty()) s.bridge_present_overlap = true;
        }
        if (s.triggered && s.frame == s.target) {
            Begin(s);
            if (!s.enabled) return;
            if (!s.pending_operations.empty()) {
                auto incomplete = Json();
                incomplete << "{\"event\":\"capture_incomplete\",\"frame\":" << s.frame
                           << ",\"reason\":\"pending_operations\",\"count\":" << s.pending_operations.size() << "}\n";
                Write(s, incomplete.str());
                Stop(s);
                return;
            }
            if (s.bridge_requested) {
                const bool installed = s.bridge_install_known &&
                    s.bridge_install_status == scene_bridge::ProbeStatus::Installed;
                const bool same_thread = s.bridge_previous_present_known && s.bridge_previous_present_thread &&
                    s.bridge_present_thread == s.bridge_previous_present_thread &&
                    s.bridge_present_thread == s.bridge_probe_thread &&
                    s.bridge_present_thread == s.bridge_draw_thread && s.bridge_present_birth &&
                    s.bridge_present_birth == s.bridge_previous_present_birth &&
                    s.bridge_present_birth == s.bridge_probe_birth && s.bridge_present_birth == s.bridge_draw_birth &&
                    !s.bridge_probe_multithread &&
                    !s.bridge_draw_multithread;
                const bool synchronized = installed && s.bridge_completed && present_tracked && SUCCEEDED(result) &&
                    !s.bridge_epoch_unknown && s.bridge_previous_present_known && same_thread && !s.had_reset &&
                    !s.concurrent_operations && !s.bridge_probe_overlap && !s.bridge_present_overlap &&
                    !s.bridge_operation_overlap && !s.bridge_reset_seen && !s.bridge_reset_ticket &&
                    !s.bridge_dropped && s.bridge_pending.empty() &&
                    s.bridge_started == s.bridge_completed;
                const char* status = !installed ? "not_installed" : !s.bridge_started && !s.bridge_dropped && !probe_reentered ?
                    "no_observation" : synchronized ? "synchronized" : "unknown";
                auto bridge = Json();
                bridge << "{\"event\":\"scene_bridge_frame\",\"frame\":" << s.frame
                       << ",\"status\":\"" << status << "\",\"previous_present_observed\":"
                       << (s.bridge_previous_present_known ? "true" : "false")
                       << ",\"previous_present_thread\":" << s.bridge_previous_present_thread
                       << ",\"present_thread\":" << s.bridge_present_thread
                       << ",\"draw_thread\":" << s.bridge_draw_thread
                       << ",\"probe_thread\":" << s.bridge_probe_thread
                       << ",\"same_thread\":" << (same_thread ? "true" : "false")
                       << ",\"present_tracked\":" << (present_tracked ? "true" : "false")
                       << ",\"epoch_unknown\":" << (s.bridge_epoch_unknown ? "true" : "false")
                       << ",\"probe_overlap\":" << (s.bridge_probe_overlap ? "true" : "false")
                       << ",\"probe_reentered\":" << (probe_reentered ? "true" : "false")
                       << ",\"present_overlap\":" << (s.bridge_present_overlap ? "true" : "false")
                       << ",\"operation_overlap\":" << (s.bridge_operation_overlap ? "true" : "false")
                       << ",\"reset_seen\":" << (s.bridge_reset_seen ? "true" : "false")
                       << ",\"reset_pending\":" << (s.bridge_reset_ticket ? "true" : "false")
                       << ",\"started\":" << s.bridge_started << ",\"completed\":" << s.bridge_completed
                       << ",\"pending\":" << s.bridge_pending.size() << ",\"dropped\":" << s.bridge_dropped;
                if (s.submission_requested)
                    bridge << ",\"submission_started\":" << s.submission_started
                           << ",\"submission_completed\":" << s.submission_completed
                           << ",\"submission_pending\":" << s.submission_pending
                           << ",\"submission_dropped\":" << s.submission_dropped;
                bridge << "}\n";
                if (!Write(s, bridge.str())) return;
            }
            auto out = Json();
            out << "{\"event\":\"frame_end\",\"frame\":" << s.frame
                << ",\"present_hr\":" << static_cast<int32_t>(result) << ",\"draws\":" << s.draws
                << ",\"had_reset\":" << (s.had_reset ? "true" : "false")
                << ",\"concurrent_operations\":" << (s.concurrent_operations ? "true" : "false");
            if (live_frame_id && present_tracked && SUCCEEDED(result)) out << ",\"scene_live_frame\":" << *live_frame_id;
            out << ",\"replayable\":false}\n";
            if (!Write(s, out.str())) return;
            if (s.sample_interval_ms && live_frame_id && present_tracked && SUCCEEDED(result))
                s.sampled_live_frames.push_back(*live_frame_id);
            ++s.frames_written;
            if (s.frames_written == s.frames_requested) {
                auto end = Json();
                end << "{\"event\":\"capture_end\",\"frames\":" << s.frames_written << "}\n";
                if (Write(s, end.str())) s.complete = true;
                Stop(s);  // Closing commits the final record to the OS.
                return;
            }
            // Preserve globally unique operation IDs, but reset frame-local
            // budgets and hazards only once all paired operations completed.
            s.begun = false;
            s.draws = 0;
            s.up_bytes = 0;
            s.buffer_bytes = 0;
            s.history_words = 0;
            s.vertex_program_words = 0;
            s.vertex_constant_words = 0;
            s.had_reset = false;
            s.concurrent_operations = false;
            s.frame_image_attempted = false;
            s.bridge_pending.clear();
            s.bridge_started = s.bridge_completed = s.bridge_dropped = 0;
            s.submission_started = s.submission_completed = s.submission_pending = s.submission_dropped = 0;
            s.bridge_probe_overlap = s.bridge_present_overlap = s.bridge_operation_overlap = false;
            s.bridge_reset_seen = s.bridge_reset_ticket != 0;
            s.bridge_draw_thread = s.bridge_probe_thread = 0;
            s.bridge_draw_birth = s.bridge_probe_birth = 0;
            s.bridge_draw_multithread = s.bridge_probe_multithread = false;
            if (s.bridge_requested) {
                s.bridge_reentry_baseline = reentry_count;
                s.bridge_previous_present_known = present_tracked && SUCCEEDED(result) && !s.bridge_epoch_unknown;
                s.bridge_previous_present_thread = s.bridge_previous_present_known ? s.bridge_present_thread : 0;
                s.bridge_previous_present_birth = s.bridge_previous_present_known ? s.bridge_present_birth : 0;
                if (present_tracked) { s.bridge_present_ticket = 0; s.bridge_present_thread = 0; s.bridge_present_birth = 0; }
            }
            ++s.frame;
            if (s.sample_interval_ms) {
                s.last_sample_tick = GetTickCount64();
                s.target = std::numeric_limits<uint64_t>::max();
            } else s.target = s.frame;
            return;
        }
        if (s.bridge_requested) {
            s.bridge_reentry_baseline = reentry_count;
            s.bridge_previous_present_known = present_tracked && SUCCEEDED(result) && !s.bridge_epoch_unknown;
            s.bridge_previous_present_thread = s.bridge_previous_present_known ? s.bridge_present_thread : 0;
            s.bridge_previous_present_birth = s.bridge_previous_present_known ? s.bridge_present_birth : 0;
            if (present_tracked) { s.bridge_present_ticket = 0; s.bridge_present_thread = 0; s.bridge_present_birth = 0; }
            // A reset completed before this boundary does not taint the next
            // interval; one still in flight does.
            s.bridge_reset_seen = s.bridge_reset_ticket != 0;
            s.bridge_present_overlap = s.bridge_reset_ticket != 0;
        }
        if (s.key_trigger) CheckKey(s);
        if (s.file_trigger) CheckTriggerFile(s);
        if (s.sample_interval_ms && s.triggered && s.target == std::numeric_limits<uint64_t>::max() &&
            GetTickCount64() - s.last_sample_tick >= s.sample_interval_ms) {
            if (s.frame >= std::numeric_limits<uint64_t>::max() - 1) { Stop(s); return; }
            s.target = s.frame + 1;
        }
        if (s.frame != std::numeric_limits<uint64_t>::max()) ++s.frame;
        else Stop(s);
    } catch (...) { Fail(); }
}

void CapturePresent(IDirect3DDevice8* device, HRESULT result) {
    CapturePresentEnd(device, 0, result);
}

void CaptureResetEnd(IDirect3DDevice8* device, uint64_t ticket,
                     const D3DPRESENT_PARAMETERS* presentation, HRESULT result) {
    try {
        const auto thread = CurrentBridgeThread();
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device) return;
        if (s.bridge_requested) {
            const bool tracked = ticket && ticket == s.bridge_reset_ticket &&
                s.bridge_reset_thread == thread.id && s.bridge_reset_birth && s.bridge_reset_birth == thread.birth;
            if (!tracked) s.bridge_epoch_unknown = true;
            if (tracked) { s.bridge_reset_ticket = 0; s.bridge_reset_thread = 0; s.bridge_reset_birth = 0; }
            s.bridge_reset_seen = true;
            if (s.bridge_present_ticket) s.bridge_present_overlap = true;
            if (!s.bridge_pending.empty()) s.bridge_operation_overlap = true;
        }
        if (SUCCEEDED(result) && presentation) {
            s.presentation = *presentation;
            s.has_presentation = true;
        }
        if (!s.triggered || s.frame != s.target) return;
        Begin(s);
        if (!s.enabled) return;
        s.had_reset = true;
        auto out = Json();
        out << "{\"event\":\"reset\",\"frame\":" << s.frame << ",\"reset_hr\":"
            << static_cast<int32_t>(result) << ",\"replayable\":false}\n";
        Write(s, out.str());
    } catch (...) { Fail(); }
}

void CaptureReset(IDirect3DDevice8* device, const D3DPRESENT_PARAMETERS* presentation, HRESULT result) {
    CaptureResetEnd(device, 0, presentation, result);
}

}  // namespace t3render
