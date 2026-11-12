// Isolated native/facade D3D8 fixture. Run the two modes in separate
// processes, compare their PPM bytes and lines beginning "parity ".
// Resources are native and their GetDevice leaks the native device; no wrapped
// buffers are passed to that escaped device. This is not a TDS test.
#include "d3d8_device_facade.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <vector>

namespace {
constexpr UINT kWidth = 256, kHeight = 256;
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr D3DCOLOR kClear = 0xff111318;
struct Vertex { float x, y, z, rhw; D3DCOLOR color; };

template<class T> struct ComPtr {
    T* value = nullptr;
    ~ComPtr() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

bool Call(const char* name, HRESULT result) {
    std::printf("parity %s=0x%08lx\n", name,
                static_cast<unsigned long>(static_cast<std::uint32_t>(result)));
    return SUCCEEDED(result);
}
bool Identity(IUnknown* object, ComPtr<IUnknown>& output) {
    return object && SUCCEEDED(object->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(output.put()))) && output.value;
}
bool EqualIdentity(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> first, second;
    return Identity(a, first) && Identity(b, second) && first.value == second.value;
}
void PrintMatrix(const char* name, const D3DMATRIX& matrix) {
    std::uint32_t bits[16]{};
    static_assert(sizeof(bits) == sizeof(matrix), "unexpected D3DMATRIX size");
    std::memcpy(bits, &matrix, sizeof(bits));
    std::printf("parity %s=", name);
    for (const auto value : bits) std::printf("%08lx", static_cast<unsigned long>(value));
    std::printf("\n");
}

template<class Resource> bool CheckEscape(const char* label, Resource* resource, IDirect3DDevice8* facade,
                                          IUnknown* native_device_identity) {
    ComPtr<IDirect3DDevice8> escaped;
    const HRESULT hr = resource->GetDevice(escaped.put());
    ComPtr<IUnknown> identity;
    const bool ok = SUCCEEDED(hr) && escaped.value && escaped.value != facade && Identity(escaped.value, identity) &&
                    identity.value == native_device_identity && !EqualIdentity(escaped.value, facade);
    std::printf("resource_%s_native_device_alias=%s hr=0x%08lx\n", label, ok ? "PASS" : "FAIL",
                static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return ok;
}

bool CheckNativeEscape(IDirect3DDevice8* facade, IDirect3D8* native_factory, IDirect3DSurface8* surface,
                       IDirect3DTexture8* texture, IDirect3DVertexBuffer8* vb, IDirect3DIndexBuffer8* ib) {
    ComPtr<IDirect3DDevice8> escaped;
    if (FAILED(surface->GetDevice(escaped.put())) || !escaped.value || escaped.value == facade) return false;
    ComPtr<IUnknown> native_identity;
    if (!Identity(escaped.value, native_identity) || EqualIdentity(escaped.value, facade)) return false;
    ComPtr<IDirect3D8> escaped_factory;
    if (FAILED(escaped->GetDirect3D(escaped_factory.put())) || !escaped_factory.value ||
        !EqualIdentity(escaped_factory.value, native_factory)) return false;
    if (!CheckEscape("surface", surface, facade, native_identity.value) ||
        !CheckEscape("texture", texture, facade, native_identity.value) ||
        !CheckEscape("vertex_buffer", vb, facade, native_identity.value) ||
        !CheckEscape("index_buffer", ib, facade, native_identity.value)) return false;
    std::printf("resource_get_device_boundary=native_device; wrapped_buffers=not_tested\n");
    return true;
}

bool SetAndReadState(IDirect3DDevice8* device) {
    D3DMATRIX matrix{};
    matrix._11 = matrix._22 = matrix._33 = matrix._44 = 1.0f;
    const D3DTRANSFORMSTATETYPE kinds[] = {D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION};
    const char* set_names[] = {"SetTransformWorld", "SetTransformView", "SetTransformProjection"};
    const char* get_names[] = {"GetTransformWorld", "GetTransformView", "GetTransformProjection"};
    const char* value_names[] = {"World", "View", "Projection"};
    for (unsigned i = 0; i < 3; ++i) {
        matrix._41 = static_cast<float>(i + 1);
        if (!Call(set_names[i], device->SetTransform(kinds[i], &matrix))) return false;
        D3DMATRIX observed{};
        if (!Call(get_names[i], device->GetTransform(kinds[i], &observed)) ||
            std::memcmp(&observed, &matrix, sizeof(matrix))) return false;
        PrintMatrix(value_names[i], observed);
    }
    D3DVIEWPORT8 viewport{0, 0, kWidth, kHeight, 0.0f, 1.0f};
    if (!Call("SetViewport", device->SetViewport(&viewport))) return false;
    D3DVIEWPORT8 observed{};
    if (!Call("GetViewport", device->GetViewport(&observed)) || observed.X != viewport.X || observed.Y != viewport.Y ||
        observed.Width != viewport.Width || observed.Height != viewport.Height ||
        observed.MinZ != viewport.MinZ || observed.MaxZ != viewport.MaxZ) return false;
    std::printf("parity Viewport=%lu,%lu,%lu,%lu,%.1f,%.1f\n", static_cast<unsigned long>(observed.X),
                static_cast<unsigned long>(observed.Y), static_cast<unsigned long>(observed.Width),
                static_cast<unsigned long>(observed.Height), observed.MinZ, observed.MaxZ);
    struct RenderState { D3DRENDERSTATETYPE type; DWORD value; const char* name; };
    const RenderState states[] = {{D3DRS_LIGHTING, FALSE, "Lighting"}, {D3DRS_CULLMODE, D3DCULL_NONE, "Cull"},
                                  {D3DRS_ZENABLE, FALSE, "ZEnable"}, {D3DRS_ALPHABLENDENABLE, FALSE, "AlphaBlend"},
                                  {D3DRS_DITHERENABLE, FALSE, "Dither"}, {D3DRS_FOGENABLE, FALSE, "Fog"}};
    for (const auto& state : states) {
        char set_name[64]{}, get_name[64]{};
        std::snprintf(set_name, sizeof(set_name), "SetRender%s", state.name);
        std::snprintf(get_name, sizeof(get_name), "GetRender%s", state.name);
        DWORD value = 0;
        if (!Call(set_name, device->SetRenderState(state.type, state.value)) ||
            !Call(get_name, device->GetRenderState(state.type, &value)) || value != state.value) return false;
        std::printf("parity Render%s=%lu\n", state.name, static_cast<unsigned long>(value));
    }
    struct StageState { D3DTEXTURESTAGESTATETYPE type; DWORD value; const char* name; };
    const StageState stages[] = {{D3DTSS_COLOROP, D3DTOP_SELECTARG1, "ColorOp"},
                                 {D3DTSS_COLORARG1, D3DTA_DIFFUSE, "ColorArg1"},
                                 {D3DTSS_ALPHAOP, D3DTOP_SELECTARG1, "AlphaOp"},
                                 {D3DTSS_ALPHAARG1, D3DTA_DIFFUSE, "AlphaArg1"}};
    for (const auto& state : stages) {
        char set_name[64]{}, get_name[64]{};
        std::snprintf(set_name, sizeof(set_name), "SetStage%s", state.name);
        std::snprintf(get_name, sizeof(get_name), "GetStage%s", state.name);
        DWORD value = 0;
        if (!Call(set_name, device->SetTextureStageState(0, state.type, state.value)) ||
            !Call(get_name, device->GetTextureStageState(0, state.type, &value)) || value != state.value) return false;
        std::printf("parity Stage%s=%lu\n", state.name, static_cast<unsigned long>(value));
    }
    DWORD fvf = 0;
    if (!Call("SetVertexShader", device->SetVertexShader(kFvf)) ||
        !Call("GetVertexShader", device->GetVertexShader(&fvf)) || fvf != kFvf ||
        !Call("SetPixelShader", device->SetPixelShader(0))) return false;
    std::printf("parity FVF=%lu\n", static_cast<unsigned long>(fvf));
    return true;
}

bool ReadBack(IDirect3DSurface8* surface, std::vector<BYTE>& ppm) {
    D3DSURFACE_DESC desc{};
    if (!Call("BackBufferDesc", surface->GetDesc(&desc)) || desc.Width != kWidth || desc.Height != kHeight ||
        (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)) return false;
    D3DLOCKED_RECT lock{};
    if (!Call("BackBufferLock", surface->LockRect(&lock, nullptr, D3DLOCK_READONLY))) return false;
    constexpr std::size_t row_bytes = kWidth * 4u;
    const auto max_offset = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    bool valid = lock.pBits && lock.Pitch >= static_cast<INT>(row_bytes);
    if (valid) {
        const auto pitch = static_cast<std::size_t>(lock.Pitch);
        valid = pitch <= (max_offset - row_bytes) / (kHeight - 1);
        if (valid) {
            const auto extent = pitch * (kHeight - 1) + row_bytes;
            valid = reinterpret_cast<std::uintptr_t>(lock.pBits) <=
                    std::numeric_limits<std::uintptr_t>::max() - extent;
        }
    }
    if (!valid) {
        Call("BackBufferUnlock", surface->UnlockRect());
        std::fprintf(stderr, "Backbuffer returned an invalid pointer or pitch\n");
        return false;
    }
    const char header[] = "P6\n256 256\n255\n";
    ppm.assign(header, header + sizeof(header) - 1);
    ppm.reserve(ppm.size() + kWidth * kHeight * 3);
    const UINT sample_x[] = {30, 225, 30, 225};
    const UINT sample_y[] = {30, 30, 225, 160};
    const std::uint32_t expected[] = {0xe43b3b, 0x35c665, 0x438af0, 0xe6c948};
    std::uint32_t samples[4]{};
    for (UINT y = 0; y < kHeight; ++y) {
        const auto* row = static_cast<const BYTE*>(lock.pBits) + static_cast<std::ptrdiff_t>(y) * lock.Pitch;
        for (UINT x = 0; x < kWidth; ++x) {
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, row + x * 4, sizeof(pixel));
            for (unsigned i = 0; i < 4; ++i) if (x == sample_x[i] && y == sample_y[i]) samples[i] = pixel & 0xffffffu;
            ppm.push_back(static_cast<BYTE>(pixel >> 16));
            ppm.push_back(static_cast<BYTE>(pixel >> 8));
            ppm.push_back(static_cast<BYTE>(pixel));
        }
    }
    if (!Call("BackBufferUnlock", surface->UnlockRect())) return false;
    for (unsigned i = 0; i < 4; ++i) {
        if (samples[i] != expected[i]) {
            std::fprintf(stderr, "triangle %u: expected 0x%06lx, got 0x%06lx\n", i,
                         static_cast<unsigned long>(expected[i]), static_cast<unsigned long>(samples[i]));
            return false;
        }
    }
    std::printf("parity BackBufferFormat=%lu\n", static_cast<unsigned long>(desc.Format));
    return true;
}

bool WriteNew(const wchar_t* path, const std::vector<BYTE>& bytes) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "CreateFile failed: %lu\n", GetLastError()); return false; }
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size();
    const bool closed = CloseHandle(file) != 0;
    if (!ok || !closed) DeleteFileW(path);
    return ok && closed;
}

bool Render(HWND window, bool facade_mode, const wchar_t* output) {
    ComPtr<IDirect3D8> native_factory;
    native_factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    if (!native_factory.value) return false;
    D3DDISPLAYMODE display{};
    if (!Call("GetAdapterDisplayMode", native_factory->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &display))) return false;
    ComPtr<IDirect3D8> facade_factory;
    IDirect3D8* selected = native_factory.value;
    if (facade_mode) {
        native_factory->AddRef();
        facade_factory.value = t3render::fixture::CreateDeviceFacadeFactory(native_factory.value);
        if (!facade_factory.value) { native_factory->Release(); return false; }
        selected = facade_factory.value;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kWidth; pp.BackBufferHeight = kHeight;
    pp.BackBufferFormat = display.Format; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window; pp.Windowed = TRUE;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ComPtr<IDirect3DDevice8> device;
    if (!Call("CreateDevice", selected->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
              D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, device.put())) || !device.value) return false;
    IDirect3D8* expected_factory = selected;
    if (facade_mode) { facade_factory->Release(); facade_factory.value = nullptr; }
    ComPtr<IDirect3D8> returned_factory;
    if (!Call("GetDirect3D", device->GetDirect3D(returned_factory.put())) || !returned_factory.value ||
        !EqualIdentity(returned_factory.value, expected_factory) ||
        (facade_mode && returned_factory.value != expected_factory)) return false;
    ComPtr<IUnknown> factory_unknown, device_unknown;
    ComPtr<IDirect3D8> factory_roundtrip;
    ComPtr<IDirect3DDevice8> device_roundtrip;
    if (!Call("FactoryQIUnknown", returned_factory->QueryInterface(IID_IUnknown,
              reinterpret_cast<void**>(factory_unknown.put()))) || !factory_unknown.value ||
        !Call("FactoryQIRoundtrip", factory_unknown->QueryInterface(IID_IDirect3D8,
              reinterpret_cast<void**>(factory_roundtrip.put()))) ||
        !factory_roundtrip.value || !EqualIdentity(factory_roundtrip.value, returned_factory.value) ||
        (facade_mode && factory_roundtrip.value != returned_factory.value) ||
        !Call("DeviceQIUnknown", device->QueryInterface(IID_IUnknown,
              reinterpret_cast<void**>(device_unknown.put()))) || !device_unknown.value ||
        !Call("DeviceQIRoundtrip", device_unknown->QueryInterface(IID_IDirect3DDevice8,
              reinterpret_cast<void**>(device_roundtrip.put()))) ||
        !device_roundtrip.value || !EqualIdentity(device_roundtrip.value, device.value) ||
        (facade_mode && device_roundtrip.value != device.value)) return false;
    std::printf("factory_and_device_QI_roundtrip=PASS factory_initial_release=%s\n", facade_mode ? "PASS" : "not_applicable");

    const Vertex buffered[] = {
        {12, 12, 0.5f, 1, 0xffe43b3b}, {116, 12, 0.5f, 1, 0xffe43b3b}, {12, 116, 0.5f, 1, 0xffe43b3b},
        {140, 12, 0.5f, 1, 0xff35c665}, {244, 12, 0.5f, 1, 0xff35c665}, {244, 116, 0.5f, 1, 0xff35c665},
    };
    const WORD indices[] = {3, 4, 5};
    const Vertex up[] = {{12, 140, 0.5f, 1, 0xff438af0}, {116, 244, 0.5f, 1, 0xff438af0}, {12, 244, 0.5f, 1, 0xff438af0}};
    const Vertex indexed_up[] = {{140, 140, 0.5f, 1, 0xffe6c948}, {244, 140, 0.5f, 1, 0xffe6c948}, {244, 244, 0.5f, 1, 0xffe6c948}};
    const WORD up_indices[] = {0, 1, 2};
    ComPtr<IDirect3DVertexBuffer8> vb;
    ComPtr<IDirect3DIndexBuffer8> ib;
    ComPtr<IDirect3DTexture8> texture;
    ComPtr<IDirect3DSurface8> back;
    if (!Call("CreateVertexBuffer", device->CreateVertexBuffer(sizeof(buffered), D3DUSAGE_WRITEONLY, kFvf,
                                                                  D3DPOOL_MANAGED, vb.put())) || !vb.value ||
        !Call("CreateIndexBuffer", device->CreateIndexBuffer(sizeof(indices), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                                               D3DPOOL_MANAGED, ib.put())) || !ib.value ||
        !Call("CreateTexture", device->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                                       texture.put())) || !texture.value ||
        !Call("GetBackBuffer", device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, back.put())) || !back.value) return false;
    if (facade_mode && !CheckNativeEscape(device.value, native_factory.value, back.value, texture.value, vb.value, ib.value)) return false;
    BYTE* data = nullptr;
    if (!Call("VertexLock", vb->Lock(0, sizeof(buffered), &data, 0)) || !data) return false;
    std::memcpy(data, buffered, sizeof(buffered));
    if (!Call("VertexUnlock", vb->Unlock())) return false;
    data = nullptr;
    if (!Call("IndexLock", ib->Lock(0, sizeof(indices), &data, 0)) || !data) return false;
    std::memcpy(data, indices, sizeof(indices));
    if (!Call("IndexUnlock", ib->Unlock()) || !SetAndReadState(device.value)) return false;
    if (!Call("SetTexture", device->SetTexture(0, texture.value))) return false;
    ComPtr<IDirect3DBaseTexture8> read_texture;
    if (!Call("GetTexture", device->GetTexture(0, read_texture.put())) || !read_texture.value ||
        !EqualIdentity(texture.value, read_texture.value) || !Call("ClearTexture", device->SetTexture(0, nullptr))) return false;
    if (!Call("SetStreamSource", device->SetStreamSource(0, vb.value, sizeof(Vertex))) ||
        !Call("SetIndices", device->SetIndices(ib.value, 0))) return false;
    ComPtr<IDirect3DVertexBuffer8> read_vb;
    ComPtr<IDirect3DIndexBuffer8> read_ib;
    UINT stride = 0, base = 0;
    if (!Call("GetStreamSource", device->GetStreamSource(0, read_vb.put(), &stride)) || !read_vb.value ||
        !EqualIdentity(vb.value, read_vb.value) || stride != sizeof(Vertex) ||
        !Call("GetIndices", device->GetIndices(read_ib.put(), &base)) || !read_ib.value ||
        !EqualIdentity(ib.value, read_ib.value) || base != 0) return false;
    std::printf("parity Bindings stride=%u base=%u\n", stride, base);
    if (!Call("Clear", device->Clear(0, nullptr, D3DCLEAR_TARGET, kClear, 1, 0)) ||
        !Call("BeginScene", device->BeginScene())) return false;
    const bool drawn = Call("DrawPrimitive", device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1)) &&
        Call("DrawIndexedPrimitive", device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 3, 3, 0, 1)) &&
        Call("DrawPrimitiveUP", device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, up, sizeof(Vertex))) &&
        Call("DrawIndexedPrimitiveUP", device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, up_indices,
                                                                       D3DFMT_INDEX16, indexed_up, sizeof(Vertex)));
    const bool ended = Call("EndScene", device->EndScene());
    if (!drawn || !ended) return false;
    std::vector<BYTE> pixels;
    if (!ReadBack(back.value, pixels) || !Call("Present", device->Present(nullptr, nullptr, nullptr, nullptr)) ||
        !WriteNew(output, pixels)) return false;
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || (std::wcscmp(argv[1], L"--baseline") && std::wcscmp(argv[1], L"--facade"))) {
        std::fprintf(stderr, "usage: fixture (--baseline|--facade) new-output.ppm\n"); return 2;
    }
    const bool facade_mode = std::wcscmp(argv[1], L"--facade") == 0;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderD3D8DeviceFacadeFixture";
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = instance; cls.lpszClassName = name;
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(name, L"T3Render D3D8 device facade", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 300, 300, nullptr, nullptr, instance, nullptr);
    if (!window) { UnregisterClassW(name, instance); return 1; }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    const bool ok = Render(window, facade_mode, argv[2]);
    DestroyWindow(window);
    UnregisterClassW(name, instance);
    std::printf("device_facade_fixture=%s mode=%s\n", ok ? "PASS" : "FAIL", facade_mode ? "facade" : "baseline");
    return ok ? 0 : 1;
}
