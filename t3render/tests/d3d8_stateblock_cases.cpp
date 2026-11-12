// Controlled per-case variant of the state-block fixture. This does not change
// the original fixture or the production proxy. See README_stateblock_experiment.
#define wmain unused_combined_stateblock_fixture_wmain
#include "d3d8_stateblock_buffer_fixture.cpp"
#undef wmain

namespace {
bool RenderCase(HWND window, const wchar_t* selected, const wchar_t* output) {
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
        !CreatePair(device.value, retained, 0xffff0000, true) || !Bind(device.value, a)) return false;
    DWORD token = 0;
    bool token_live = false;
    const bool lifetime = !std::wcscmp(selected, L"lifetime") || !std::wcscmp(selected, L"reacquire");
    if (!std::wcscmp(selected, L"control")) {
        if (!BoundPair(device.value, a, "control")) return false;
    } else if (!std::wcscmp(selected, L"create")) {
        if (!Check(device->CreateStateBlock(D3DSBT_ALL, &token), "CreateStateBlock") ||
            !BoundPair(device.value, a, "create")) return false;
        token_live = true;
    } else if (!std::wcscmp(selected, L"record")) {
        if (!Check(device->BeginStateBlock(), "BeginStateBlock") || !Bind(device.value, b) ||
            !Check(device->EndStateBlock(&token), "EndStateBlock") ||
            !BoundPair(device.value, a, "record")) return false;
        token_live = true;
    } else if (!std::wcscmp(selected, L"apply") || !std::wcscmp(selected, L"capture") ||
               !std::wcscmp(selected, L"delete")) {
        if (!Check(device->CreateStateBlock(D3DSBT_ALL, &token), "CreateStateBlock")) return false;
        token_live = true;
        if (!std::wcscmp(selected, L"apply")) {
            if (!Bind(device.value, b) || !Check(device->ApplyStateBlock(token), "ApplyStateBlock") ||
                !BoundPair(device.value, a, "apply")) return false;
        } else if (!std::wcscmp(selected, L"capture")) {
            if (!Bind(device.value, b) || !Check(device->CaptureStateBlock(token), "CaptureStateBlock") ||
                !Bind(device.value, a) || !Check(device->ApplyStateBlock(token), "ApplyStateBlock(captured)") ||
                !BoundPair(device.value, b, "capture") || !Bind(device.value, a)) return false;
        } else {
            if (!Check(device->DeleteStateBlock(token), "DeleteStateBlock") ||
                !BoundPair(device.value, a, "delete")) return false;
            token_live = false;
        }
    } else if (lifetime) {
        if (!Check(device->BeginStateBlock(), "BeginStateBlock(retained)") || !Bind(device.value, retained) ||
            !Check(device->EndStateBlock(&token), "EndStateBlock(retained)")) return false;
        token_live = true;
        retained.vertex.reset(); retained.index.reset();
        if (!Check(device->ApplyStateBlock(token), "ApplyStateBlock(retained)")) return false;
        if (!std::wcscmp(selected, L"reacquire") && !BoundOversized(device.value)) return false;
        // In lifetime mode, no application getter reacquires the native buffer
        // before the draw. The internal capture getter still finds the binding.
        std::printf("state case_%ls=PASS\n", selected);
    } else return false;

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
    if (token_live && !Check(device->DeleteStateBlock(token), "DeleteStateBlock(cleanup)")) return false;
    std::printf("state result=PASS\n");
    return true;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 5 || std::wcscmp(argv[1], L"--case") || !argv[2][0] ||
        std::wcscmp(argv[3], L"--output") || !argv[4][0]) return 2;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* name = L"T3RenderStateBlockCases";
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = instance; cls.lpszClassName = name;
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(name, name, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                256, 256, nullptr, nullptr, instance, nullptr);
    if (!window) { UnregisterClassW(name, instance); return 1; }
    ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
    const bool ok = RenderCase(window, argv[2], argv[4]);
    DestroyWindow(window); UnregisterClassW(name, instance);
    return ok ? 0 : 1;
}
