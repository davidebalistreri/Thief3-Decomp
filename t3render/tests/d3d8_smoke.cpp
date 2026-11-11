// Synthetic D3D8 rendering test. No TDS data is used. Compare hashes only
// between runs with the same adapter, display format and runtime settings.
#include <windows.h>
#include <d3d8.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr UINT kWidth = 256;
constexpr UINT kHeight = 256;
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr D3DCOLOR kClear = 0xff111318;

struct Vertex { float x, y, z, rhw; D3DCOLOR color; };

template<typename T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

bool Check(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "%s failed: HRESULT=0x%08lx\n", operation,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return false;
}

bool HashBackbuffer(IDirect3DDevice8* device, std::uint64_t& hash, D3DFORMAT& format) {
    ComPtr<IDirect3DSurface8> surface;
    if (!Check(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, surface.put()), "GetBackBuffer")) return false;
    D3DSURFACE_DESC desc{};
    if (!Check(surface->GetDesc(&desc), "GetBackBuffer::GetDesc")) return false;
    if (desc.Width != kWidth || desc.Height != kHeight ||
        (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)) {
        std::fprintf(stderr, "Unsupported backbuffer: %lux%lu format=%lu (need 256x256 32-bit RGB)\n",
                     static_cast<unsigned long>(desc.Width), static_cast<unsigned long>(desc.Height),
                     static_cast<unsigned long>(desc.Format));
        return false;
    }
    D3DLOCKED_RECT locked{};
    if (!Check(surface->LockRect(&locked, nullptr, D3DLOCK_READONLY), "BackBuffer::LockRect")) return false;
    bool varied = false;
    std::uint32_t first = 0;
    const UINT sample_x[] = {30, 225, 30, 225};
    const UINT sample_y[] = {30, 30, 225, 160};
    const std::uint32_t expected[] = {0xe43b3b, 0x35c665, 0x438af0, 0xe6c948};
    std::uint32_t samples[4]{};
    hash = UINT64_C(14695981039346656037);
    for (UINT y = 0; y < kHeight; ++y) {
        const auto* row = static_cast<const unsigned char*>(locked.pBits) +
            static_cast<std::ptrdiff_t>(y) * locked.Pitch;
        for (UINT x = 0; x < kWidth; ++x) {
            std::uint32_t pixel;
            std::memcpy(&pixel, row + x * 4, sizeof pixel);
            if (desc.Format == D3DFMT_X8R8G8B8) pixel |= 0xff000000u; // X is undefined.
            if (x == 0 && y == 0) first = pixel;
            else if (pixel != first) varied = true;
            for (unsigned i = 0; i < 4; ++i) {
                if (x == sample_x[i] && y == sample_y[i]) samples[i] = pixel & 0xffffffu;
            }
            for (unsigned shift = 0; shift < 32; shift += 8) {
                hash ^= (pixel >> shift) & 0xffu;
                hash *= UINT64_C(1099511628211);
            }
        }
    }
    if (!Check(surface->UnlockRect(), "BackBuffer::UnlockRect")) return false;
    if (!varied) {
        std::fprintf(stderr, "Backbuffer is uniform: the test triangles were not rendered\n");
        return false;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (samples[i] != expected[i]) {
            std::fprintf(stderr, "Triangle %u sample mismatch: got 0x%06lx expected 0x%06lx\n",
                         i, static_cast<unsigned long>(samples[i]), static_cast<unsigned long>(expected[i]));
            return false;
        }
    }
    format = desc.Format;
    return true;
}

bool Render(HWND window) {
    ComPtr<IDirect3D8> d3d;
    d3d.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d.value) { std::fprintf(stderr, "Direct3DCreate8 failed\n"); return false; }
    D3DDISPLAYMODE display{};
    if (!Check(d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display), "GetAdapterDisplayMode")) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth;
    pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

    ComPtr<IDirect3DDevice8> device;
    if (!Check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put()), "CreateDevice")) return false;

    const Vertex buffered[] = {
        {12, 12, 0.5f, 1, 0xffe43b3b}, {116, 12, 0.5f, 1, 0xffe43b3b}, {12, 116, 0.5f, 1, 0xffe43b3b},
        {140, 12, 0.5f, 1, 0xff35c665}, {244, 12, 0.5f, 1, 0xff35c665}, {244, 116, 0.5f, 1, 0xff35c665},
    };
    const WORD indices[] = {3, 4, 5};
    const Vertex up[] = {
        {12, 140, 0.5f, 1, 0xff438af0}, {116, 244, 0.5f, 1, 0xff438af0}, {12, 244, 0.5f, 1, 0xff438af0},
    };
    const Vertex indexed_up[] = {
        {140, 140, 0.5f, 1, 0xffe6c948}, {244, 140, 0.5f, 1, 0xffe6c948}, {244, 244, 0.5f, 1, 0xffe6c948},
    };
    const WORD up_indices[] = {0, 1, 2};

    ComPtr<IDirect3DVertexBuffer8> vb;
    ComPtr<IDirect3DIndexBuffer8> ib;
    if (!Check(device->CreateVertexBuffer(sizeof buffered, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, vb.put()),
               "CreateVertexBuffer") ||
        !Check(device->CreateIndexBuffer(sizeof indices, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, ib.put()),
               "CreateIndexBuffer")) return false;
    BYTE* data = nullptr;
    if (!Check(vb->Lock(0, sizeof buffered, &data, 0), "VertexBuffer::Lock")) return false;
    std::memcpy(data, buffered, sizeof buffered);
    if (!Check(vb->Unlock(), "VertexBuffer::Unlock")) return false;
    data = nullptr;
    if (!Check(ib->Lock(0, sizeof indices, &data, 0), "IndexBuffer::Lock")) return false;
    std::memcpy(data, indices, sizeof indices);
    if (!Check(ib->Unlock(), "IndexBuffer::Unlock")) return false;

    if (!Check(device->SetVertexShader(kFvf), "SetVertexShader(FVF)") ||
        !Check(device->SetPixelShader(0), "SetPixelShader") ||
        !Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "SetRenderState(LIGHTING)") ||
        !Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "SetRenderState(CULLMODE)") ||
        !Check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "SetRenderState(ZENABLE)") ||
        !Check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), "SetRenderState(ALPHABLENDENABLE)") ||
        !Check(device->SetRenderState(D3DRS_DITHERENABLE, FALSE), "SetRenderState(DITHERENABLE)") ||
        !Check(device->SetRenderState(D3DRS_FOGENABLE, FALSE), "SetRenderState(FOGENABLE)") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "SetTextureStageState(COLOROP)") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "SetTextureStageState(COLORARG1)") ||
        !Check(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), "SetTextureStageState(ALPHAOP)") ||
        !Check(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), "SetTextureStageState(ALPHAARG1)") ||
        !Check(device->SetStreamSource(0, vb.value, sizeof(Vertex)), "SetStreamSource") ||
        !Check(device->SetIndices(ib.value, 0), "SetIndices") ||
        !Check(device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1.0f, 0), "Clear") ||
        !Check(device->BeginScene(), "BeginScene")) return false;

    const bool drawn =
        Check(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1), "DrawPrimitive") &&
        Check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 3, 3, 0, 1), "DrawIndexedPrimitive") &&
        Check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, up, sizeof(Vertex)), "DrawPrimitiveUP") &&
        Check(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, up_indices,
                                             D3DFMT_INDEX16, indexed_up, sizeof(Vertex)), "DrawIndexedPrimitiveUP");
    const bool ended = Check(device->EndScene(), "EndScene");
    if (!drawn || !ended) return false;

    std::uint64_t hash = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    if (!HashBackbuffer(device.value, hash, format)) return false;
    if (!Check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present")) return false;
    std::printf("d3d8_smoke width=%u height=%u format=%lu draws=4 fnv1a64=%016llx\n",
                kWidth, kHeight, static_cast<unsigned long>(format), static_cast<unsigned long long>(hash));
    return true;
}
}  // namespace

int main() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderD3D8Smoke";
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
    HWND window = CreateWindowW(name, L"T3Render D3D8 smoke", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, instance, nullptr);
    if (!window) {
        std::fprintf(stderr, "CreateWindowW failed: error=%lu\n", GetLastError());
        UnregisterClassW(name, instance);
        return 1;
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    const bool ok = Render(window);
    DestroyWindow(window);
    UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
