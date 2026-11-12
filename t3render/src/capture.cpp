// Read-only, opt-in D3D8 diagnostic snapshots. A capture is deliberately not a
// replay: resource contents and calls that do not have getters are unavailable.
#include "capture.hpp"

#include <windows.h>

#include <algorithm>
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
constexpr DWORD kClearRectLimit = 256;
constexpr size_t kPendingOperationLimit = 4096;
enum class OperationKind { Draw, Clear };

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
    bool buffer_requested = false;
    bool begun = false;
    bool had_reset = false;
    uint64_t frame = 0;
    uint64_t target = 0;
    uint64_t draws = 0;
    uint64_t up_bytes = 0;  // successfully emitted raw bytes, not hex characters
    uint64_t buffer_bytes = 0;
    uint64_t next_operation = 1;
    bool concurrent_operations = false;
    std::unordered_map<uint64_t, OperationKind> pending_operations;
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
    s.buffer_requested = Environment(L"T3RENDER_CAPTURE_BUFFERS") == L"1";
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
        << "\"operation_results\":1,\"clear_rect_limit\":" << kClearRectLimit << ','
        << "\"replayable\":false,\"up_capture\":{\"encoding\":\"hex\",\"component_limit_bytes\":"
        << kUpComponentLimit << ",\"frame_limit_bytes\":" << kUpFrameLimit << "},"
        << "\"buffer_capture\":{\"requested\":" << (s.buffer_requested ? "true" : "false")
        << ",\"encoding\":\"hex\",\"component_limit_bytes\":" << kBufferComponentLimit
        << ",\"frame_limit_bytes\":" << kBufferFrameLimit << "},\"limitations\":["
        << "\"texture bytes and T3 asset names are not captured\","
        << "\"optional buffer shadows are bounded observations, not proof of driver input\","
        << "\"UP data is bounded, may be unavailable, and may change between snapshot and draw\","
        << "\"indexed UP vertex window uses MinVertexIndex times stride; nonzero behavior is not runtime verified\","
        << "\"draw and clear events describe attempts; results are paired by op_id\","
        << "\"concurrent device calls may have a different driver submission order\","
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
}

// Read only from already-tracked CPU shadows. No additional D3D Lock occurs.
// These are pre-draw observations, not a guarantee about driver input: the
// paired result reports whether the intercepted draw window stayed stable.
template<typename Resource, typename Reader>
UpBytes ReadShadow(CaptureState& s, const BufferObserver& observer, IDirect3DDevice8* device,
                   Resource* resource, Reader reader, uint64_t offset, uint64_t count) {
    UpBytes result;
    result.byte_count = count;
    result.byte_count_known = true;
    if (!observer.valid) { result.status = "window_unavailable"; return result; }
    if (!reader) { result.status = "unknown"; return result; }
    if (!count) { result.status = "empty"; return result; }
    if (count > kBufferComponentLimit) { result.status = "per_draw_limit"; return result; }
    if (count > kBufferFrameLimit - s.buffer_bytes) { result.status = "frame_limit"; return result; }
    if (offset > std::numeric_limits<size_t>::max() || count > std::numeric_limits<size_t>::max()) {
        result.status = "overflow"; return result;
    }
    const size_t size = static_cast<size_t>(count);
    std::vector<BYTE> copy(size);
    if (!reader(observer.context, device, resource, static_cast<size_t>(offset), copy.data(), size)) {
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
    ComRef<IDirect3DVertexBuffer8> vb;
    ComRef<IDirect3DIndexBuffer8> ib;
    UINT stride = 0, base = 0;
    const HRESULT vertex_hr = device->GetStreamSource(0, &vb.value, &stride);
    HRESULT index_hr = E_FAIL;
    if (indexed) index_hr = device->GetIndices(&ib.value, &base);

    uint64_t elements = 0;
    const bool elements_known = PrimitiveElements(draw.primitive_type, draw.primitive_count, elements);
    uint64_t first = 0, vertex_count = 0, vertex_offset = 0, vertex_size = 0;
    const bool first_known = !indexed || (SUCCEEDED(index_hr) && ib.value);
    if (first_known) first = indexed ? uint64_t(base) + draw.min_vertex_index : draw.start_vertex;
    const bool vertex_count_known = indexed || elements_known;
    if (vertex_count_known) vertex_count = indexed ? (draw.primitive_count ? draw.num_vertices : 0) : elements;
    const bool stride_known = SUCCEEDED(vertex_hr) && vb.value;
    const bool offset_known = first_known && stride_known && Product(first, stride, vertex_offset);
    const bool vertex_size_known = vertex_count_known && stride_known && Product(vertex_count, stride, vertex_size);
    UpBytes vertex;
    if (FAILED(vertex_hr)) vertex.status = "getter_failed";
    else if (!vb.value) vertex.status = "unbound";
    else if (!first_known || !vertex_count_known) vertex.status = "unknown_range";
    else if (!offset_known || !vertex_size_known || first > UINT64_C(0xffffffff) ||
             vertex_count > UINT64_C(0x100000000) - first) vertex.status = "overflow";
    else if (vertex_count && !stride) vertex.status = "invalid_stride";
    else {
        D3DVERTEXBUFFER_DESC desc{};
        if (FAILED(vb.value->GetDesc(&desc))) vertex.status = "descriptor_failed";
        else if (vertex_offset > desc.Size || vertex_size > uint64_t(desc.Size) - vertex_offset)
            vertex.status = "out_of_bounds";
        else vertex = ReadShadow(s, observer, device, vb.value, observer.read_vertex, vertex_offset, vertex_size);
    }

    uint64_t index_offset = 0, index_size = 0;
    bool index_offset_known = false;
    unsigned index_format = 0;
    bool index_format_known = false;
    UpBytes index;
    if (indexed) {
        if (FAILED(index_hr)) index.status = "getter_failed";
        else if (!ib.value) index.status = "unbound";
        else {
            D3DINDEXBUFFER_DESC desc{};
            if (FAILED(ib.value->GetDesc(&desc))) index.status = "descriptor_failed";
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
                    else index = ReadShadow(s, observer, device, ib.value, observer.read_index, index_offset, index_size);
                }
            }
        }
    }

    out << ",\"buffer_data\":{\"source\":\"application_lock_shadow\",\"vertex\":{";
    BytesJson(out, vertex);
    out << ",\"resource\":"; Id(out, vb.value);
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
        out << ",\"resource\":"; Id(out, ib.value);
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

bool Draw(CaptureState& s, IDirect3DDevice8* device, const DrawCall& draw, uint64_t operation) {
    const bool up = draw.kind == DrawKind::PrimitiveUP || draw.kind == DrawKind::IndexedPrimitiveUP;
    UpBytes vertex, index;
    uint64_t vertex_count = 0, vertex_offset = 0, index_count = 0;
    bool vertex_count_known = false, offset_known = false, index_count_known = false;
    if (up) CaptureUp(s, draw, vertex, index, vertex_count, vertex_offset, index_count,
                      vertex_count_known, offset_known, index_count_known);
    auto out = Json();
    out << "{\"event\":\"draw\",\"frame\":" << s.frame << ",\"op_id\":" << operation
        << ",\"call\":\"" << CallName(draw.kind)
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
    const HRESULT vs_hr = device->GetVertexShader(&vs), ps_hr = device->GetPixelShader(&ps);
    out << "],\"vertex_shader\":";
    if (SUCCEEDED(vs_hr)) out << vs; else out << "null";
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
    if (!s.pending_operations.empty()) s.concurrent_operations = true;
    const uint64_t token = s.next_operation++;
    s.pending_operations.emplace(token, kind);
    return token;
}

void Fail() noexcept;

void FinishOperation(IDirect3DDevice8* device, uint64_t token, HRESULT result, OperationKind kind,
                     int buffer_stable = -1) {
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
        s.up_bytes = 0;
        s.buffer_bytes = 0;
        s.had_reset = false;
        s.next_operation = 1;
        s.pending_operations.clear();
        s.concurrent_operations = false;
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

uint64_t CaptureDraw(IDirect3DDevice8* device, const DrawCall& draw) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device || !s.triggered || s.frame != s.target) return 0;
        Begin(s);
        if (!s.enabled) return 0;
        const uint64_t token = BeginOperation(s, OperationKind::Draw);
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

void CaptureClearResult(IDirect3DDevice8* device, uint64_t token, HRESULT result) {
    FinishOperation(device, token, result, OperationKind::Clear);
}

void CapturePresent(IDirect3DDevice8* device, HRESULT result) {
    try {
        auto& s = State();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled || s.complete || !device || device != s.device) return;
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
            auto out = Json();
            out << "{\"event\":\"frame_end\",\"frame\":" << s.frame
                << ",\"present_hr\":" << static_cast<int32_t>(result) << ",\"draws\":" << s.draws
                << ",\"had_reset\":" << (s.had_reset ? "true" : "false")
                << ",\"concurrent_operations\":" << (s.concurrent_operations ? "true" : "false")
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
