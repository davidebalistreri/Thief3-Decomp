// Isolated D3D8 state-block and WRITEONLY-buffer experiment. The same binary is
// run with builtin D3D8 and with T3Render's experimental buffer proxy. No TDS
// assets, game process, or wrapper is ever passed through a raw device alias.
#include <windows.h>
#include <d3d8.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {
constexpr UINT kWidth = 256, kHeight = 256;
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
struct Vertex { float x, y, z, rhw; D3DCOLOR color; };
static_assert(sizeof(Vertex) == 20, "Unexpected XYZRHW|DIFFUSE layout");

template<class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { reset(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    void reset() { if (value) { T* old = value; value = nullptr; old->Release(); } }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

bool Check(HRESULT hr, const char* name) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "%s failed: 0x%08lx\n", name,
                 static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return false;
}

struct Pair {
    ComPtr<IDirect3DVertexBuffer8> vertex;
    ComPtr<IDirect3DIndexBuffer8> index;
};

bool CreatePair(IDirect3DDevice8* device, Pair& pair, D3DCOLOR color, bool oversized) {
    const Vertex vertices[] = {{25.6f, 217.6f, 0, 1, color}, {128, 25.6f, 0, 1, color},
                               {230.4f, 217.6f, 0, 1, color}, {1, 1, 0, 1, color}};
    const std::uint16_t indices[] = {0, 1, 2, 2};
    const UINT vertex_bytes = sizeof(Vertex) * (oversized ? 4 : 3);
    const UINT index_bytes = sizeof(std::uint16_t) * (oversized ? 4 : 3);
    if (!Check(device->CreateVertexBuffer(vertex_bytes, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED,
                                           pair.vertex.put()), "CreateVertexBuffer") || !pair.vertex.value ||
        !Check(device->CreateIndexBuffer(index_bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                                          pair.index.put()), "CreateIndexBuffer") || !pair.index.value) return false;
    BYTE* p = nullptr;
    if (!Check(pair.vertex->Lock(0, vertex_bytes, &p, 0), "VB Lock") || !p) return false;
    std::memcpy(p, vertices, vertex_bytes);
    if (!Check(pair.vertex->Unlock(), "VB Unlock")) return false;
    p = nullptr;
    if (!Check(pair.index->Lock(0, index_bytes, &p, 0), "IB Lock") || !p) return false;
    std::memcpy(p, indices, index_bytes);
    return Check(pair.index->Unlock(), "IB Unlock");
}

bool Bind(IDirect3DDevice8* device, const Pair& pair) {
    return Check(device->SetStreamSource(0, pair.vertex.value, sizeof(Vertex)), "SetStreamSource") &&
           Check(device->SetIndices(pair.index.value, 0), "SetIndices");
}

bool SameIdentity(IUnknown* first, IUnknown* second) {
    ComPtr<IUnknown> a, b;
    return first && second &&
           Check(first->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(a.put())), "QI first") &&
           Check(second->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(b.put())), "QI second") &&
           a.value && a.value == b.value;
}

bool BoundPair(IDirect3DDevice8* device, const Pair& expected, const char* marker) {
    ComPtr<IDirect3DVertexBuffer8> vertex;
    ComPtr<IDirect3DIndexBuffer8> index;
    UINT stride = 0, base = 0;
    const bool ok = Check(device->GetStreamSource(0, vertex.put(), &stride), "GetStreamSource") &&
        Check(device->GetIndices(index.put(), &base), "GetIndices") && vertex.value && index.value &&
        stride == sizeof(Vertex) && base == 0 && SameIdentity(vertex.value, expected.vertex.value) &&
        SameIdentity(index.value, expected.index.value);
    std::printf("state %s=%s\n", marker, ok ? "PASS" : "FAIL");
    return ok;
}

bool BoundOversized(IDirect3DDevice8* device) {
    ComPtr<IDirect3DVertexBuffer8> vertex;
    ComPtr<IDirect3DIndexBuffer8> index;
    UINT stride = 0, base = 0;
    D3DVERTEXBUFFER_DESC vd{};
    D3DINDEXBUFFER_DESC id{};
    const bool ok = Check(device->GetStreamSource(0, vertex.put(), &stride), "GetStreamSource retained") &&
        Check(device->GetIndices(index.put(), &base), "GetIndices retained") && vertex.value && index.value &&
        Check(vertex->GetDesc(&vd), "GetDesc retained VB") && Check(index->GetDesc(&id), "GetDesc retained IB") &&
        stride == sizeof(Vertex) && base == 0 && vd.Size == sizeof(Vertex) * 4 && id.Size == sizeof(std::uint16_t) * 4;
    std::printf("state stateblock_retains_released_buffers=%s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool ReadPpm(IDirect3DDevice8* device, std::vector<BYTE>& output) {
    ComPtr<IDirect3DSurface8> back;
    D3DSURFACE_DESC desc{};
    if (!Check(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put()), "GetBackBuffer") || !back.value ||
        !Check(back->GetDesc(&desc), "GetBackBuffer::GetDesc") || desc.Width != kWidth || desc.Height != kHeight ||
        (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)) return false;
    D3DLOCKED_RECT lock{};
    if (!Check(back->LockRect(&lock, nullptr, D3DLOCK_READONLY), "BackBuffer::LockRect")) return false;
    const bool valid = lock.pBits && lock.Pitch >= static_cast<INT>(kWidth * 4) &&
        static_cast<std::size_t>(lock.Pitch) <=
            static_cast<std::size_t>(PTRDIFF_MAX) / (kHeight - 1);
    if (valid) {
        const char header[] = "P6\n256 256\n255\n";
        output.assign(header, header + sizeof(header) - 1);
        output.reserve(output.size() + kWidth * kHeight * 3);
        for (UINT y = 0; y < kHeight; ++y) {
            const auto* row = static_cast<const BYTE*>(lock.pBits) + std::ptrdiff_t(y) * lock.Pitch;
            for (UINT x = 0; x < kWidth; ++x) {
                output.push_back(row[x * 4 + 2]); output.push_back(row[x * 4 + 1]); output.push_back(row[x * 4]);
            }
        }
    }
    if (!Check(back->UnlockRect(), "BackBuffer::UnlockRect") || !valid) return false;
    const std::size_t center = sizeof("P6\n256 256\n255\n") - 1 + (128 * kWidth + 128) * 3;
    return output[center] == 255 && output[center + 1] == 0 && output[center + 2] == 0;
}

bool WriteNew(const wchar_t* path, const std::vector<BYTE>& data) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size();
    const bool closed = CloseHandle(file) != 0;
    if (!ok || !closed) DeleteFileW(path);
    return ok && closed;
}

bool Render(HWND window, const wchar_t* output) {
    ComPtr<IDirect3D8> factory;
    factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    D3DDISPLAYMODE display{};
    if (!factory.value || !Check(factory->GetAdapterDisplayMode(0, &display), "GetAdapterDisplayMode")) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.Windowed = TRUE; pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ComPtr<IDirect3DDevice8> device;
    if (!Check(factory->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                      &pp, device.put()), "CreateDevice") || !device.value) return false;
    Pair a, b, retained;
    if (!CreatePair(device.value, a, 0xffff0000, false) || !CreatePair(device.value, b, 0xff00ff00, false) ||
        !CreatePair(device.value, retained, 0xff0000ff, true) || !Bind(device.value, a)) return false;

    DWORD all = 0;
    if (!Check(device->CreateStateBlock(D3DSBT_ALL, &all), "CreateStateBlock") || !Bind(device.value, b) ||
        !BoundPair(device.value, b, "before_apply_all") || !Check(device->ApplyStateBlock(all), "ApplyStateBlock(all)") ||
        !BoundPair(device.value, a, "apply_all_restores_a") || !Check(device->DeleteStateBlock(all), "DeleteStateBlock(all)") ||
        !BoundPair(device.value, a, "delete_preserves_bound_a")) return false;

    DWORD recorded = 0;
    if (!Check(device->BeginStateBlock(), "BeginStateBlock") || !Bind(device.value, b) ||
        !Check(device->EndStateBlock(&recorded), "EndStateBlock") ||
        !BoundPair(device.value, a, "recording_preserves_bound_a") ||
        !Check(device->ApplyStateBlock(recorded), "ApplyStateBlock(recorded)") ||
        !BoundPair(device.value, b, "recorded_restores_b") || !Bind(device.value, a) ||
        !Check(device->CaptureStateBlock(recorded), "CaptureStateBlock") || !Bind(device.value, b) ||
        !Check(device->ApplyStateBlock(recorded), "ApplyStateBlock(captured)") ||
        !BoundPair(device.value, a, "capture_restores_a") ||
        !Check(device->DeleteStateBlock(recorded), "DeleteStateBlock(recorded)")) return false;

    DWORD owned = 0;
    if (!Check(device->BeginStateBlock(), "BeginStateBlock(retained)") || !Bind(device.value, retained) ||
        !Check(device->EndStateBlock(&owned), "EndStateBlock(retained)")) return false;
    retained.vertex.reset(); retained.index.reset();
    if (!Check(device->ApplyStateBlock(owned), "ApplyStateBlock(retained)") || !BoundOversized(device.value) ||
        !Check(device->DeleteStateBlock(owned), "DeleteStateBlock(retained)") ||
        !BoundOversized(device.value) || !Bind(device.value, a)) return false;

    if (!Check(device->SetVertexShader(kFvf), "SetVertexShader") ||
        !Check(device->SetPixelShader(0), "SetPixelShader") ||
        !Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "SetRenderState(LIGHTING)") ||
        !Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "SetRenderState(CULLMODE)") ||
        !Check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "SetRenderState(ZENABLE)") ||
        !Check(device->SetTexture(0, nullptr), "SetTexture") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "COLOROP") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "COLORARG1") ||
        !Check(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE), "COLOROP(1)") ||
        !Check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1, 0), "Clear") ||
        !Check(device->BeginScene(), "BeginScene") ||
        !Check(device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 3, 0, 1), "DrawIndexedPrimitive") ||
        !Check(device->EndScene(), "EndScene")) return false;
    std::vector<BYTE> ppm;
    if (!ReadPpm(device.value, ppm) || !WriteNew(output, ppm) ||
        !Check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present")) return false;
    std::printf("state result=PASS\n");
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || std::wcscmp(argv[1], L"--output") || !argv[2][0]) return 2;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderStateBlockFixture";
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = instance; cls.lpszClassName = name;
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(name, name, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                256, 256, nullptr, nullptr, instance, nullptr);
    if (!window) { UnregisterClassW(name, instance); return 1; }
    ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
    const bool ok = Render(window, argv[2]);
    DestroyWindow(window); UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
