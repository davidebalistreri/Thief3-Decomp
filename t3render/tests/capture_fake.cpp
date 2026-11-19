// Host-side behavioral test of capture.cpp; this is not a Windows ABI test.
#include "capture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool key_down = false;
unsigned writes = 0;
unsigned opens = 0;
bool bad_create_options = false;
std::uint64_t fake_tick = 0;
constexpr uintptr_t kVertexAddress = 0x01000000;
constexpr uintptr_t kIndexAddress = 0x02000000;
constexpr uintptr_t kGuardAddress = 0x03000000;
constexpr uintptr_t kRectAddress = 0x04000000;
std::array<BYTE, 256 * 1024> fake_vertex{};
std::array<BYTE, 64> fake_index{};
std::array<D3DRECT, 2> fake_rectangles{{{-5, 2, 31, 42}, {50, 60, 70, 80}}};
unsigned memory_reads = 0;
unsigned image_calls = 0;

t3render::FrameImageResult FakeFrameImage(IDirect3DDevice8*, const std::wstring& path, std::uint64_t frame) {
    ++image_calls;
    if (path.empty() || frame != image_calls) return {};
    return {"captured", "complete", S_OK, 2, 1, 21};
}

std::string Narrow(const wchar_t* source) {
    std::string result;
    while (*source) result.push_back(static_cast<char>(*source++));
    return result;
}

struct Texture : IDirect3DTexture8 {
    ULONG refs = 1;
    ULONG Release() override { return --refs; }
    D3DRESOURCETYPE GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD GetLevelCount() override { return 3; }
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC* out) override {
        if (level) return E_FAIL;
        *out = {D3DFMT_INDEX16, 5, 2, 64, 32};
        return S_OK;
    }
};
struct VertexBuffer : IDirect3DVertexBuffer8 {
    ULONG refs = 1;
    DWORD size = 4096;
    bool desc_failure = false;
    ULONG Release() override { return --refs; }
    HRESULT GetDesc(D3DVERTEXBUFFER_DESC* out) override {
        if (desc_failure) return E_FAIL;
        *out = {D3DFMT_UNKNOWN, D3DRTYPE_VERTEXBUFFER, 7, 2, size, 123};
        return S_OK;
    }
};
struct IndexBuffer : IDirect3DIndexBuffer8 {
    ULONG refs = 1;
    DWORD size = 1024;
    D3DFORMAT format = D3DFMT_INDEX16;
    bool desc_failure = false;
    ULONG Release() override { return --refs; }
    HRESULT GetDesc(D3DINDEXBUFFER_DESC* out) override {
        if (desc_failure) return E_FAIL;
        *out = {format, D3DRTYPE_INDEXBUFFER, 8, 2, size};
        return S_OK;
    }
};

struct Device : IDirect3DDevice8 {
    Texture texture;
    VertexBuffer vertex;
    IndexBuffer index;
    unsigned getters = 0;
    DWORD render_state = 42;
    unsigned vertex_shader_getters = 0;
    unsigned stream_getters = 0, index_getters = 0;
    DWORD vertex_shader = 444;
    DWORD pixel_shader = 555;
    UINT stream_stride = 32, index_base = 3;
    bool vertex_get_failure = false, index_get_failure = false;
    bool vertex_unbound = false, index_unbound = false;
    float material_power = 16;
    D3DVIEWPORT8 viewport = {1, 2, 1280, 720, 0, 1};

    HRESULT GetDeviceCaps(D3DCAPS8* out) override {
        ++getters; *out = {2, 2, 2, 4, 96}; return S_OK;
    }
    HRESULT GetTransform(D3DTRANSFORMSTATETYPE type, D3DMATRIX* out) override {
        ++getters;
        if (type != D3DTS_VIEW && type != D3DTS_PROJECTION) return E_FAIL;
        *out = {};
        for (int i = 0; i < 4; ++i) out->m[i][i] = 1;
        out->m[3][0] = type == D3DTS_VIEW ? 12 : 34;
        return S_OK;
    }
    HRESULT GetViewport(D3DVIEWPORT8* out) override {
        ++getters;
        if (std::getenv("T3_TEST_BRIDGE_REENTER")) (void)t3render::CaptureBridgeBegin(GetCurrentThreadId());
        *out = viewport;
        return S_OK;
    }
    HRESULT GetRenderState(D3DRENDERSTATETYPE name, DWORD* out) override {
        ++getters;
        // Some real runtimes accept unknown numeric IDs, so in this mode the
        // fixture succeeds even for invalid states. The writer must filter.
        if (std::getenv("T3_TEST_ALL_STATES")) { *out = static_cast<DWORD>(name); return S_OK; }
        if (name != D3DRS_ZENABLE) return E_FAIL;
        *out = render_state; return S_OK;
    }
    HRESULT GetMaterial(D3DMATERIAL8* out) override {
        ++getters; *out = {}; out->Diffuse = {0.5f, 0.25f, 0.75f, 1};
        out->Power = material_power; return S_OK;
    }
    HRESULT GetLight(DWORD index_number, D3DLIGHT8* out) override {
        ++getters; if (index_number != 1) return E_FAIL;
        *out = {}; out->Type = 2; out->Position = {1, 2, 3}; out->Range = 10;
        return S_OK;
    }
    HRESULT GetLightEnable(DWORD index_number, BOOL* out) override {
        ++getters; if (index_number != 1) return E_FAIL; *out = TRUE; return S_OK;
    }
    HRESULT GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE name, DWORD* out) override {
        ++getters;
        if (std::getenv("T3_TEST_ALL_STATES")) { *out = stage * 1000 + static_cast<DWORD>(name); return S_OK; }
        if (stage != 0 || name != D3DTSS_COLOROP) return E_FAIL;
        *out = 9; return S_OK;
    }
    HRESULT GetTexture(DWORD stage, IDirect3DBaseTexture8** out) override {
        ++getters; *out = nullptr;
        if (stage == 0) { ++texture.refs; *out = &texture; }
        return S_OK;
    }
    HRESULT GetVertexShader(DWORD* out) override { ++getters; ++vertex_shader_getters; *out = vertex_shader; return S_OK; }
    HRESULT GetPixelShader(DWORD* out) override { ++getters; *out = pixel_shader; return S_OK; }
    HRESULT GetStreamSource(UINT stream, IDirect3DVertexBuffer8** out, UINT* stride) override {
        ++getters; ++stream_getters; *out = nullptr; *stride = 0;
        if (vertex_get_failure) return E_FAIL;
        if (stream == 0 && !vertex_unbound) { ++vertex.refs; *out = &vertex; *stride = stream_stride; }
        return S_OK;
    }
    HRESULT GetIndices(IDirect3DIndexBuffer8** out, UINT* base) override {
        ++getters; ++index_getters; *out = nullptr; *base = 0;
        if (index_get_failure) return E_FAIL;
        if (!index_unbound) { ++index.refs; *out = &index; *base = index_base; }
        return S_OK;
    }
    bool Unchanged() const {
        return texture.refs == 1 && vertex.refs == 1 && index.refs == 1 && render_state == 42 &&
            vertex_shader == 444 && pixel_shader == 555 && material_power == 16 &&
            viewport.X == 1 && viewport.Y == 2 && viewport.Width == 1280 && viewport.Height == 720;
    }
};

struct ObserverState {
    Device* expected = nullptr;
    bool vertex_known = true, index_known = true;
    bool repeated_unlock = false;
    unsigned vertex_reads = 0, index_reads = 0;
};

bool ReadObservedVertex(void* context, IDirect3DDevice8* device, IDirect3DVertexBuffer8* buffer,
                        std::size_t offset, void* destination, std::size_t length) {
    auto& state = *static_cast<ObserverState*>(context);
    ++state.vertex_reads;
    if (device != state.expected || buffer != &state.expected->vertex || !destination) return false;
    auto* bytes = static_cast<BYTE*>(destination);
    if (!state.vertex_known) { if (length) bytes[0] = 0xef; return false; }
    for (std::size_t i = 0; i < length; ++i) bytes[i] = static_cast<BYTE>((offset + i) & 0xff);
    return true;
}

bool ReadObservedIndex(void* context, IDirect3DDevice8* device, IDirect3DIndexBuffer8* buffer,
                       std::size_t offset, void* destination, std::size_t length) {
    auto& state = *static_cast<ObserverState*>(context);
    ++state.index_reads;
    if (device != state.expected || buffer != &state.expected->index || !destination) return false;
    auto* bytes = static_cast<BYTE*>(destination);
    if (!state.index_known) { if (length) bytes[0] = 0xef; return false; }
    for (std::size_t i = 0; i < length; ++i) {
        const auto address = offset + i;
        bytes[i] = address % 2 ? 0 : static_cast<BYTE>(1 + address / 2);
    }
    return true;
}

t3render::buffers::ShadowReadStatus DetailedVertex(void* context, IDirect3DDevice8* device, IDirect3DVertexBuffer8* buffer,
                                                   std::size_t offset, void* destination, std::size_t length) {
    if (ReadObservedVertex(context, device, buffer, offset, destination, length))
        return static_cast<ObserverState*>(context)->repeated_unlock
            ? t3render::buffers::ShadowReadStatus::CapturedPostRepeatedUnlock : t3render::buffers::ShadowReadStatus::Captured;
    return static_cast<ObserverState*>(context)->repeated_unlock
        ? t3render::buffers::ShadowReadStatus::AbandonedRepeatedUnlock : t3render::buffers::ShadowReadStatus::Abandoned;
}
t3render::buffers::ShadowReadStatus DetailedIndex(void* context, IDirect3DDevice8* device, IDirect3DIndexBuffer8* buffer,
                                                  std::size_t offset, void* destination, std::size_t length) {
    return ReadObservedIndex(context, device, buffer, offset, destination, length)
        ? t3render::buffers::ShadowReadStatus::Captured : t3render::buffers::ShadowReadStatus::UnknownData;
}

t3render::DrawCall MakeDraw(t3render::DrawKind kind) {
    t3render::DrawCall draw{};
    draw.kind = kind;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.primitive_count = 2;
    draw.num_vertices = 6;
    draw.index_format = D3DFMT_INDEX16;
    return draw;
}
void SimulatedDraw(IDirect3DDevice8* device, const t3render::DrawCall& draw) {
    const auto token = t3render::CaptureDraw(device, draw);
    t3render::CaptureDrawResult(device, token, S_OK);
}
}  // namespace

thread_local DWORD fake_last_error = ERROR_SUCCESS;
thread_local DWORD fake_thread_id = 11;
thread_local DWORD fake_thread_birth = 1234;
DWORD GetLastError() { return fake_last_error; }
DWORD GetCurrentThreadId() { return fake_thread_id; }
std::uint64_t GetTickCount64() { return fake_tick; }
HANDLE GetCurrentThread() { return 456; }
BOOL GetThreadTimes(HANDLE thread, FILETIME* created, FILETIME*, FILETIME*, FILETIME*) {
    if (thread != 456 || !fake_thread_birth) return FALSE;
    *created = {fake_thread_birth, 0};
    return TRUE;
}
void SetLastError(DWORD value) { fake_last_error = value; }

DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, DWORD size) {
    const std::string narrow_name = Narrow(name);
    const char* value = std::getenv(narrow_name.c_str());
    if (!value) { SetLastError(ERROR_ENVVAR_NOT_FOUND); return 0; }
    if (!*value) { SetLastError(ERROR_SUCCESS); return 0; }
    const DWORD length = static_cast<DWORD>(std::strlen(value));
    if (!buffer || size <= length) return length + 1;
    for (DWORD i = 0; i < length; ++i) buffer[i] = static_cast<unsigned char>(value[i]);
    buffer[length] = 0;
    return length;
}
DWORD GetFileAttributesW(const wchar_t* path) {
    const std::string name = Narrow(path);
    struct stat info{};
    if (stat(name.c_str(), &info) == 0)
        return S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    const auto slash = name.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "." : name.substr(0, slash);
    SetLastError(stat(parent.c_str(), &info) == 0 && S_ISDIR(info.st_mode)
        ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
}
HANDLE CreateFileW(const wchar_t* path, DWORD access, DWORD, void*, DWORD disposition, DWORD, void*) {
    ++opens;
    if (access != GENERIC_WRITE || disposition != CREATE_NEW) {
        bad_create_options = true;
        return INVALID_HANDLE_VALUE;
    }
    return open(Narrow(path).c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
}
BOOL WriteFile(HANDLE handle, const void* data, DWORD size, DWORD* written, void*) {
    const char* fail_after = std::getenv("T3_TEST_WRITE_FAIL_AFTER");
    if (fail_after && writes >= std::strtoul(fail_after, nullptr, 10)) return FALSE;
    ++writes;
    const char* max_write = std::getenv("T3_TEST_MAX_WRITE");
    if (max_write) size = std::min<DWORD>(size, std::strtoul(max_write, nullptr, 10));
    const ssize_t result = write(handle, data, size);
    if (result <= 0) return FALSE;
    *written = static_cast<DWORD>(result);
    return TRUE;
}
BOOL CloseHandle(HANDLE handle) { return close(handle) == 0 ? TRUE : FALSE; }
BOOL GetKeyboardState(BYTE* keys) {
    std::memset(keys, 0, 256);
    if (key_down) keys[VK_F11] = 0x80;
    return TRUE;
}
SIZE_T VirtualQuery(const void* pointer, MEMORY_BASIC_INFORMATION* info, SIZE_T size) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (size != sizeof(*info)) return 0;
    uintptr_t base = 0;
    SIZE_T length = 0;
    DWORD protection = PAGE_READWRITE;
    if (address >= kVertexAddress && address - kVertexAddress < fake_vertex.size()) {
        // Expose two adjacent readable regions so the writer must walk both.
        const size_t half = fake_vertex.size() / 2;
        const bool second = address - kVertexAddress >= half;
        base = kVertexAddress + (second ? half : 0); length = half;
    } else if (address >= kIndexAddress && address - kIndexAddress < fake_index.size()) {
        base = kIndexAddress; length = fake_index.size();
    } else if (address >= kRectAddress && address - kRectAddress < sizeof(fake_rectangles)) {
        base = kRectAddress; length = sizeof(fake_rectangles);
    } else if (address >= kGuardAddress && address - kGuardAddress < 4096) {
        base = kGuardAddress; length = 4096; protection |= PAGE_GUARD;
    } else return 0;
    *info = {reinterpret_cast<void*>(base), length, MEM_COMMIT, protection};
    return sizeof(*info);
}
HANDLE GetCurrentProcess() { return 123; }
BOOL ReadProcessMemory(HANDLE process, const void* source, void* dest, SIZE_T size, SIZE_T* copied) {
    ++memory_reads;
    if (process != 123) return FALSE;
    const uintptr_t address = reinterpret_cast<uintptr_t>(source);
    const BYTE* buffer = nullptr;
    SIZE_T available = 0;
    if (address >= kVertexAddress && address - kVertexAddress < fake_vertex.size()) {
        const size_t offset = address - kVertexAddress;
        buffer = fake_vertex.data() + offset; available = fake_vertex.size() - offset;
    } else if (address >= kIndexAddress && address - kIndexAddress < fake_index.size()) {
        const size_t offset = address - kIndexAddress;
        buffer = fake_index.data() + offset; available = fake_index.size() - offset;
    } else if (address >= kRectAddress && address - kRectAddress < sizeof(fake_rectangles)) {
        const size_t offset = address - kRectAddress;
        buffer = reinterpret_cast<const BYTE*>(fake_rectangles.data()) + offset;
        available = sizeof(fake_rectangles) - offset;
    }
    if (!buffer || size > available) return FALSE;
    const char* partial = std::getenv("T3_TEST_PARTIAL_READ");
    if (partial && size) {
        *copied = size - 1;
        std::memcpy(dest, buffer, *copied);
        return std::strcmp(partial, "success") == 0 ? TRUE : FALSE;
    }
    std::memcpy(dest, buffer, size);
    *copied = size;
    return TRUE;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string scenario = argv[1];
    Device device;
    Device unrelated;
    D3DPRESENT_PARAMETERS presentation{};
    presentation.BackBufferWidth = 1280;
    presentation.BackBufferHeight = 720;
    presentation.Windowed = TRUE;
    const bool configured = t3render::CaptureConfigured();
    if (scenario == "history_config")
        std::cout << "history_requested=" << t3render::CaptureDeclarationHistoryRequested() << '\n';
    if (scenario == "experimental_header") t3render::CaptureExperimentalFacade();
    if (scenario == "experimental_repeated_header") t3render::CaptureExperimentalFacade(true);
    if (scenario == "experimental_recovery_header") t3render::CaptureExperimentalFacade(true, true);
    t3render::CaptureDevice(&device, &presentation);
    t3render::CaptureDevice(&unrelated, &presentation);
    const auto draw = MakeDraw(t3render::DrawKind::Primitive);
    const auto indexed = MakeDraw(t3render::DrawKind::IndexedPrimitive);
    const auto up = MakeDraw(t3render::DrawKind::PrimitiveUP);
    const auto indexed_up = MakeDraw(t3render::DrawKind::IndexedPrimitiveUP);

    for (size_t i = 0; i < fake_vertex.size(); ++i) fake_vertex[i] = static_cast<BYTE>(i & 0xff);
    fake_index[0] = 2; fake_index[2] = 3; fake_index[4] = 4;
    fake_index[32] = 3; fake_index[36] = 4; fake_index[40] = 5;

    bool second_used = false;
    if (scenario.rfind("bridge_", 0) == 0) {
        using t3render::scene_bridge::ProbeStatus;
        const bool install_failure = scenario == "bridge_install_failure";
        t3render::CaptureBridgeInstallResult(install_failure ? ProbeStatus::PrologueMismatch : ProbeStatus::Installed);
        unsigned present_calls=0;
        auto present = [&](IDirect3DDevice8* target = nullptr) {
            if (!target) target = &device;
            const auto token = t3render::CapturePresentBegin(target, fake_thread_id);
            const std::uint64_t live_frame=987;
            const bool link=scenario=="bridge_present_link" || scenario=="bridge_untracked_link" || scenario=="bridge_failed_link";
            ++present_calls;
            const auto captured_token=scenario=="bridge_untracked_link" && present_calls>1 ? 0 : token;
            const auto result=scenario=="bridge_failed_link" && present_calls>1 ? E_FAIL : S_OK;
            t3render::CapturePresentEnd(target, captured_token, result,
                                       link ? &live_frame : nullptr);
        };
        if (scenario == "bridge_prior_reentry") t3render::CaptureBridgeMarkReentered();
        if (scenario != "bridge_missing_pre") present();
        IDirect3DDevice8* scene_device = &device;
        if (scenario == "bridge_device_replace") {
            t3render::CaptureDeviceReleased(&device);
            t3render::CaptureDevice(&unrelated, &presentation);
            second_used = true;
            scene_device = &unrelated;
            if (t3render::CaptureBridgeBegin(fake_thread_id)) return 8;
            fake_thread_birth = 4321;
            present(scene_device);
        }
        t3render::scene_bridge::ProbeSnapshot snapshot{};
        snapshot.thread_id = fake_thread_id;
        snapshot.self = 0x1234;
        snapshot.pass = {2, true};
        snapshot.count = {1, true};
        snapshot.entries_attempted = 1;
        snapshot.entries[0].address = {0x5678, true};
        snapshot.entries[0].owner_name_available = true;
        std::strcpy(snapshot.entries[0].owner_name, "synthetic\"actor");
        if (scenario == "bridge_map_witness") {
            auto& entry=snapshot.entries[0];entry.vtable={0x10e4e8c0,true};entry.type={4,true};
            entry.owner_candidate={0x4000,true};entry.owner_object_index={2,true};entry.owner_fname={(2u<<16)|7,true};
            entry.owner_class={0x7000,true};entry.owner_backlink=entry.address;entry.owner_table_member=entry.owner_stable=true;
            std::strcpy(entry.owner_name,"StaticMeshActor");snapshot.array={0x2000,true};
            auto& witness=snapshot.map_witness;witness.available=witness.list_stable=witness.object_table_complete=true;
            witness.entry=entry;witness.owner_outer={0xa000,true};
            witness.owner_class.address=entry.owner_class;witness.owner_class.fname={7,true};
            witness.owner_class.member=witness.owner_class.stable=witness.owner_class.name_available=true;
            std::strcpy(witness.owner_class.name,"StaticMeshActor");
            witness.outer.address=witness.owner_outer;witness.outer.fname={8,true};witness.outer.outer={0,true};
            witness.outer.member=witness.outer.stable=witness.outer.name_available=true;
            std::strcpy(witness.outer.name,"LOADGAME:bedroom");
        }
        if (scenario == "bridge_asset_audit") {
            auto& asset = snapshot.target.asset;
            asset.status = t3render::scene_bridge::AssetStatus::Candidate;
            asset.descriptor_match = asset.local_property_found = asset.proxy_live_member = true;
            asset.proxy_vtable_match = asset.resource_vtable_match = asset.stable = true;
            asset.name_available = true;
            std::strcpy(asset.name, "INNbed1A1");
            auto& a = asset.audit;
            a.available = true;
            a.descriptor_table = 0x6000; a.descriptor_array = 0x6100; a.descriptor = 0x6200;
            a.descriptor_vtable = 0x10e72e48; a.descriptor_copy = 0x10aeecc0;
            a.descriptor_count = 1; a.descriptor_size = 4; a.property_hash = 0x2000;
            a.bucket_head = 3; a.slot_index = 3; a.slot_id_flags = 0x802b; a.slot_value = 0x3000;
            a.chain_count = 1; a.chain[0] = {3, 0, 0x802b, 0x3000};
            a.objects_data = 0x8000; a.objects_count = 3; a.objects_capacity = 3; a.object_index = 2;
            a.proxy = 0x3000; a.proxy_vtable = 0x10e4dfa8; a.proxy_index = 2;
            a.resource = 0x4000; a.resource_vtable = 0x10ec0e20;
            a.name_address = 0x5004; a.name_length = 9;
            auto& node = snapshot.target.mesh_node;
            node.pool_scan_complete = true; node.nodes_scanned = 1; node.occurrences = 1;
            node.address = {0x9000, true}; node.vtable = {0x10e49d14, true}; node.name_address = {0x9100, true};
            node.name_available = node.counts_available = node.bounds_available = node.stable = true;
            std::strcpy(node.name, "INNbed1A1");
            node.vertex_count = 257; node.triangle_count = 236; node.index_count = 708; node.section_count = 3;
            const float bounds[6] = {-1, -2, -3, 1, 2, 3};
            std::memcpy(node.bounds_bits, bounds, sizeof(bounds));
            node.section_array = {0x9200, true}; node.instance_materials = {0x9300, true};
            node.sections_available = true; node.sections_copied = 3;
            const char* names[] = {"BF", "GENgoodnessA_D", "GENiFABRICwoolA2_D"};
            const unsigned first[] = {0, 24, 504}, count[] = {24, 480, 204};
            for (unsigned i = 0; i < 3; ++i) {
                auto& section = node.sections[i];
                section.name_available = section.material_name_available = true;
                std::strcpy(section.name, "Default"); std::strcpy(section.material_name, names[i]);
                section.vertex_count = 257; section.first_index = first[i]; section.index_count = count[i];
                section.triangles = count[i] / 3;
            }
            snapshot.count = {2, true}; snapshot.entries_attempted = 2;
            snapshot.entries[1].list_index = 1;
            snapshot.entries[1].address = {0x171b0, true};
            snapshot.entries[1].vtable = {0x10e4a668, true}; snapshot.entries[1].type = {5, true};
            auto& bsp = snapshot.bsp;
            bsp.renderer = {0x15000, true}; bsp.source = {0x16000, true}; bsp.blocks = {0x17004, true};
            bsp.block_count = 2; bsp.type5_seen = bsp.member_count = bsp.candidates_copied = 1; bsp.source_stable = true;
            auto& block = bsp.candidates[0];
            block.list_index = block.source_index = 1; block.address = {0x171b0, true};
            block.metadata_available = block.stable = true;
            block.vertex_count = 3; block.triangle_count = 1; block.index_count = 3; block.section_count = 1;
            std::memcpy(block.bounds_bits, bounds, sizeof(bounds));
            const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(block.transform_bits, identity, sizeof(identity));
            block.section_array = {0x18004, true}; block.sections_available = true; block.sections_copied = 1;
            block.sections[0].material_name_available = true;
            std::strcpy(block.sections[0].material_name, "wall");
            block.sections[0].triangles = 1; block.sections[0].vertex_count = block.sections[0].index_count = 3;
        }
        if (install_failure) {
            if (t3render::CaptureBridgeBegin(fake_thread_id)) return 8;
            SimulatedDraw(scene_device, draw);
            present(scene_device);
        } else {
            const auto token = t3render::CaptureBridgeBegin(fake_thread_id);
            if (!token) return 8;
            if (scenario == "bridge_overlap_draw") SimulatedDraw(scene_device, draw);
            if (scenario == "bridge_overlap_present" || scenario == "bridge_unfinished") {
                const auto present_ticket = t3render::CapturePresentBegin(scene_device, fake_thread_id);
                if (scenario == "bridge_overlap_present") t3render::CaptureBridgeEnd(token, snapshot);
                SimulatedDraw(scene_device, draw);
                t3render::CapturePresentEnd(scene_device, present_ticket, S_OK);
                if (scenario == "bridge_unfinished") t3render::CaptureBridgeEnd(token, snapshot);
            } else {
                t3render::CaptureBridgeEnd(token, snapshot);
                if (scenario == "bridge_reentered") t3render::CaptureBridgeMarkReentered();
                if (scenario == "bridge_capacity") {
                    for (unsigned i = 1; i < 16; ++i) {
                        const auto another = t3render::CaptureBridgeBegin(fake_thread_id);
                        if (!another) return 8;
                        t3render::CaptureBridgeEnd(another, snapshot);
                    }
                    if (t3render::CaptureBridgeBegin(fake_thread_id)) return 8;
                }
                if (scenario == "bridge_thread_change") fake_thread_id = 12;
                if (scenario == "bridge_birth_change") fake_thread_birth = 9876;
                if (scenario == "bridge_image_good") {
                    SimulatedDraw(scene_device, draw);
                    const auto present_ticket = t3render::CapturePresentBegin(scene_device, fake_thread_id);
                    t3render::CaptureFrameImage(scene_device, FakeFrameImage, false, false);
                    t3render::CapturePresentEnd(scene_device, present_ticket, S_OK);
                } else if (scenario == "bridge_present_draw" || scenario == "bridge_reset_pending") {
                    const auto reset_ticket = scenario == "bridge_reset_pending" ?
                        t3render::CaptureResetBegin(scene_device, fake_thread_id) : 0;
                    const auto present_ticket = t3render::CapturePresentBegin(scene_device, fake_thread_id);
                    SimulatedDraw(scene_device, draw);
                    t3render::CapturePresentEnd(scene_device, present_ticket, S_OK);
                    if (reset_ticket) t3render::CaptureResetEnd(scene_device, reset_ticket, &presentation, S_OK);
                } else {
                    SimulatedDraw(scene_device, draw);
                    present(scene_device);
                }
            }
        }
    } else if (scenario == "sampled_series") {
        t3render::CaptureBridgeInstallResult(t3render::scene_bridge::ProbeStatus::Installed);
        for (std::uint64_t i = 0; i <= 7; ++i) {
            fake_tick = i * 500;
            const bool selected = t3render::CaptureWantsDraw(&device);
            if (selected != (i == 1 || i == 4 || i == 7)) return 8;
            if (selected) SimulatedDraw(&device, draw);
            const auto ticket = t3render::CapturePresentBegin(&device, fake_thread_id);
            const std::uint64_t live = 1000 + i;
            t3render::CapturePresentEnd(&device, ticket, S_OK, &live);
            if (t3render::CaptureSampledLiveFrame(live) != selected) return 8;
        }
        if (!t3render::CaptureSampledLiveFrame(1001) || !t3render::CaptureSampledLiveFrame(1007)) return 8;
    } else if (scenario == "frame_image") {
        t3render::CaptureFrameImage(&unrelated, FakeFrameImage, false, false);
        t3render::CaptureFrameImage(&device, FakeFrameImage, false, false); // frame 0 is not selected
        t3render::CapturePresent(&device, S_OK);
        for (unsigned i = 0; i < 2; ++i) {
            SimulatedDraw(&device, draw);
            t3render::CaptureFrameImage(&device, FakeFrameImage, false, false);
            t3render::CaptureFrameImage(&device, FakeFrameImage, false, false); // once per frame
            t3render::CapturePresent(&device, S_OK);
        }
        if (image_calls != 2) return 8;
    } else if (scenario == "shader_abandon" || scenario == "shader_abandon_all") {
        t3render::CapturePresent(&device, S_OK);
        const auto token = t3render::CaptureDraw(&device, draw);
        if (!token) return 8;
        t3render::CaptureAbandonDevice(scenario == "shader_abandon_all" ? nullptr : &device);
        t3render::CaptureDrawResult(&device, token, S_OK);
        t3render::CapturePresent(&device, S_OK);
    } else if (scenario == "shader_history" || scenario == "shader_history_budget" || scenario == "shader_history_program") {
        t3render::CapturePresent(&device, S_OK);
        t3render::ShaderSnapshot snapshot;
        snapshot.getter_result = S_OK;
        snapshot.value = 0xf0000001u;
        auto& declaration = snapshot.declaration;
        declaration.status = t3render::ir::ShaderObservationStatus::Unavailable;
        declaration.reason = t3render::ir::ShaderUnavailableReason::ResetAttempt;
        declaration.generation = 9;
        declaration.has_scan_status = true;
        declaration.scan_status = t3render::ir::DeclarationStatus::Complete;
        declaration.reset_epoch = 3;
        declaration.reset_hr = 0;
        declaration.rebind_epoch = 4;
        declaration.pre_reset_words = {0x20000000u, 0xffffffffu};
        if (scenario == "shader_history_program")
            declaration.pre_reset_program_words = {0xfffe0101u, 1u, 0xc00f0000u, 0x90e40000u, 0x0000ffffu};
        if (scenario == "shader_history_budget") {
            declaration.pre_reset_words.assign(1024, 0);
            declaration.pre_reset_words.back() = 0xffffffffu;
        }
        auto call = draw; call.shader_snapshot = &snapshot;
        const unsigned count = scenario == "shader_history_budget" ? 257 : 5;
        for (unsigned i=0; i<count; ++i) {
            if (scenario != "shader_history_budget") {
                if (i == 2) declaration.rebind_epoch = declaration.reset_epoch; // no post-reset rebind
                if (i == 3) { declaration.rebind_epoch = 4; declaration.reason = t3render::ir::ShaderUnavailableReason::HandleCollision; }
                if (i == 4) { declaration.reason = t3render::ir::ShaderUnavailableReason::ResetAttempt;
                              declaration.pre_reset_words = {0x20000000u}; } // no END
            }
            const auto token = t3render::CaptureDraw(&device, call);
            if (!token) return 8;
            t3render::CaptureShaderDrawResult(&device, token, S_OK, -1, i!=1);
        }
        t3render::CapturePresent(&device, S_OK);
        if (scenario == "shader_history_budget") {
            const auto token = t3render::CaptureDraw(&device, call);
            if (!token) return 8;
            t3render::CaptureShaderDrawResult(&device, token, S_OK, -1, true);
            t3render::CapturePresent(&device, S_OK);
        }
    } else if (scenario == "shader_program_budget") {
        t3render::CapturePresent(&device, S_OK);
        t3render::ShaderSnapshot snapshot;
        snapshot.getter_result = S_OK;
        snapshot.value = 0xf0000001u;
        auto& declaration = snapshot.declaration;
        declaration.status = t3render::ir::ShaderObservationStatus::Candidate;
        declaration.generation = 9;
        declaration.words = {0x20000000u, 0xffffffffu};
        declaration.program_status = t3render::ir::ShaderProgramStatus::Complete;
        declaration.program_words.assign(1024, 0);
        declaration.program_words.front() = 0xfffe0101u;
        declaration.program_words.back() = 0x0000ffffu;
        auto call = draw; call.shader_snapshot = &snapshot;
        for (unsigned i=0; i<257; ++i) {
            const auto token = t3render::CaptureDraw(&device, call);
            if (!token) return 8;
            t3render::CaptureShaderDrawResult(&device, token, S_OK, -1, true);
        }
        t3render::CapturePresent(&device, S_OK);
        const auto token = t3render::CaptureDraw(&device, call);
        if (!token) return 8;
        t3render::CaptureShaderDrawResult(&device, token, S_OK, -1, true);
        t3render::CapturePresent(&device, S_OK);
    } else if (scenario == "shader_constants_budget" || scenario == "shader_constants_unavailable") {
        t3render::CapturePresent(&device, S_OK);
        t3render::ShaderSnapshot snapshot;
        snapshot.getter_result = S_OK;
        snapshot.value = 0xf0000001u;
        snapshot.constants_requested = true;
        snapshot.constants_count = 96;
        snapshot.constants_result = snapshot.constants_verify_result = S_OK;
        snapshot.constants_match = true;
        for (size_t i=0; i<snapshot.constant_bits.size(); ++i) snapshot.constant_bits[i]=0x3f800000u+static_cast<unsigned>(i);
        auto call = draw; call.shader_snapshot = &snapshot;
        const unsigned draws = scenario == "shader_constants_budget" ? 683 : 4;
        for (unsigned i=0; i<draws; ++i) {
            if (scenario == "shader_constants_unavailable") {
                if (i==0) snapshot.constants_result=E_FAIL;
                if (i==1) { snapshot.constants_result=S_OK; snapshot.constants_verify_result=E_FAIL; }
                if (i==2) { snapshot.constants_verify_result=S_OK; snapshot.constants_match=false; }
                if (i==3) { snapshot.constants_count=0; snapshot.constants_result=E_FAIL;
                            snapshot.constants_verify_result=E_FAIL; snapshot.constants_match=false; }
            }
            const auto token = t3render::CaptureDraw(&device, call);
            if (!token) return 8;
            t3render::CaptureShaderDrawResult(&device, token, S_OK, -1, true);
        }
        t3render::CapturePresent(&device, S_OK);
        if (scenario == "shader_constants_budget") {
            const auto token = t3render::CaptureDraw(&device, call);
            if (!token) return 8;
            t3render::CaptureShaderDrawResult(&device, token, S_OK, -1, true);
            t3render::CapturePresent(&device, S_OK);
        }
    } else if (scenario == "shader_reason") {
        t3render::CapturePresent(&device, S_OK);
        t3render::ShaderSnapshot snapshot;
        snapshot.getter_result = S_OK;
        snapshot.value = 0xf0000001u;
        snapshot.declaration.status = t3render::ir::ShaderObservationStatus::Unavailable;
        snapshot.declaration.generation = 9;
        snapshot.declaration.has_scan_status = true;
        snapshot.declaration.scan_status = t3render::ir::DeclarationStatus::NullAddress;
        snapshot.declaration.reason = t3render::ir::ShaderUnavailableReason::ScanIncomplete;
        auto candidate = draw;
        candidate.shader_snapshot = &snapshot;
        for (unsigned i=0; i<4; ++i) {
            if (i == 1) snapshot.declaration.reason = t3render::ir::ShaderUnavailableReason::ResetAttempt;
            if (i == 2) {
                snapshot.declaration.reason = t3render::ir::ShaderUnavailableReason::ObservationError;
                snapshot.declaration.scan_status = t3render::ir::DeclarationStatus::Complete;
            }
            if (i == 3) { snapshot.declaration.generation = 0; snapshot.declaration.has_scan_status = false; }
            const auto token = t3render::CaptureDraw(&device, candidate);
            if (!token) return 8;
            t3render::CaptureShaderDrawResult(&device, token, S_OK, -1, i!=2);
        }
        t3render::CapturePresent(&device, S_OK);
    } else if (scenario == "shader" || scenario == "shader_program" || scenario == "shader_program_10" || scenario == "shader_program_unavailable" ||
               scenario == "shader_constants") {
        if (t3render::CaptureWantsDraw(&device)) return 8;
        t3render::CapturePresent(&device, S_OK);
        if (!t3render::CaptureWantsDraw(&device) || t3render::CaptureWantsDraw(&unrelated)) return 8;
        t3render::ShaderSnapshot snapshot;
        snapshot.getter_result = S_OK;
        snapshot.value = 0xf0000001u;
        snapshot.declaration.status = t3render::ir::ShaderObservationStatus::Candidate;
        snapshot.declaration.generation = 7;
        snapshot.declaration.words = {0x20000000u, 0xffffffffu};
        snapshot.declaration.program_status = t3render::ir::ShaderProgramStatus::Complete;
        snapshot.declaration.program_words = {0xfffe0101u, 1u, 0xc00f0000u, 0x90e40000u, 0x0000ffffu};
        if (scenario == "shader_program_10") snapshot.declaration.program_words[0] = 0xfffe0100u;
        if (scenario == "shader_constants") {
            snapshot.constants_requested = true;
            snapshot.constants_count = 96;
            snapshot.constants_result = snapshot.constants_verify_result = S_OK;
            snapshot.constants_match = true;
            for (size_t i=0; i<snapshot.constant_bits.size(); ++i) snapshot.constant_bits[i]=0x3f800000u+static_cast<unsigned>(i);
        }
        if (scenario == "shader_program_unavailable") {
            snapshot.declaration.program_status = t3render::ir::ShaderProgramStatus::UnsupportedVersion;
            snapshot.declaration.program_words.clear();
            snapshot.declaration.program_first_word_known = true;
            snapshot.declaration.program_first_word = 0xfffe0200;
        }
        auto candidate = draw;
        candidate.shader_snapshot = &snapshot;
        for (unsigned i=0; i<4; ++i) {
            if (i == 3) {
                snapshot.getter_result = E_FAIL;
                snapshot.declaration = {};
            }
            const auto token = t3render::CaptureDraw(&device, candidate);
            if (!token) return 8;
            t3render::CaptureShaderDrawResult(&device, token, i==1 ? E_FAIL : S_OK,
                                              scenario == "shader" && i==2 ? 0 : -1, i!=2);
        }
        if (device.vertex_shader_getters != 0) return 8;
        t3render::CapturePresent(&device, S_OK);
        if (t3render::CaptureWantsDraw(&device)) return 8;
    } else if (scenario == "window" || scenario == "window_key" || scenario == "window_file" || scenario == "window_pending" ||
        scenario == "window_release" || scenario == "window_budget") {
        if (scenario == "window_key") {
            t3render::CapturePresent(&device, S_OK); // frame 0, F11 not pressed
            key_down = true;
            t3render::CapturePresent(&device, S_OK); // arm frame 2
        } else if (scenario == "window_file") {
            SimulatedDraw(&device, draw); // no signal: nothing is captured
            t3render::CapturePresent(&device, S_OK);
            const char* marker = std::getenv("T3RENDER_CAPTURE_TRIGGER_PATH");
            if (marker) {
                const int signal = open(marker, O_CREAT | O_EXCL | O_WRONLY, 0600);
                if (signal < 0) return 9;
                close(signal);
            }
            t3render::CapturePresent(&device, S_OK); // signal arms frame 2
        } else {
            t3render::CapturePresent(&device, S_OK); // advance to frame 1
        }
        if (scenario == "window_budget") {
            device.stream_stride = 4;
            device.vertex.size = 256 * 1024;
            auto immediate = up;
            immediate.primitive_type = D3DPT_POINTLIST; immediate.primitive_count = 65536;
            immediate.vertex_stride = 4; immediate.vertex_data = reinterpret_cast<void*>(kVertexAddress);
            auto buffered = draw;
            buffered.primitive_type = D3DPT_POINTLIST; buffered.primitive_count = 65536;
            ObserverState state{&device};
            t3render::BufferObserver observer{&state, true, ReadObservedVertex, ReadObservedIndex};
            buffered.buffer_observer = &observer;
            auto emit_buffered = [&]() {
                const auto token = t3render::CaptureDraw(&device, buffered);
                if (!token) return false;
                t3render::CaptureBufferedDrawResult(&device, token, S_OK, true);
                return true;
            };
            for (unsigned i = 0; i < 33; ++i) {
                SimulatedDraw(&device, immediate);
                if (!emit_buffered()) return 8;
            }
            t3render::CapturePresent(&device, S_OK);
            SimulatedDraw(&device, immediate);
            if (!emit_buffered()) return 8;
            t3render::CapturePresent(&device, S_OK);
        } else {
            for (unsigned i = 0; i < 3; ++i) {
                const auto clear = t3render::CaptureClear(&device, 0, nullptr, 1, 0xff000000, 1.0f, 0);
                if (i == 0) {
                    // Paired operations deliberately overlap in the first frame.
                    const auto token = t3render::CaptureDraw(&device, draw);
                    t3render::CaptureClearResult(&device, clear, S_OK);
                    t3render::CaptureDrawResult(&device, token, S_OK);
                } else {
                    t3render::CaptureClearResult(&device, clear, S_OK);
                    if (scenario == "window_pending" && i == 1) {
                        const auto pending = t3render::CaptureDraw(&device, indexed);
                        t3render::CapturePresent(&device, S_OK);
                        t3render::CaptureDrawResult(&device, pending, S_OK);
                        break;
                    }
                    SimulatedDraw(&device, indexed);
                }
                if (i == 1) {
                    D3DPRESENT_PARAMETERS update = presentation;
                    update.BackBufferWidth = 1600;
                    t3render::CaptureReset(&device, &update, S_OK);
                }
                t3render::CapturePresent(&device, i == 0 ? E_FAIL : S_OK);
                if (scenario == "window_release" && i == 0) {
                    t3render::CaptureDeviceReleased(&device);
                    t3render::CaptureDevice(&unrelated, &presentation);
                    SimulatedDraw(&unrelated, draw);
                    t3render::CapturePresent(&unrelated, S_OK);
                    second_used = true;
                    break;
                }
            }
        }
    } else if (scenario == "prepared_buffers") {
        t3render::CapturePresent(&device, S_OK);
        ObserverState state{&device};
        t3render::BufferObserver observer{&state, true, ReadObservedVertex, ReadObservedIndex};
        t3render::BufferBindingSnapshot bindings;
        bindings.device = &device;
        bindings.vertex_result = device.GetStreamSource(0, &bindings.vertex, &bindings.stride);
        bindings.index_result = device.GetIndices(&bindings.index, &bindings.base);
        if (FAILED(bindings.vertex_result) || FAILED(bindings.index_result)) return 8;
        t3render::ShaderSnapshot shader;
        shader.getter_result = S_OK; shader.value = 0xf0000001u;
        shader.declaration.status = t3render::ir::ShaderObservationStatus::Candidate;
        shader.declaration.generation = 7;
        shader.declaration.words = {0x20000000u, 0xffffffffu};
        shader.declaration.program_status = t3render::ir::ShaderProgramStatus::Complete;
        shader.declaration.program_words = {0xfffe0101u, 1u, 0xc00f0000u, 0x90e40000u, 0x0000ffffu};
        shader.constants_requested = true; shader.constants_count = 1;
        shader.constants_result = shader.constants_verify_result = S_OK;
        shader.constants_match = true; shader.constant_bits[0] = 0x3f800000u;
        auto call = indexed;
        call.min_vertex_index = 2; call.num_vertices = 3;
        call.start_index = 1; call.primitive_count = 1;
        call.buffer_observer = &observer; call.buffer_bindings = &bindings; call.shader_snapshot = &shader;
        auto emit = [&](bool stable = true) {
            const auto stream_calls = device.stream_getters, index_calls = device.index_getters;
            const auto token = t3render::CaptureDraw(&device, call);
            // General state enumeration still observes two streams and the
            // indices. Buffered data must reuse the prepared getter results.
            if (!token || device.stream_getters - stream_calls != 2 || device.index_getters - index_calls != 1) return false;
            t3render::CaptureShaderDrawResult(&device, token, S_OK, stable ? 1 : 0, true);
            return true;
        };
        if (!emit()) return 8;
        bindings.device = &unrelated;
        if (!emit()) return 8;
        bindings.device = &device; bindings.vertex_result = E_FAIL; bindings.index_result = E_FAIL;
        if (!emit()) return 8;
        observer.valid = false; observer.failure_status = "device_unsafe";
        observer.failure_cause = "unlock_mismatch"; observer.failure_line = 123;
        observer.failure_unlock.present = true; observer.failure_unlock.vertex = true;
        observer.failure_unlock.last_lock_known = true; observer.failure_unlock.last_lock_hr = E_FAIL;
        observer.failure_unlock.unlock_result_known = true; observer.failure_unlock.unlock_hr_candidate = S_OK;
        observer.failure_lock.present = true; observer.failure_lock.vertex = true;
        observer.failure_lock.recovery_attempted = true; observer.failure_lock.registry_result = "invalid_range";
        observer.failure_lock.offset = 1; observer.failure_lock.requested_bytes = 0;
        observer.failure_lock.normalized_bytes = 0; observer.failure_lock.buffer_size = 8; observer.failure_lock.flags = 0;
        observer.repeated_unlock_policy = true; observer.repeated_unlocks = 2;
        if (!emit(false)) return 8;
        bindings.vertex->Release(); bindings.index->Release();
        if (state.vertex_reads != 1 || state.index_reads != 1) return 8;
        t3render::CapturePresent(&device, S_OK);
    } else if (scenario == "shadow_reason") {
        t3render::CapturePresent(&device, S_OK);
        ObserverState state{&device};
        t3render::BufferObserver observer{&state, true, ReadObservedVertex, ReadObservedIndex};
        observer.read_vertex_detailed = DetailedVertex;
        observer.read_index_detailed = DetailedIndex;
        auto call = indexed;
        call.min_vertex_index = 2; call.num_vertices = 3; call.start_index = 1; call.primitive_count = 1;
        call.buffer_observer = &observer;
        state.vertex_known = false; state.index_known = false;
        auto token = t3render::CaptureDraw(&device, call);
        if (!token) return 8;
        t3render::CaptureBufferedDrawResult(&device, token, S_OK, true);
        state.repeated_unlock = true;
        token = t3render::CaptureDraw(&device, call);
        if (!token) return 8;
        t3render::CaptureBufferedDrawResult(&device, token, S_OK, true);
        state.vertex_known = true; state.index_known = true;
        token = t3render::CaptureDraw(&device, call);
        if (!token) return 8;
        t3render::CaptureBufferedDrawResult(&device, token, S_OK, true);
        t3render::CapturePresent(&device, S_OK);
    } else if (scenario == "buffered" || scenario == "buffer_budget" || scenario == "buffer_budget_fail" ||
        scenario == "buffer_budget_mixed") {
        t3render::CapturePresent(&device, S_OK);  // target frame 1
        ObserverState state{&device};
        t3render::BufferObserver observer{&state, true, ReadObservedVertex, ReadObservedIndex};
        auto emit = [&](t3render::DrawCall call, bool stable = true, HRESULT result = S_OK) {
            call.buffer_observer = &observer;
            const auto token = t3render::CaptureDraw(&device, call);
            if (!token) return false;
            t3render::CaptureBufferedDrawResult(&device, token, result, stable);
            return true;
        };
        if (scenario == "buffer_budget" || scenario == "buffer_budget_fail" || scenario == "buffer_budget_mixed") {
            device.stream_stride = 4;
            device.vertex.size = 256 * 1024;
            auto call = draw;
            call.primitive_type = D3DPT_POINTLIST; call.primitive_count = 65536;
            state.vertex_known = scenario != "buffer_budget_fail";
            const unsigned attempts = scenario == "buffer_budget" ? 33 : scenario == "buffer_budget_mixed" ? 31 : 40;
            for (unsigned i = 0; i < attempts; ++i) if (!emit(call)) return 8;
            if (scenario == "buffer_budget_fail") { state.vertex_known = true; if (!emit(call)) return 8; }
            if (scenario == "buffer_budget_mixed") {
                device.index_base = 0;
                call = indexed; call.primitive_count = 1; call.num_vertices = 65534;
                if (!emit(call)) return 8; // 262136 vertex bytes + 6 index bytes
                call.num_vertices = 0;
                if (!emit(call)) return 8; // only 2 bytes remain, not enough for indices
            }
        } else {
            auto primitive = draw;
            primitive.start_vertex = 2; primitive.primitive_count = 1;
            auto indexed_call = indexed;
            indexed_call.min_vertex_index = 2; indexed_call.num_vertices = 3;
            indexed_call.start_index = 1; indexed_call.primitive_count = 1;
            if (!emit(primitive) || !emit(indexed_call, false)) return 8;
            state.vertex_known = false; if (!emit(indexed_call)) return 8;
            state.vertex_known = true; state.index_known = false; if (!emit(indexed_call)) return 8;
            state.index_known = true; observer.valid = false; if (!emit(indexed_call, false)) return 8;
            observer.valid = true; if (!emit(indexed_call, true, E_FAIL)) return 8;
            observer.read_vertex = nullptr; if (!emit(indexed_call)) return 8;
            observer.read_vertex = ReadObservedVertex;
            SimulatedDraw(&device, primitive); // legacy: no observer or stability field
            auto immediate = up;
            immediate.vertex_data = reinterpret_cast<void*>(kVertexAddress); immediate.vertex_stride = 4;
            immediate.primitive_count = 1; immediate.buffer_observer = &observer;
            const auto up_token = t3render::CaptureDraw(&device, immediate);
            if (!up_token) return 8;
            t3render::CaptureDrawResult(&device, up_token, S_OK);
            primitive.primitive_count = 0; if (!emit(primitive)) return 8;
            device.index_base = std::numeric_limits<UINT>::max();
            indexed_call.min_vertex_index = 1; if (!emit(indexed_call)) return 8;
            device.index_base = 3; indexed_call.min_vertex_index = 2;
            device.vertex_get_failure = true; device.index_get_failure = true; if (!emit(indexed_call)) return 8;
            device.vertex_get_failure = false; device.index_get_failure = false;
            device.vertex_unbound = true; device.index_unbound = true; if (!emit(indexed_call)) return 8;
            device.vertex_unbound = false; device.index_unbound = false;
            device.vertex.desc_failure = true; device.index.desc_failure = true; if (!emit(indexed_call)) return 8;
            device.vertex.desc_failure = false; device.index.desc_failure = false;
            device.stream_stride = 0; if (!emit(indexed_call)) return 8; device.stream_stride = 32;
            device.vertex.size = 64; device.index.size = 2; if (!emit(indexed_call)) return 8;
            device.vertex.size = 4096; device.index.size = 1024;
            device.index.format = D3DFMT_UNKNOWN; if (!emit(indexed_call)) return 8;
            device.index.format = D3DFMT_INDEX16;
            indexed_call.primitive_type = D3DPT_FAKE; if (!emit(indexed_call)) return 8;
            primitive = draw; primitive.primitive_type = D3DPT_FAKE; if (!emit(primitive)) return 8;
            primitive = draw; primitive.primitive_type = D3DPT_POINTLIST; primitive.primitive_count = 65537;
            device.stream_stride = 4; device.vertex.size = 512 * 1024; if (!emit(primitive)) return 8;
            device.stream_stride = 32; device.vertex.size = 4096;
            indexed_call.primitive_type = D3DPT_TRIANGLELIST; device.index.format = D3DFMT_INDEX32;
            if (!emit(indexed_call)) return 8; device.index.format = D3DFMT_INDEX16;
        }
        t3render::CapturePresent(&device, S_OK);
    } else if (scenario == "clear" || scenario == "clear_only" || scenario == "clear_nan" || scenario == "clear_pending" ||
        scenario == "draw_pending" || scenario == "overlap") {
        t3render::CapturePresent(&device, S_OK);  // target frame 1
        const auto empty = t3render::CaptureClear(&device, 0, nullptr, 7, 0x12345678, 0.75f, 9);
        if (!empty) return 8;
        if (scenario == "clear_pending") {
            t3render::CapturePresent(&device, S_OK);
            t3render::CaptureClearResult(&device, empty, S_OK);
        } else {
            t3render::CaptureClearResult(&device, empty, S_OK);
            if (scenario == "draw_pending") {
                const auto pending = t3render::CaptureDraw(&device, draw);
                if (!pending) return 8;
                t3render::CapturePresent(&device, S_OK);
                t3render::CaptureDrawResult(&device, pending, E_FAIL);
            } else if (scenario == "overlap") {
                const auto pending_clear = t3render::CaptureClear(&device, 1,
                    reinterpret_cast<const D3DRECT*>(kRectAddress), 1, 0xff000000, 1.0f, 0);
                const auto pending_draw = t3render::CaptureDraw(&device, draw);
                if (!pending_clear || !pending_draw) return 8;
                t3render::CaptureClearResult(&device, pending_clear, S_OK);
                t3render::CaptureDrawResult(&device, pending_draw, E_FAIL);
                t3render::CapturePresent(&device, S_OK);
            } else {
                if (scenario == "clear_nan") {
                    const auto nan = t3render::CaptureClear(&device, 0, nullptr, 1, 0, std::numeric_limits<float>::quiet_NaN(), 0);
                    if (!nan) return 8;
                    t3render::CaptureClearResult(&device, nan, S_OK);
                }
                if (scenario == "clear") {
                    SimulatedDraw(&device, draw);
                    auto emit = [&](DWORD count, const D3DRECT* rects, HRESULT result) {
                        const auto token = t3render::CaptureClear(&device, count, rects, 3, 0xaabbccdd, 0.5f, 4);
                        if (!token) return false;
                        t3render::CaptureClearResult(&device, token, result);
                        return true;
                    };
                    if (!emit(2, reinterpret_cast<const D3DRECT*>(kRectAddress), E_FAIL) ||
                        !emit(1, nullptr, S_OK) ||
                        !emit(0, reinterpret_cast<const D3DRECT*>(kRectAddress), S_OK) ||
                        !emit(1, reinterpret_cast<const D3DRECT*>(kGuardAddress), S_OK) ||
                        !emit(257, reinterpret_cast<const D3DRECT*>(kRectAddress), S_OK) ||
                        !emit(1, reinterpret_cast<const D3DRECT*>(std::numeric_limits<uintptr_t>::max() - 1), S_OK)) return 8;
                }
                t3render::CapturePresent(&device, S_OK);
            }
        }
    } else if (scenario == "up" || scenario == "up_budget" || scenario == "up_mixed_budget") {
        t3render::CapturePresent(&device, S_OK);  // target frame 1
        auto call = up;
        call.vertex_data = reinterpret_cast<void*>(kVertexAddress);
        call.vertex_stride = 4;
        if (scenario == "up_budget" || scenario == "up_mixed_budget") {
            call.primitive_type = D3DPT_POINTLIST;
            call.primitive_count = 65536; // 256 KiB, exactly the per-component limit
            const unsigned iterations = scenario == "up_budget" ? 33 : 31;
            for (unsigned i = 0; i < iterations; ++i) SimulatedDraw(&device, call);
            if (scenario == "up_mixed_budget") {
                call = indexed_up; call.primitive_count = 1; call.num_vertices = 65534;
                call.vertex_stride = 4; call.vertex_data = reinterpret_cast<void*>(kVertexAddress);
                call.index_data = reinterpret_cast<void*>(kIndexAddress);
                SimulatedDraw(&device, call);
                call.num_vertices = 0;
                SimulatedDraw(&device, call);
            }
        } else {
            call.primitive_count = 2;
            for (unsigned topology = 1; topology <= 6; ++topology) {
                call.primitive_type = static_cast<D3DPRIMITIVETYPE>(topology);
                SimulatedDraw(&device, call);
            }
            call = indexed_up;
            call.primitive_type = D3DPT_TRIANGLELIST; call.primitive_count = 1;
            call.min_vertex_index = 2; call.num_vertices = 3; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress);
            call.index_data = reinterpret_cast<void*>(kIndexAddress);
            SimulatedDraw(&device, call);
            call.index_format = D3DFMT_INDEX32; call.min_vertex_index = 3;
            call.index_data = reinterpret_cast<void*>(kIndexAddress + 32);
            SimulatedDraw(&device, call);
            call = up; call.primitive_count = 1; call.vertex_stride = 4;
            const unsigned before_invalid_reads = memory_reads;
            call.vertex_data = nullptr; SimulatedDraw(&device, call);
            call.vertex_data = reinterpret_cast<void*>(uintptr_t(0xdeadbeef)); SimulatedDraw(&device, call);
            call.vertex_data = reinterpret_cast<void*>(kGuardAddress); SimulatedDraw(&device, call);
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress + fake_vertex.size() - 2); SimulatedDraw(&device, call);
            if (memory_reads != before_invalid_reads) return 6;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress); call.vertex_stride = 0; SimulatedDraw(&device, call);
            call.vertex_stride = 4; call.primitive_type = D3DPT_FAKE; SimulatedDraw(&device, call);
            call.primitive_type = D3DPT_TRIANGLELIST; call.primitive_count = std::numeric_limits<UINT>::max();
            call.vertex_stride = std::numeric_limits<UINT>::max(); SimulatedDraw(&device, call);
            call.primitive_count = 1; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 2); SimulatedDraw(&device, call);
            call.primitive_type = D3DPT_POINTLIST; call.primitive_count = 65537;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress); SimulatedDraw(&device, call);
            call = indexed_up; call.primitive_count = 1; call.num_vertices = 3; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress); call.index_format = D3DFMT_UNKNOWN;
            call.index_data = reinterpret_cast<void*>(kIndexAddress); SimulatedDraw(&device, call);
            call.primitive_count = 0; call.vertex_data = nullptr; call.index_data = nullptr;
            call.index_format = D3DFMT_INDEX16; call.vertex_stride = 0;
            const unsigned before_empty_reads = memory_reads;
            SimulatedDraw(&device, call);
            if (memory_reads != before_empty_reads) return 7;
            call = indexed_up; call.primitive_count = 2; call.num_vertices = 8;
            call.min_vertex_index = 2; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress);
            call.index_data = reinterpret_cast<void*>(kIndexAddress);
            for (unsigned topology = 1; topology <= 6; ++topology) {
                call.primitive_type = static_cast<D3DPRIMITIVETYPE>(topology);
                SimulatedDraw(&device, call);
            }
            call = up; call.primitive_count = 1; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress + fake_vertex.size() / 2 - 2);
            SimulatedDraw(&device, call);
        }
        t3render::CapturePresent(&device, S_OK);
    } else if (scenario == "file_release_armed" || scenario == "file_release_pending") {
        t3render::CapturePresent(&device, S_OK);
        const char* marker = std::getenv("T3RENDER_CAPTURE_TRIGGER_PATH");
        if (!marker) return 9;
        const int signal = open(marker, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (signal < 0) return 9;
        close(signal);
        if (scenario == "file_release_armed") t3render::CapturePresent(&device, S_OK);
        t3render::CaptureDeviceReleased(&device);
        t3render::CaptureDevice(&unrelated, &presentation);
        second_used = true;
        t3render::CapturePresent(&unrelated, S_OK);
        SimulatedDraw(&unrelated, draw);
        t3render::CapturePresent(&unrelated, S_OK);
    } else if (scenario == "release_before" || scenario == "release_key" || scenario == "release_key_held") {
        if (scenario != "release_before") key_down = true;
        t3render::CapturePresent(&device, S_OK);  // F11 can arm the first device.
        if (opens != 0) return 4;
        t3render::CaptureDeviceReleased(&device);
        if (scenario != "release_key_held") key_down = false;
        D3DPRESENT_PARAMETERS update = presentation;
        update.BackBufferWidth = 1600;
        update.BackBufferHeight = 900;
        t3render::CaptureDevice(&unrelated, &update);
        second_used = true;
        SimulatedDraw(&unrelated, draw);  // second device, frame 0
        t3render::CapturePresent(&unrelated, S_OK);
        if (scenario == "release_key" || scenario == "release_key_held") {
            SimulatedDraw(&unrelated, draw); // no stale F11 trigger
            if (opens != 0) return 4;
            if (scenario == "release_key_held") {
                t3render::CapturePresent(&unrelated, S_OK); // held: still no trigger
                key_down = false;
                t3render::CapturePresent(&unrelated, S_OK); // release edge
                if (opens != 0) return 4;
            }
            key_down = true;
            t3render::CapturePresent(&unrelated, S_OK); // arms the next frame
        }
        if (opens != 0) return 4;
        SimulatedDraw(&unrelated, indexed);
        t3render::CapturePresent(&unrelated, S_OK);
        if (device.getters != 1 || unrelated.getters <= 1) return 5;
    } else if (scenario == "release_open") {
        t3render::CapturePresent(&device, S_OK);  // advances to frame 1
        SimulatedDraw(&device, draw);     // opens the trace
        t3render::CaptureDeviceReleased(&device);
        t3render::CaptureDevice(&unrelated, &presentation);
        SimulatedDraw(&unrelated, indexed);
        t3render::CapturePresent(&unrelated, S_OK);
    } else if (scenario == "key") {
        SimulatedDraw(&device, draw);       // frame 0
        t3render::CapturePresent(&device, S_OK);
        SimulatedDraw(&device, draw);       // frame 1
        key_down = true;
        t3render::CapturePresent(&device, S_OK);  // arms frame 2
        if (opens != 0) return 4;                  // no file before frame 2
        SimulatedDraw(&device, indexed);
        t3render::CapturePresent(&device, S_OK);
    } else {
        SimulatedDraw(&unrelated, draw);
        t3render::CapturePresent(&unrelated, S_OK);
        if (scenario == "reset_before" || scenario == "reset_before_failure") {
            D3DPRESENT_PARAMETERS update = presentation;
            update.BackBufferWidth = 1600;
            update.BackBufferHeight = 900;
            t3render::CaptureReset(&device, &update,
                                   scenario == "reset_before" ? S_OK : E_FAIL);
        }
        SimulatedDraw(&device, draw);       // frame 0
        t3render::CapturePresent(&device, S_OK);
        if (opens != 0) return 4;                  // target is frame 1
        SimulatedDraw(&device, draw);       // frame 1
        SimulatedDraw(&device, indexed);
        SimulatedDraw(&device, up);
        SimulatedDraw(&device, indexed_up);
        t3render::CaptureReset(&device, &presentation, S_OK);
        t3render::CapturePresent(&device, -7);
    }
    SimulatedDraw(&device, draw);           // no capture after completion
    t3render::CapturePresent(&device, S_OK);
    if (bad_create_options || !device.Unchanged() || !unrelated.Unchanged() ||
        (!second_used && unrelated.getters != 0) ||
        presentation.BackBufferWidth != 1280 || presentation.BackBufferHeight != 720) {
        std::cerr << "fake device state or reference counts changed\n";
        return 3;
    }
    std::cout << "state=unchanged configured=" << configured << " getters=" << device.getters << '\n';
    return 0;
}
