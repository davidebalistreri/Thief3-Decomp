// Read-only, opt-in D3D8 diagnostic snapshots. A capture is deliberately not a
// replay: resource contents and calls that do not have getters are unavailable.
#include "capture.hpp"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace t3render {
namespace {

constexpr unsigned kLightLimit = 32;
constexpr unsigned kStageLimit = 8;
constexpr unsigned kStreamLimit = 16;
constexpr unsigned kWorldLimit = 4;

// The writer owns no D3D objects; resource interfaces returned by getters are
// released immediately. All state is protected because Present can be called
// from more than one thread when a multithreaded device is used.
struct CaptureState {
    std::mutex mutex;
    bool initialized = false;
    bool enabled = false;
    bool complete = false;
    bool key_trigger = false;
    bool key_down = false;
    bool triggered = false;
    bool begun = false;
    bool had_reset = false;
    uint64_t frame = 0;
    uint64_t target = 0;
    uint64_t draws = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring path;
    IDirect3DDevice8* device = nullptr;  // identity only; never dereferenced after replacement
    D3DPRESENT_PARAMETERS presentation{};
    bool has_presentation = false;
    D3DCAPS8 caps{};
    bool has_caps = false;

    ~CaptureState() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
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

void Number(std::ostream& out, float value) {
    if (std::isfinite(value)) out << value;
    else out << "null";  // JSON does not have NaN or infinity
}

void Id(std::ostream& out, const void* pointer) {
    if (!pointer) { out << "null"; return; }
    out << '"' << "0x" << std::hex << reinterpret_cast<uintptr_t>(pointer) << std::dec << '"';
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
    if (path.empty() || (!frame.empty() && !key.empty()) || (frame.empty() && key.empty())) return;
    if (!key.empty()) {
        if (key != L"F11") return;
        s.key_trigger = true;
    } else {
        if (frame.empty() || frame[0] == L'-' || frame[0] == L'+') return;
        for (wchar_t c : frame) if (c < L'0' || c > L'9') return;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long parsed = std::wcstoull(frame.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end || parsed == std::numeric_limits<uint64_t>::max()) return;
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
        << "\"trigger\":\"" << (s.key_trigger ? "F11" : "frame") << "\","
        << "\"replayable\":false,\"limitations\":["
        << "\"resource bytes and T3 asset names are not captured\","
        << "\"UP vertex and index data are not captured\","
        << "\"draws are attempted calls; draw return values are not captured\","
        << "\"shader programs, declarations and constants are not captured\","
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
    else out << "{\"texture_blend_stages\":" << s.caps.MaxTextureBlendStages
             << ",\"simultaneous_textures\":" << s.caps.MaxSimultaneousTextures
             << ",\"streams\":" << s.caps.MaxStreams
             << ",\"active_lights\":" << s.caps.MaxActiveLights << '}';
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

// Selected D3D8 render-state identifiers. Store DWORDs raw: many are
// floats or packed colours. Failed/unsupported getters are omitted.
constexpr unsigned kRenderStates[] = {
    7, 8, 9, 14, 15, 16, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    34, 35, 36, 37, 38, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    52, 53, 54, 55, 56, 57, 58, 59, 60,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 139, 140, 141,
    142, 143, 144, 145, 146, 147, 148, 151, 152, 153, 154, 155, 156, 157,
    158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 170, 171, 172, 173
};
constexpr unsigned kStageStates[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28
};

void Draw(CaptureState& s, IDirect3DDevice8* device, const DrawCall& draw) {
    auto out = Json();
    out << "{\"event\":\"draw\",\"frame\":" << s.frame << ",\"call\":\"" << CallName(draw.kind)
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
    for (unsigned name : kRenderStates) {
        DWORD value = 0;
        if (FAILED(device->GetRenderState(static_cast<D3DRENDERSTATETYPE>(name), &value))) continue;
        if (comma) out << ',';
        comma = true;
        out << '"' << name << "\":" << value;
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
        for (unsigned name : kStageStates) {
            DWORD value = 0;
            if (FAILED(device->GetTextureStageState(stage, static_cast<D3DTEXTURESTAGESTATETYPE>(name), &value))) continue;
            if (comma) out << ',';
            comma = true;
            out << '"' << name << "\":" << value;
        }
        out << "},\"texture\":";
        ComRef<IDirect3DBaseTexture8> texture;
        const HRESULT hr = device->GetTexture(stage, &texture.value);
        Texture(out, texture.value);
        if (FAILED(hr)) out << ",\"texture_hr\":" << static_cast<int32_t>(hr);
        out << '}';
    }
    DWORD vs = 0, ps = 0;
    const HRESULT vs_hr = device->GetVertexShader(&vs), ps_hr = device->GetPixelShader(&ps);
    out << "],\"vertex_shader\":";
    if (SUCCEEDED(vs_hr)) out << vs; else out << "null";
    out << ",\"pixel_shader\":";
    if (SUCCEEDED(ps_hr)) out << ps; else out << "null";
    out << ',';
    const unsigned streams = s.has_caps ? std::min<unsigned>(s.caps.MaxStreams, kStreamLimit) : kStreamLimit;
    BufferBindings(out, device, streams);
    out << "}\n";
    if (Write(s, out.str())) ++s.draws;
}

// Captures keyboard state from the calling thread's input queue, without
// consuming a message or changing the game's input. A processed F11 key-down
// arms the *next* frame; releasing F11 rearms the edge detector.
void CheckKey(CaptureState& s) {
    BYTE keys[256]{};
    if (!GetKeyboardState(keys)) return;
    const bool down = (keys[VK_F11] & 0x80) != 0;
    if (!s.triggered && down && !s.key_down && s.frame != std::numeric_limits<uint64_t>::max()) {
        s.target = s.frame + 1;
        s.triggered = true;
    }
    s.key_down = down;
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

void CaptureDevice(IDirect3DDevice8* device, const D3DPRESENT_PARAMETERS* presentation) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        Initialize(s);
        if (!s.enabled || !device || s.device) return;
        s.device = device;
        if (presentation) { s.presentation = *presentation; s.has_presentation = true; }
        s.has_caps = SUCCEEDED(device->GetDeviceCaps(&s.caps));
    } catch (...) { Fail(); }
}

void CaptureDeviceReleased(IDirect3DDevice8* device) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        // The underlying COM object has already been destroyed: only compare
        // its address, never dereference or release it here.
        if (!s.enabled || s.complete || !device || device != s.device) return;
        if (s.begun) {
            // A partial frame is evidence, not a completed capture. Close it
            // without fabricating Present, frame_end or capture_end records.
            Stop(s);
            return;
        }
        s.device = nullptr;
        s.presentation = {};
        s.has_presentation = false;
        s.caps = {};
        s.has_caps = false;
        s.frame = 0;
        s.draws = 0;
        s.had_reset = false;
        if (s.key_trigger) {
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

void CaptureDraw(IDirect3DDevice8* device, const DrawCall& draw) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device || !s.triggered || s.frame != s.target) return;
        Begin(s);
        if (s.enabled) Draw(s, device, draw);
    } catch (...) { Fail(); }
}

void CapturePresent(IDirect3DDevice8* device, HRESULT result) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device) return;
        if (s.triggered && s.frame == s.target) {
            Begin(s);
            if (!s.enabled) return;
            auto out = Json();
            out << "{\"event\":\"frame_end\",\"frame\":" << s.frame
                << ",\"present_hr\":" << static_cast<int32_t>(result) << ",\"draws\":" << s.draws
                << ",\"had_reset\":" << (s.had_reset ? "true" : "false")
                << ",\"replayable\":false}\n";
            if (Write(s, out.str()) && Write(s, "{\"event\":\"capture_end\",\"frames\":1}\n")) {
                s.complete = true;
            }
            Stop(s);  // Closing commits the final record to the OS.
            return;
        }
        if (s.key_trigger) CheckKey(s);
        if (s.frame != std::numeric_limits<uint64_t>::max()) ++s.frame;
        else Stop(s);
    } catch (...) { Fail(); }
}

void CaptureReset(IDirect3DDevice8* device, const D3DPRESENT_PARAMETERS* presentation, HRESULT result) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device) return;
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

}  // namespace t3render
