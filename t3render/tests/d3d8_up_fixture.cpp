// Synthetic single-draw D3D8 references for offline replay. No game assets
// are loaded. The camera fixture uses XYZ vertices with distinct matrices;
// the buffered fixture uses an indexed draw with nonzero offsets. Raster
// rules may differ from D3D12.
#include "d3d12_probe_pixels.hpp"
#include "d3d8_buffer_readback.hpp"

#include <windows.h>
#include <d3d8.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <vector>

namespace {
constexpr UINT kWidth = 256;
constexpr UINT kHeight = 256;
constexpr DWORD kFvfScreen = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr DWORD kFvfCamera = D3DFVF_XYZ | D3DFVF_DIFFUSE;
enum class Fixture { Screen, Camera, Buffered };

// XYZRHW is four float32 values followed by D3DCOLOR AARRGGBB (little endian
// bytes BB GG RR AA). The captured stride is 20 and there are three vertices.
struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR diffuse;
};
static_assert(sizeof(Vertex) == 20 && offsetof(Vertex, diffuse) == 16, "Unexpected D3D8 vertex layout");
struct CameraVertex {
    float x, y, z;
    D3DCOLOR diffuse;
};
static_assert(sizeof(CameraVertex) == 16 && offsetof(CameraVertex, diffuse) == 12, "Unexpected XYZ vertex layout");

template <typename T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "%s failed: HRESULT=0x%08lx\n", operation,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return false;
}

bool ReadMappedBuffer(void*, const void* source, void* destination, std::size_t length) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), source, destination, length, &copied) && copied == length;
}

bool WriteAll(HANDLE file, const void* data, DWORD size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size) {
        DWORD written = 0;
        if (!WriteFile(file, bytes, size, &written, nullptr) || !written) return false;
        bytes += written;
        size -= written;
    }
    return true;
}

bool WritePpm(const wchar_t* path, const std::vector<std::uint8_t>& rgba) {
    Handle file{CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"Cannot create %ls (error %lu; existing files are not replaced)\n",
                      path, GetLastError());
        return false;
    }
    static const char header[] = "P6\n256 256\n255\n";
    if (!WriteAll(file.value, header, static_cast<DWORD>(sizeof(header) - 1))) {
        std::fwprintf(stderr, L"Incomplete output remains at %ls\n", path);
        return false;
    }
    std::vector<std::uint8_t> row(kWidth * 3);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const std::size_t source = (std::size_t(y) * kWidth + x) * 4;
            row[x * 3] = rgba[source];
            row[x * 3 + 1] = rgba[source + 1];
            row[x * 3 + 2] = rgba[source + 2];
        }
        if (!WriteAll(file.value, row.data(), static_cast<DWORD>(row.size()))) {
            std::fwprintf(stderr, L"Incomplete output remains at %ls\n", path);
            return false;
        }
    }
    return true;
}

bool ReadBackbuffer(IDirect3DDevice8* device, std::vector<std::uint8_t>& rgba, D3DFORMAT& format) {
    ComPtr<IDirect3DSurface8> surface;
    if (!Check(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, surface.put()), "GetBackBuffer")) return false;
    D3DSURFACE_DESC desc{};
    if (!Check(surface->GetDesc(&desc), "BackBuffer::GetDesc")) return false;
    if (desc.Width != kWidth || desc.Height != kHeight ||
        (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)) {
        std::fprintf(stderr, "Unsupported backbuffer dimensions or format (%lu)\n",
                     static_cast<unsigned long>(desc.Format));
        return false;
    }
    D3DLOCKED_RECT lock{};
    if (!Check(surface->LockRect(&lock, nullptr, D3DLOCK_READONLY), "BackBuffer::LockRect")) return false;
    const bool valid = lock.pBits != nullptr && lock.Pitch >= static_cast<INT>(kWidth * 4) &&
        static_cast<std::size_t>(lock.Pitch) <=
            static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / (kHeight - 1);
    if (valid) {
        for (UINT y = 0; y < kHeight; ++y) {
            const auto* row = static_cast<const std::uint8_t*>(lock.pBits) + std::ptrdiff_t(y) * lock.Pitch;
            for (UINT x = 0; x < kWidth; ++x) {
                const std::size_t out = (std::size_t(y) * kWidth + x) * 4;
                rgba[out] = row[x * 4 + 2];
                rgba[out + 1] = row[x * 4 + 1];
                rgba[out + 2] = row[x * 4];
                rgba[out + 3] = desc.Format == D3DFMT_X8R8G8B8 ? 255 : row[x * 4 + 3];
            }
        }
    }
    if (!Check(surface->UnlockRect(), "BackBuffer::UnlockRect")) return false;
    if (!valid) { std::fprintf(stderr, "Backbuffer returned an invalid pointer or pitch\n"); return false; }
    format = desc.Format;
    return true;
}

bool Render(HWND window, const wchar_t* output, Fixture fixture, bool verify_buffer_readback) {
    const bool camera = fixture == Fixture::Camera;
    const bool buffered = fixture == Fixture::Buffered;
    ComPtr<IDirect3D8> d3d;
    d3d.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d.value) { std::fprintf(stderr, "Direct3DCreate8 failed\n"); return false; }
    D3DDISPLAYMODE display{};
    if (!Check(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display), "GetAdapterDisplayMode")) return false;
    D3DPRESENT_PARAMETERS params{};
    params.BackBufferWidth = kWidth;
    params.BackBufferHeight = kHeight;
    params.BackBufferFormat = display.Format;
    params.BackBufferCount = 1;
    params.MultiSampleType = D3DMULTISAMPLE_NONE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    params.Windowed = TRUE;
    params.EnableAutoDepthStencil = FALSE;
    params.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ComPtr<IDirect3DDevice8> device;
    if (!Check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                  D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, device.put()), "CreateDevice")) return false;

    // D3D12 NDC (-.8,-.7), (0,.8), (.8,-.7) mapped into its 256x256 viewport.
    // Deliberately retain those coordinates without assuming half-pixel parity.
    const Vertex vertices[] = {
        {25.6f, 217.6f, 0.0f, 1.0f, 0xffff0000},
        {128.0f, 25.6f, 0.0f, 1.0f, 0xffff0000},
        {230.4f, 217.6f, 0.0f, 1.0f, 0xffff0000},
    };
    // Row-vector D3D8 matrices. Rotation, translation, view scale and
    // perspective do not commute; using only identity matrices would not
    // exercise matrix order or the homogeneous divide in the replay.
    const CameraVertex camera_vertices[] = {
        {-0.7f, 0.8f, 0.0f, 0xffff0000},
        {0.8f, 0.0f, 0.2f, 0xffff0000},
        {-0.7f, -0.8f, 0.1f, 0xffff0000},
    };
    D3DMATRIX world{};
    world.m[0][1] = 1.0f; world.m[1][0] = -1.0f; world.m[2][2] = 1.0f;
    world.m[3][0] = 0.1f; world.m[3][1] = -0.05f; world.m[3][2] = 2.0f; world.m[3][3] = 1.0f;
    D3DMATRIX view{};
    view.m[0][0] = 0.9f; view.m[1][1] = 1.1f; view.m[2][2] = 1.0f;
    view.m[3][0] = -0.05f; view.m[3][1] = 0.05f; view.m[3][2] = 0.5f; view.m[3][3] = 1.0f;
    D3DMATRIX projection{};
    projection.m[0][0] = 1.8f; projection.m[1][1] = 1.8f;
    projection.m[2][2] = 8.0f / 7.5f; projection.m[2][3] = 1.0f;
    projection.m[3][2] = -4.0f / 7.5f;
    if (camera && (!Check(device->SetTransform(D3DTS_WORLD, &world), "SetTransform(WORLD)") ||
                   !Check(device->SetTransform(D3DTS_VIEW, &view), "SetTransform(VIEW)") ||
                   !Check(device->SetTransform(D3DTS_PROJECTION, &projection), "SetTransform(PROJECTION)"))) return false;

    const DWORD fvf = camera ? kFvfCamera : kFvfScreen;
    const UINT stride = camera ? sizeof(CameraVertex) : sizeof(Vertex);
    const void* vertex_data = camera ? static_cast<const void*>(camera_vertices) : static_cast<const void*>(vertices);
    // These buffers are created and partially written before the captured
    // frame. Indices 1..3 with base vertex 1 address VB vertices 2..4; the
    // first two index slots and the first vertex remain unwritten.
    ComPtr<IDirect3DVertexBuffer8> vertex_buffer;
    ComPtr<IDirect3DIndexBuffer8> index_buffer;
    if (buffered) {
        const Vertex buffered_vertices[] = {{}, vertices[0], vertices[0], vertices[1], vertices[2]};
        const std::uint16_t indices[] = {1, 2, 3};
        if (!Check(device->CreateVertexBuffer(sizeof(buffered_vertices), 0, kFvfScreen, D3DPOOL_MANAGED,
                                               vertex_buffer.put()), "CreateVertexBuffer") ||
            !Check(device->CreateIndexBuffer(5 * sizeof(std::uint16_t), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                              index_buffer.put()), "CreateIndexBuffer")) return false;
        BYTE* mapped = nullptr;
        if (!Check(vertex_buffer->Lock(sizeof(Vertex), 4 * sizeof(Vertex), &mapped, 0), "VertexBuffer::Lock")) return false;
        if (!mapped) { vertex_buffer->Unlock(); std::fprintf(stderr, "VertexBuffer::Lock returned null\n"); return false; }
        std::memcpy(mapped, buffered_vertices + 1, 4 * sizeof(Vertex));
        if (!Check(vertex_buffer->Unlock(), "VertexBuffer::Unlock")) return false;
        mapped = nullptr;
        if (!Check(index_buffer->Lock(2 * sizeof(std::uint16_t), sizeof(indices), &mapped, 0), "IndexBuffer::Lock")) return false;
        if (!mapped) { index_buffer->Unlock(); std::fprintf(stderr, "IndexBuffer::Lock returned null\n"); return false; }
        std::memcpy(mapped, indices, sizeof(indices));
        if (!Check(index_buffer->Unlock(), "IndexBuffer::Unlock")) return false;
        if (verify_buffer_readback) {
            const bool vertex_ok = d3d8_fixture::VerifyBufferRange(vertex_buffer.value, sizeof(Vertex),
                4 * sizeof(Vertex), buffered_vertices + 1, D3DLOCK_READONLY, "VertexBuffer", ReadMappedBuffer, nullptr);
            const bool index_ok = d3d8_fixture::VerifyBufferRange(index_buffer.value, 2 * sizeof(std::uint16_t),
                sizeof(indices), indices, D3DLOCK_READONLY, "IndexBuffer", ReadMappedBuffer, nullptr);
            if (!vertex_ok || !index_ok) return false;
            std::printf("buffer_readback vertex_offset=%zu vertex_bytes=%zu index_offset=%zu index_bytes=%zu result=PASS\n",
                        sizeof(Vertex), 4 * sizeof(Vertex), 2 * sizeof(std::uint16_t), sizeof(indices));
        }
        if (!Check(device->SetStreamSource(0, vertex_buffer.value, sizeof(Vertex)), "SetStreamSource") ||
            !Check(device->SetIndices(index_buffer.value, 1), "SetIndices")) return false;
    }
    if (!Check(device->SetVertexShader(fvf), "SetVertexShader(FVF)") ||
        !Check(device->SetPixelShader(0), "SetPixelShader") ||
        !Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "SetRenderState(LIGHTING)") ||
        !Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "SetRenderState(CULLMODE)") ||
        !Check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "SetRenderState(ZENABLE)") ||
        !Check(device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE), "SetRenderState(ALPHATESTENABLE)") ||
        !Check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), "SetRenderState(ALPHABLENDENABLE)") ||
        !Check(device->SetRenderState(D3DRS_DITHERENABLE, FALSE), "SetRenderState(DITHERENABLE)") ||
        !Check(device->SetRenderState(D3DRS_FOGENABLE, FALSE), "SetRenderState(FOGENABLE)") ||
        !Check(device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD), "SetRenderState(SHADEMODE)") ||
        !Check(device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE |
            D3DCOLORWRITEENABLE_ALPHA), "SetRenderState(COLORWRITEENABLE)") ||
        !Check(device->SetTexture(0, nullptr), "SetTexture(0)") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "SetTextureStageState(COLOROP)") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "SetTextureStageState(COLORARG1)") ||
        !Check(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), "SetTextureStageState(ALPHAOP)") ||
        !Check(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), "SetTextureStageState(ALPHAARG1)") ||
        !Check(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE), "SetTextureStageState(1,COLOROP)") ||
        !Check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0), "Clear") ||
        !Check(device->BeginScene(), "BeginScene")) return false;

    const bool drawn = buffered
        ? Check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 1, 3, 2, 1), "DrawIndexedPrimitive")
        : Check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertex_data, stride), "DrawPrimitiveUP");
    const bool ended = Check(device->EndScene(), "EndScene");
    if (!drawn || !ended) return false;

    std::vector<std::uint8_t> rgba(kWidth * kHeight * 4);
    D3DFORMAT format = D3DFMT_UNKNOWN;
    if (!ReadBackbuffer(device.value, rgba, format)) return false;
    d3d12_probe::PixelStats stats;
    const bool pixels_ok = camera
        ? d3d12_probe::AnalyzePixels(rgba.data(), rgba.size(), 0, kWidth * 4, kWidth, kHeight, stats) &&
          stats.center_red && stats.corners_black && stats.red_pixels > 1000 && stats.red_pixels < kWidth * kHeight
        : d3d12_probe::AnalyzeTriangle256(rgba.data(), rgba.size(), 0, kWidth * 4, stats);
    if (!pixels_ok) {
        std::fprintf(stderr, "Triangle readback did not match expected interior/background pixels\n");
        return false;
    }
    if (!Check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present")) return false;
    if (output && !WritePpm(output, rgba)) return false;
    std::printf("d3d8_up_fixture mode=%s width=%u height=%u format=%lu draws=1 stride=%u fvf=0x%08lx "
                "red_pixels=%zu fnv1a=%016llx\n", camera ? "camera" : (buffered ? "buffered" : "screen"), kWidth, kHeight,
                static_cast<unsigned long>(format), stride, static_cast<unsigned long>(fvf), stats.red_pixels,
                static_cast<unsigned long long>(stats.fnv1a));
    std::printf("result=PASS; edge pixels may differ from D3D12 due to raster/half-pixel rules\n");
    return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* output = nullptr;
    Fixture fixture = Fixture::Screen;
    bool verify_buffer_readback = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::wcscmp(argv[i], L"--camera") && fixture == Fixture::Screen) fixture = Fixture::Camera;
        else if (!std::wcscmp(argv[i], L"--buffered") && fixture == Fixture::Screen) fixture = Fixture::Buffered;
        else if (!std::wcscmp(argv[i], L"--verify-buffer-readback") && !verify_buffer_readback) verify_buffer_readback = true;
        else if (!std::wcscmp(argv[i], L"--output") && !output && i + 1 < argc && argv[i + 1][0]) output = argv[++i];
        else {
            std::fwprintf(stderr, L"usage: d3d8_up_fixture [--camera | --buffered [--verify-buffer-readback]] [--output new-image.ppm]\n");
            return 2;
        }
    }
    if (verify_buffer_readback && fixture != Fixture::Buffered) {
        std::fprintf(stderr, "--verify-buffer-readback requires --buffered\n");
        return 2;
    }
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderD3D8UPFixture";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = instance;
    window_class.lpszClassName = name;
    if (!RegisterClassW(&window_class)) {
        std::fprintf(stderr, "RegisterClassW failed: error=%lu\n", GetLastError());
        return 1;
    }
    RECT rect{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    if (!AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE)) {
        std::fprintf(stderr, "AdjustWindowRect failed: error=%lu\n", GetLastError());
        UnregisterClassW(name, instance);
        return 1;
    }
    HWND window = CreateWindowW(name, L"T3Render D3D8 UP fixture", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, instance, nullptr);
    if (!window) {
        std::fprintf(stderr, "CreateWindowW failed: error=%lu\n", GetLastError());
        UnregisterClassW(name, instance);
        return 1;
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    const bool ok = Render(window, output, fixture, verify_buffer_readback);
    DestroyWindow(window);
    UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
