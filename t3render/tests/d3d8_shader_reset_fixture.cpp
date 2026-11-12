// Synthetic D3D8 Reset experiment; never opens TDS or game assets.
// Reuse the shader, declaration and PPM helpers from the declaration fixture.
#define wmain unused_declaration_fixture_wmain
#include "d3d8_declaration_fixture.cpp"
#undef wmain

namespace {
// vs_1_1: mov oPos, v0; mov oD0, c0. The fixture's red vertices become
// green only when this program and its constant are actually applied.
const DWORD kResetProgram[] = {
    D3DVS_VERSION(1, 1), D3DSIO_MOV,
    0x80000000u | D3DSPR_RASTOUT | D3DSP_WRITEMASK_ALL | D3DSRO_POSITION,
    0x80000000u | D3DSPR_INPUT | D3DSP_NOSWIZZLE | D3DVSDE_POSITION,
    D3DSIO_MOV, 0x80000000u | D3DSPR_ATTROUT | D3DSP_WRITEMASK_ALL,
    0x80000000u | D3DSPR_CONST | D3DSP_NOSWIZZLE, D3DSIO_END
};
unsigned long Bits(HRESULT hr) { return static_cast<unsigned long>(static_cast<std::uint32_t>(hr)); }
void Event(const char* name, unsigned phase, HRESULT hr) {
    std::printf("reset-json {\"event\":\"%s\",\"phase\":%u,\"hr\":\"0x%08lx\"}\n", name, phase, Bits(hr));
}

void ProbeBinding(IDirect3DDevice8* device, unsigned phase, DWORD shader) {
    DWORD value = 0;
    const HRESULT hr = device->GetVertexShader(&value);
    std::printf("reset-json {\"event\":\"binding_before\",\"phase\":%u,\"hr\":\"0x%08lx\",\"kind\":",
                phase, Bits(hr));
    if (FAILED(hr)) std::printf("null,\"other\":null}\n");
    else if (value == shader) std::printf("\"handle\",\"other\":null}\n");
    else std::printf("\"other\",\"other\":%lu}\n", static_cast<unsigned long>(value));
}

void ProbeWords(IDirect3DDevice8* device, unsigned phase, DWORD shader, bool function) {
    const char* name = function ? "function" : "declaration";
    auto get = [&](void* data, DWORD* bytes) -> HRESULT {
        return function ? device->GetVertexShaderFunction(shader, data, bytes)
                        : device->GetVertexShaderDeclaration(shader, data, bytes);
    };
    DWORD size = 0;
    const HRESULT query = get(nullptr, &size);
    std::printf("reset-json {\"event\":\"%s_size\",\"phase\":%u,\"hr\":\"0x%08lx\",\"size\":", name, phase, Bits(query));
    if (FAILED(query)) std::printf("null}\n");
    else std::printf("%lu}\n", static_cast<unsigned long>(size));
    constexpr DWORD limit = 1024 * sizeof(DWORD);
    if (FAILED(query) || !size || size > limit || size % sizeof(DWORD)) return;
    DWORD first[1025], second[1025];
    for (auto& word : first) word = 0xa5a5a5a5u;
    for (auto& word : second) word = 0x5a5a5a5au;
    DWORD returned = size;
    const HRESULT copy = get(first, &returned);
    bool tail_ok = true;
    for (DWORD n = size / sizeof(DWORD); n < 1025; ++n) tail_ok = tail_ok && first[n] == 0xa5a5a5a5u;
    bool second_attempted = false, verify_attempted = false, reads_match = false;
    DWORD second_returned = size, verify_size = 0;
    HRESULT copy2 = E_FAIL, verify = E_FAIL;
    if (SUCCEEDED(copy) && returned == size && tail_ok) {
        second_attempted = true;
        copy2 = get(second, &second_returned);
        for (DWORD n = size / sizeof(DWORD); n < 1025; ++n) tail_ok = tail_ok && second[n] == 0x5a5a5a5au;
        if (SUCCEEDED(copy2) && second_returned == size && tail_ok) {
            reads_match = true;
            for (DWORD n = 0; n < size / sizeof(DWORD); ++n) reads_match = reads_match && first[n] == second[n];
            if (reads_match) {
                verify_attempted = true;
                verify = get(nullptr, &verify_size);
            }
        }
    }
    const bool complete = SUCCEEDED(copy) && returned == size && second_attempted && SUCCEEDED(copy2) &&
        second_returned == size && tail_ok && reads_match && verify_attempted && SUCCEEDED(verify) && verify_size == size;
    std::printf("reset-json {\"event\":\"%s\",\"phase\":%u,\"hr\":\"0x%08lx\","
                "\"requested_bytes\":%lu,\"returned_bytes\":%lu,\"second_hr\":", name, phase, Bits(copy),
                static_cast<unsigned long>(size), static_cast<unsigned long>(returned));
    if (second_attempted) std::printf("\"0x%08lx\",\"second_returned_bytes\":%lu", Bits(copy2), static_cast<unsigned long>(second_returned));
    else std::printf("null,\"second_returned_bytes\":null");
    std::printf(",\"verify_hr\":");
    if (verify_attempted) {
        std::printf("\"0x%08lx\",\"verify_size\":", Bits(verify));
        if (SUCCEEDED(verify)) std::printf("%lu", static_cast<unsigned long>(verify_size)); else std::printf("null");
    } else std::printf("null,\"verify_size\":null");
    std::printf(",\"tail_intact\":%s,\"reads_match\":", tail_ok ? "true" : "false");
    if (second_attempted && SUCCEEDED(copy2) && second_returned == size && tail_ok)
        std::printf(reads_match ? "true" : "false");
    else std::printf("null");
    std::printf(",\"words\":");
    if (!complete) { std::printf("null}\n"); return; }
    std::printf("[");
    for (DWORD n = 0; n < size / sizeof(DWORD); ++n)
        std::printf("%s%lu", n ? "," : "", static_cast<unsigned long>(first[n]));
    std::printf("]}\n");
}

bool PrepareFixedState(IDirect3DDevice8* device) {
    D3DMATRIX identity{}; identity._11 = identity._22 = identity._33 = identity._44 = 1;
    return Check(device->SetTransform(D3DTS_WORLD, &identity), "SetWorld") &&
        Check(device->SetTransform(D3DTS_VIEW, &identity), "SetView") &&
        Check(device->SetTransform(D3DTS_PROJECTION, &identity), "SetProjection") &&
        Check(device->SetRenderState(D3DRS_LIGHTING, FALSE), "Lighting") &&
        Check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "Cull") &&
        Check(device->SetRenderState(D3DRS_ZENABLE, FALSE), "Depth") &&
        Check(device->SetTexture(0, nullptr), "Texture") &&
        Check(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), "ColorOp") &&
        Check(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), "ColorArg") &&
        Check(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE), "Stage1") &&
        Check(device->SetPixelShader(0), "PixelShader");
}

bool DrawPhase(IDirect3DDevice8* device, unsigned phase, DWORD shader, const wchar_t* path) {
    ProbeBinding(device, phase, shader);
    ProbeWords(device, phase, shader, false);
    ProbeWords(device, phase, shader, true);
    const HRESULT set = device->SetVertexShader(shader);
    Event("set_shader", phase, set);
    const bool original = SUCCEEDED(set);
    if (!original) {
        const HRESULT fallback = device->SetVertexShader(kFvf);
        Event("fallback_fvf", phase, fallback);
        if (FAILED(fallback)) return false;
    }
    DWORD bound = 0;
    const HRESULT get = device->GetVertexShader(&bound);
    std::printf("reset-json {\"event\":\"binding_after\",\"phase\":%u,\"hr\":\"0x%08lx\","
                "\"original\":%s,\"matches_selected\":%s}\n", phase, Bits(get), original ? "true" : "false",
                SUCCEEDED(get) && bound == (original ? shader : kFvf) ? "true" : "false");
    if (FAILED(get) || bound != (original ? shader : kFvf) || !PrepareFixedState(device)) return false;
    const float green[] = {0, 1, 0, 1};
    if (!Check(device->SetVertexShaderConstant(0, green, 1), "ShaderConstant")) return false;
    const Vertex triangle[] = {{-.8f,-.8f,.5f,0xffff0000}, {0,.8f,.5f,0xffff0000}, {.8f,-.8f,.5f,0xffff0000}};
    if (!Check(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff000000, 1, 0), "Clear") ||
        !Check(device->BeginScene(), "BeginScene") ||
        !Check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, triangle, sizeof(Vertex)), "Draw") ||
        !Check(device->EndScene(), "EndScene") || !SaveImage(device, path) ||
        !Check(device->Present(nullptr, nullptr, nullptr, nullptr), "Present")) return false;
    return true;
}

bool ResetRender(HWND window, const wchar_t* const* paths, bool failed_first) {
    ComPtr<IDirect3D8> factory;
    factory.value = Direct3DCreate8(D3D_SDK_VERSION);
    D3DDISPLAYMODE display{};
    if (!factory.value || !Check(factory->GetAdapterDisplayMode(0, &display), "GetAdapterDisplayMode")) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = kSize; pp.BackBufferHeight = kSize; pp.BackBufferFormat = display.Format;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    pp.Windowed = TRUE; pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    ComPtr<IDirect3DDevice8> device;
    if (!Check(factory->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                     &pp, device.put()), "CreateDevice") || !device.value) return false;
    DWORD shader = 0;
    const HRESULT create = device->CreateVertexShader(kDeclaration, kResetProgram, &shader, D3DUSAGE_SOFTWAREPROCESSING);
    Event("create_shader", 0, create);
    if (FAILED(create) || !shader) return false;
    std::printf("shader-handle %08lx\n", static_cast<unsigned long>(shader));
    if (!DrawPhase(device.value, 0, shader, paths[0])) return false;
    if (failed_first) {
        D3DPRESENT_PARAMETERS invalid = pp;
        invalid.SwapEffect = static_cast<D3DSWAPEFFECT>(0); // no large allocations or display changes
        const HRESULT failed = device->Reset(&invalid);
        Event("failed_reset", 1, failed);
        // A failed D3D Reset can leave the device lost. No getters, rendering or
        // resource calls are safe to assume until a successful recovery Reset.
        D3DPRESENT_PARAMETERS recovery = pp;
        const HRESULT recovered = device->Reset(&recovery);
        Event("recovery_reset", 1, recovered);
        if (FAILED(recovered)) return false;
    } else {
        D3DPRESENT_PARAMETERS first = pp;
        const HRESULT succeeded = device->Reset(&first);
        Event("first_reset", 1, succeeded);
        if (FAILED(succeeded)) return false;
    }
    if (!DrawPhase(device.value, 1, shader, paths[1])) return false;
    D3DPRESENT_PARAMETERS valid = pp;
    const HRESULT succeeded = device->Reset(&valid);
    Event("successful_reset", 2, succeeded);
    if (FAILED(succeeded) || !DrawPhase(device.value, 2, shader, paths[2])) return false;
    const HRESULT unbind = device->SetVertexShader(kFvf);
    Event("unbind_shader", 2, unbind);
    if (FAILED(unbind)) return false;
    const HRESULT remove = device->DeleteVertexShader(shader);
    Event("delete_shader", 2, remove);
    std::puts("reset-fixture result=PASS");
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 9 || std::wcscmp(argv[1], L"--out0") || std::wcscmp(argv[3], L"--out1") ||
        std::wcscmp(argv[5], L"--out2") || !argv[2][0] || !argv[4][0] || !argv[6][0] ||
        std::wcscmp(argv[7], L"--scenario") ||
        (std::wcscmp(argv[8], L"recovery") && std::wcscmp(argv[8], L"valid-only"))) return 2;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderShaderResetFixture";
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = instance; cls.lpszClassName = name;
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(name, name, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                kSize, kSize, nullptr, nullptr, instance, nullptr);
    if (!window) { UnregisterClassW(name, instance); return 1; }
    ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
    const wchar_t* paths[] = {argv[2], argv[4], argv[6]};
    const bool ok = ResetRender(window, paths, !std::wcscmp(argv[8], L"recovery"));
    DestroyWindow(window); UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
