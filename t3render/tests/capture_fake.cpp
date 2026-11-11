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
#include <unistd.h>

namespace {
bool key_down = false;
unsigned writes = 0;
unsigned opens = 0;
bool bad_create_options = false;
constexpr uintptr_t kVertexAddress = 0x01000000;
constexpr uintptr_t kIndexAddress = 0x02000000;
constexpr uintptr_t kGuardAddress = 0x03000000;
std::array<BYTE, 256 * 1024> fake_vertex{};
std::array<BYTE, 64> fake_index{};
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
    ULONG Release() override { return --refs; }
    HRESULT GetDesc(D3DVERTEXBUFFER_DESC* out) override {
        *out = {D3DFMT_UNKNOWN, D3DRTYPE_TEXTURE, 7, 2, 4096, 123};
        return S_OK;
    }
};
struct IndexBuffer : IDirect3DIndexBuffer8 {
    ULONG refs = 1;
    ULONG Release() override { return --refs; }
    HRESULT GetDesc(D3DINDEXBUFFER_DESC* out) override {
        *out = {D3DFMT_INDEX16, D3DRTYPE_TEXTURE, 8, 2, 1024};
        return S_OK;
    }
};

struct Device : IDirect3DDevice8 {
    Texture texture;
    VertexBuffer vertex;
    IndexBuffer index;
    unsigned getters = 0;
    DWORD render_state = 42;
    DWORD vertex_shader = 444;
    DWORD pixel_shader = 555;
    float material_power = 16;
    D3DVIEWPORT8 viewport = {1, 2, 1280, 720, 0, 1};

    HRESULT GetDeviceCaps(D3DCAPS8* out) override {
        ++getters; *out = {2, 2, 2, 4}; return S_OK;
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
    HRESULT GetVertexShader(DWORD* out) override { ++getters; *out = vertex_shader; return S_OK; }
    HRESULT GetPixelShader(DWORD* out) override { ++getters; *out = pixel_shader; return S_OK; }
    HRESULT GetStreamSource(UINT stream, IDirect3DVertexBuffer8** out, UINT* stride) override {
        ++getters; *out = nullptr; *stride = 0;
        if (stream == 0) { ++vertex.refs; *out = &vertex; *stride = 32; }
        return S_OK;
    }
    HRESULT GetIndices(IDirect3DIndexBuffer8** out, UINT* base) override {
        ++getters; ++index.refs; *out = &index; *base = 3; return S_OK;
    }
    bool Unchanged() const {
        return texture.refs == 1 && vertex.refs == 1 && index.refs == 1 && render_state == 42 &&
            vertex_shader == 444 && pixel_shader == 555 && material_power == 16 &&
            viewport.X == 1 && viewport.Y == 2 && viewport.Width == 1280 && viewport.Height == 720;
    }
};

t3render::DrawCall MakeDraw(t3render::DrawKind kind) {
    t3render::DrawCall draw{};
    draw.kind = kind;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.primitive_count = 2;
    draw.num_vertices = 6;
    draw.index_format = D3DFMT_INDEX16;
    return draw;
}
}  // namespace

DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, DWORD size) {
    const std::string narrow_name = Narrow(name);
    const char* value = std::getenv(narrow_name.c_str());
    if (!value || !*value) return 0;
    const DWORD length = static_cast<DWORD>(std::strlen(value));
    if (!buffer || size <= length) return length + 1;
    for (DWORD i = 0; i < length; ++i) buffer[i] = static_cast<unsigned char>(value[i]);
    buffer[length] = 0;
    return length;
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
    if (scenario == "up" || scenario == "up_budget" || scenario == "up_mixed_budget") {
        t3render::CapturePresent(&device, S_OK);  // target frame 1
        auto call = up;
        call.vertex_data = reinterpret_cast<void*>(kVertexAddress);
        call.vertex_stride = 4;
        if (scenario == "up_budget" || scenario == "up_mixed_budget") {
            call.primitive_type = D3DPT_POINTLIST;
            call.primitive_count = 65536; // 256 KiB, exactly the per-component limit
            const unsigned iterations = scenario == "up_budget" ? 33 : 31;
            for (unsigned i = 0; i < iterations; ++i) t3render::CaptureDraw(&device, call);
            if (scenario == "up_mixed_budget") {
                call = indexed_up; call.primitive_count = 1; call.num_vertices = 65534;
                call.vertex_stride = 4; call.vertex_data = reinterpret_cast<void*>(kVertexAddress);
                call.index_data = reinterpret_cast<void*>(kIndexAddress);
                t3render::CaptureDraw(&device, call);
                call.num_vertices = 0;
                t3render::CaptureDraw(&device, call);
            }
        } else {
            call.primitive_count = 2;
            for (unsigned topology = 1; topology <= 6; ++topology) {
                call.primitive_type = static_cast<D3DPRIMITIVETYPE>(topology);
                t3render::CaptureDraw(&device, call);
            }
            call = indexed_up;
            call.primitive_type = D3DPT_TRIANGLELIST; call.primitive_count = 1;
            call.min_vertex_index = 2; call.num_vertices = 3; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress);
            call.index_data = reinterpret_cast<void*>(kIndexAddress);
            t3render::CaptureDraw(&device, call);
            call.index_format = D3DFMT_INDEX32; call.min_vertex_index = 3;
            call.index_data = reinterpret_cast<void*>(kIndexAddress + 32);
            t3render::CaptureDraw(&device, call);
            call = up; call.primitive_count = 1; call.vertex_stride = 4;
            const unsigned before_invalid_reads = memory_reads;
            call.vertex_data = nullptr; t3render::CaptureDraw(&device, call);
            call.vertex_data = reinterpret_cast<void*>(uintptr_t(0xdeadbeef)); t3render::CaptureDraw(&device, call);
            call.vertex_data = reinterpret_cast<void*>(kGuardAddress); t3render::CaptureDraw(&device, call);
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress + fake_vertex.size() - 2); t3render::CaptureDraw(&device, call);
            if (memory_reads != before_invalid_reads) return 6;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress); call.vertex_stride = 0; t3render::CaptureDraw(&device, call);
            call.vertex_stride = 4; call.primitive_type = D3DPT_FAKE; t3render::CaptureDraw(&device, call);
            call.primitive_type = D3DPT_TRIANGLELIST; call.primitive_count = std::numeric_limits<UINT>::max();
            call.vertex_stride = std::numeric_limits<UINT>::max(); t3render::CaptureDraw(&device, call);
            call.primitive_count = 1; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 2); t3render::CaptureDraw(&device, call);
            call.primitive_type = D3DPT_POINTLIST; call.primitive_count = 65537;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress); t3render::CaptureDraw(&device, call);
            call = indexed_up; call.primitive_count = 1; call.num_vertices = 3; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress); call.index_format = D3DFMT_UNKNOWN;
            call.index_data = reinterpret_cast<void*>(kIndexAddress); t3render::CaptureDraw(&device, call);
            call.primitive_count = 0; call.vertex_data = nullptr; call.index_data = nullptr;
            call.index_format = D3DFMT_INDEX16; call.vertex_stride = 0;
            const unsigned before_empty_reads = memory_reads;
            t3render::CaptureDraw(&device, call);
            if (memory_reads != before_empty_reads) return 7;
            call = indexed_up; call.primitive_count = 2; call.num_vertices = 8;
            call.min_vertex_index = 2; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress);
            call.index_data = reinterpret_cast<void*>(kIndexAddress);
            for (unsigned topology = 1; topology <= 6; ++topology) {
                call.primitive_type = static_cast<D3DPRIMITIVETYPE>(topology);
                t3render::CaptureDraw(&device, call);
            }
            call = up; call.primitive_count = 1; call.vertex_stride = 4;
            call.vertex_data = reinterpret_cast<void*>(kVertexAddress + fake_vertex.size() / 2 - 2);
            t3render::CaptureDraw(&device, call);
        }
        t3render::CapturePresent(&device, S_OK);
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
        t3render::CaptureDraw(&unrelated, draw);  // second device, frame 0
        t3render::CapturePresent(&unrelated, S_OK);
        if (scenario == "release_key" || scenario == "release_key_held") {
            t3render::CaptureDraw(&unrelated, draw); // no stale F11 trigger
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
        t3render::CaptureDraw(&unrelated, indexed);
        t3render::CapturePresent(&unrelated, S_OK);
        if (device.getters != 1 || unrelated.getters <= 1) return 5;
    } else if (scenario == "release_open") {
        t3render::CapturePresent(&device, S_OK);  // advances to frame 1
        t3render::CaptureDraw(&device, draw);     // opens the trace
        t3render::CaptureDeviceReleased(&device);
        t3render::CaptureDevice(&unrelated, &presentation);
        t3render::CaptureDraw(&unrelated, indexed);
        t3render::CapturePresent(&unrelated, S_OK);
    } else if (scenario == "key") {
        t3render::CaptureDraw(&device, draw);       // frame 0
        t3render::CapturePresent(&device, S_OK);
        t3render::CaptureDraw(&device, draw);       // frame 1
        key_down = true;
        t3render::CapturePresent(&device, S_OK);  // arms frame 2
        if (opens != 0) return 4;                  // no file before frame 2
        t3render::CaptureDraw(&device, indexed);
        t3render::CapturePresent(&device, S_OK);
    } else {
        t3render::CaptureDraw(&unrelated, draw);
        t3render::CapturePresent(&unrelated, S_OK);
        if (scenario == "reset_before" || scenario == "reset_before_failure") {
            D3DPRESENT_PARAMETERS update = presentation;
            update.BackBufferWidth = 1600;
            update.BackBufferHeight = 900;
            t3render::CaptureReset(&device, &update,
                                   scenario == "reset_before" ? S_OK : E_FAIL);
        }
        t3render::CaptureDraw(&device, draw);       // frame 0
        t3render::CapturePresent(&device, S_OK);
        if (opens != 0) return 4;                  // target is frame 1
        t3render::CaptureDraw(&device, draw);       // frame 1
        t3render::CaptureDraw(&device, indexed);
        t3render::CaptureDraw(&device, up);
        t3render::CaptureDraw(&device, indexed_up);
        t3render::CaptureReset(&device, &presentation, S_OK);
        t3render::CapturePresent(&device, -7);
    }
    t3render::CaptureDraw(&device, draw);           // no capture after completion
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
