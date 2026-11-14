// Synthetic D3D8 declaration experiment. No game data or TDS process is used.
#include <windows.h>
#include <d3d8.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {
constexpr UINT kSize = 256;
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;
struct Vertex { float x, y, z; DWORD color; };
static_assert(sizeof(Vertex) == 16, "Unexpected fixture vertex layout");
const DWORD kDeclaration[] = {
    D3DVSD_STREAM(0), D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
    D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR), D3DVSD_END()
};
// vs_1_1: mov oPos, v0; mov oD0, v5. Register parameter tokens have bit 31 set.
const DWORD kProgram[] = {
    D3DVS_VERSION(1, 1), D3DSIO_MOV,
    0x80000000u | D3DSPR_RASTOUT | D3DSP_WRITEMASK_ALL | D3DSRO_POSITION,
    0x80000000u | D3DSPR_INPUT | D3DSP_NOSWIZZLE | D3DVSDE_POSITION,
    D3DSIO_MOV, 0x80000000u | D3DSPR_ATTROUT | D3DSP_WRITEMASK_ALL,
    0x80000000u | D3DSPR_INPUT | D3DSP_NOSWIZZLE | D3DVSDE_DIFFUSE, D3DSIO_END
};
template<class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

bool Check(HRESULT hr, const char* name) {
    std::printf("parity %s hr=%08lx\n", name, static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return SUCCEEDED(hr);
}

bool WriteNew(const wchar_t* path, const std::vector<BYTE>& bytes) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    const bool closed = CloseHandle(file) != 0;
    if (!ok || !closed) DeleteFileW(path);
    return ok && closed;
}

bool SaveImage(IDirect3DDevice8* device, const wchar_t* path) {
    ComPtr<IDirect3DSurface8> back;
    D3DSURFACE_DESC desc{};
    if (!Check(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put()), "GetBackBuffer") || !back.value ||
        !Check(back->GetDesc(&desc), "GetDesc") || desc.Width != kSize || desc.Height != kSize ||
        (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)) return false;
    D3DLOCKED_RECT lock{};
    if (!Check(back->LockRect(&lock, nullptr, D3DLOCK_READONLY), "LockRect")) return false;
    const bool valid = lock.pBits && lock.Pitch >= static_cast<INT>(kSize * 4) &&
        static_cast<std::size_t>(lock.Pitch) <= static_cast<std::size_t>(PTRDIFF_MAX) / (kSize - 1);
    std::vector<BYTE> data;
    if (valid) {
        const char header[] = "P6\n256 256\n255\n";
        data.assign(header, header + sizeof(header) - 1);
        data.reserve(data.size() + kSize * kSize * 3);
        for (UINT y = 0; y < kSize; ++y) {
            const auto* row = static_cast<const BYTE*>(lock.pBits) + std::ptrdiff_t(y) * lock.Pitch;
            for (UINT x = 0; x < kSize; ++x) {
                data.push_back(row[x * 4 + 2]); data.push_back(row[x * 4 + 1]); data.push_back(row[x * 4]);
            }
        }
    }
    return Check(back->UnlockRect(), "UnlockRect") && valid && WriteNew(path, data);
}

bool Render(HWND window, const wchar_t* path, bool programmable, bool version10) {
    ComPtr<IDirect3D8> factory;
    factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!factory.value) return false;
    D3DDISPLAYMODE display{};
    if (!Check(factory->GetAdapterDisplayMode(0, &display), "GetAdapterDisplayMode")) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kSize; pp.BackBufferHeight = kSize; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.Windowed = TRUE; pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ComPtr<IDirect3DDevice8> device;
    if (!Check(factory->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                     &pp, device.put()), "CreateDevice") || !device.value) return false;
    D3DMATRIX identity{}; identity._11 = identity._22 = identity._33 = identity._44 = 1;
    if (!Check(device->SetTransform(D3DTS_WORLD, &identity), "SetWorld") ||
        !Check(device->SetTransform(D3DTS_VIEW, &identity), "SetView") ||
        !Check(device->SetTransform(D3DTS_PROJECTION, &identity), "SetProjection") ||
        !Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "Lighting") ||
        !Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "Cull") ||
        !Check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "Depth") ||
        !Check(device->SetTexture(0, nullptr), "Texture") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "ColorOp") ||
        !Check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "ColorArg") ||
        !Check(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE), "Stage1") ||
        !Check(device->SetPixelShader(0), "PixelShader") ||
        !Check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1, 0), "Clear") ||
        !Check(device->BeginScene(), "BeginScene")) return false;
    const Vertex triangles[2][3] = {
        {{-.9f,-.8f,.5f,0xffff0000}, {-.5f,.8f,.5f,0xffff0000}, {-.1f,-.8f,.5f,0xffff0000}},
        {{ .1f,-.8f,.5f,0xff00ff00}, { .5f,.8f,.5f,0xff00ff00}, { .9f,-.8f,.5f,0xff00ff00}}
    };
    for (unsigned n = 0; n < 2; ++n) {
        DWORD shader = 0, got = 0;
        DWORD program[sizeof(kProgram) / sizeof(kProgram[0])];
        std::memcpy(program, kProgram, sizeof(program));
        if (version10) program[0] = D3DVS_VERSION(1, 0);
        const HRESULT create = device->CreateVertexShader(kDeclaration, programmable ? program : nullptr, &shader, 0);
        if (!Check(create, n ? "Create2" : "Create1") || !shader ||
            !Check(device->SetVertexShader(shader), n ? "Set2" : "Set1") ||
            !Check(device->GetVertexShader(&got), n ? "Get2" : "Get1") || got != shader) return false;
        std::printf("handle %u=%08lx\n", n, static_cast<unsigned long>(shader));
        if (!Check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangles[n], sizeof(Vertex)), n ? "Draw2" : "Draw1") ||
            !Check(device->SetVertexShader(kFvf), n ? "Restore2" : "Restore1") ||
            !Check(device->DeleteVertexShader(shader), n ? "Delete2" : "Delete1")) return false;
    }
    if (!Check(device->EndScene(), "EndScene") || !SaveImage(device.value, path) ||
        !Check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present")) return false;
    std::puts("fixture result=PASS");
    return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 5 || std::wcscmp(argv[1], L"--output") || !argv[2][0] || std::wcscmp(argv[3], L"--mode") ||
        (std::wcscmp(argv[4], L"program") && std::wcscmp(argv[4], L"program-10") &&
         std::wcscmp(argv[4], L"declaration-only"))) return 2;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderDeclarationFixture";
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = instance; cls.lpszClassName = name;
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(name, name, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                kSize, kSize, nullptr, nullptr, instance, nullptr);
    if (!window) { UnregisterClassW(name, instance); return 1; }
    ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
    const bool ok = Render(window, argv[2], std::wcscmp(argv[4], L"declaration-only") != 0,
                           !std::wcscmp(argv[4], L"program-10"));
    DestroyWindow(window); UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
