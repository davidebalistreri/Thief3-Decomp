// Synthetic single-draw D3D8 reference for the first offline replay.
// No game assets are loaded. XYZRHW positions correspond to the NDC triangle
// in d3d12_offscreen.cpp; D3D8/D3D12 raster rules may differ at half-pixels.
#include "d3d12_probe_pixels.hpp"

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
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

// XYZRHW is four float32 values followed by D3DCOLOR AARRGGBB (little endian
// bytes BB GG RR AA). The captured stride is 20 and there are three vertices.
struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR diffuse;
};
static_assert(sizeof(Vertex) == 20 && offsetof(Vertex, diffuse) == 16, "Unexpected D3D8 vertex layout");

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

bool Render(HWND window, const wchar_t* output) {
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
    if (!Check(device->SetVertexShader(kFvf), "SetVertexShader(FVF)") ||
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

    const bool drawn = Check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)),
                             "DrawPrimitiveUP");
    const bool ended = Check(device->EndScene(), "EndScene");
    if (!drawn || !ended) return false;

    std::vector<std::uint8_t> rgba(kWidth * kHeight * 4);
    D3DFORMAT format = D3DFMT_UNKNOWN;
    if (!ReadBackbuffer(device.value, rgba, format)) return false;
    d3d12_probe::PixelStats stats;
    if (!d3d12_probe::AnalyzeTriangle256(rgba.data(), rgba.size(), 0, kWidth * 4, stats)) {
        std::fprintf(stderr, "Triangle readback did not match expected interior/background pixels\n");
        return false;
    }
    if (!Check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present")) return false;
    if (output && !WritePpm(output, rgba)) return false;
    std::printf("d3d8_up_fixture width=%u height=%u format=%lu draws=1 stride=%zu fvf=0x%08lx "
                "red_pixels=%zu fnv1a=%016llx\n", kWidth, kHeight, static_cast<unsigned long>(format),
                sizeof(Vertex), static_cast<unsigned long>(kFvf), stats.red_pixels,
                static_cast<unsigned long long>(stats.fnv1a));
    std::printf("result=PASS; edge pixels may differ from D3D12 due to raster/half-pixel rules\n");
    return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* output = nullptr;
    if (argc == 3 && !std::wcscmp(argv[1], L"--output")) output = argv[2];
    else if (argc != 1) {
        std::fwprintf(stderr, L"usage: d3d8_up_fixture [--output new-image.ppm]\n");
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
    const bool ok = Render(window, output);
    DestroyWindow(window);
    UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
