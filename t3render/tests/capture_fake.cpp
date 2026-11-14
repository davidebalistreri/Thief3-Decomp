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
constexpr uintptr_t kVertexAddress = 0x01000000;
constexpr uintptr_t kIndexAddress = 0x02000000;
constexpr uintptr_t kGuardAddress = 0x03000000;
constexpr uintptr_t kRectAddress = 0x04000000;
std::array<BYTE, 256 * 1024> fake_vertex{};
std::array<BYTE, 64> fake_index{};
std::array<D3DRECT, 2> fake_rectangles{{{-5, 2, 31, 42}, {50, 60, 70, 80}}};
unsigned memory_reads = 0;

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
    HRESULT GetViewport(D3DVIEWPORT8* out) override { ++getters; *out = viewport; return S_OK; }
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
    return ReadObservedVertex(context, device, buffer, offset, destination, length)
        ? t3render::buffers::ShadowReadStatus::Captured : t3render::buffers::ShadowReadStatus::Abandoned;
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
DWORD GetLastError() { return fake_last_error; }
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
    if (scenario == "experimental_header") t3render::CaptureExperimentalFacade();
    if (scenario == "experimental_repeated_header") t3render::CaptureExperimentalFacade(true);
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
    if (scenario == "shader_abandon" || scenario == "shader_abandon_all") {
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
    } else if (scenario == "shader" || scenario == "shader_program" || scenario == "shader_program_unavailable" ||
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
